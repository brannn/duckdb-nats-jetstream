#include "nats_scan.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/vector.hpp"
#include "utf8proc_wrapper.hpp"
#include "yyjson.hpp"
#include <nats/nats.h>
#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor_database.h>
#include <google/protobuf/util/json_util.h>
#include <google/protobuf/any.pb.h>
#include <google/protobuf/duration.pb.h>
#include <google/protobuf/empty.pb.h>
#include <google/protobuf/field_mask.pb.h>
#include <google/protobuf/struct.pb.h>
#include <google/protobuf/timestamp.pb.h>
#include <google/protobuf/wrappers.pb.h>
#include <cmath>
#include <filesystem>

// Windows defines GetMessage as a macro (GetMessageA/GetMessageW)
// This conflicts with protobuf's Reflection::GetMessage() method
#ifdef GetMessage
#undef GetMessage
#endif

using namespace duckdb_yyjson;
using namespace google::protobuf;
using namespace google::protobuf::compiler;

namespace duckdb {

static constexpr int NATS_TIMEOUT_MS = 5000;

class ProtobufErrorCollector : public MultiFileErrorCollector {
public:
    // Protobuf 3.21.x and earlier use AddError with std::string
    // Protobuf 3.22.x and later use RecordError with absl::string_view
#if GOOGLE_PROTOBUF_VERSION >= 3022000
    void RecordError(absl::string_view filename, int line, int column, absl::string_view message) override {
        errors.push_back(string(filename) + ":" + std::to_string(line) + ":" + std::to_string(column) + ": " + string(message));
    }
#else
    void AddError(const std::string& filename, int line, int column, const std::string& message) override {
        errors.push_back(filename + ":" + std::to_string(line) + ":" + std::to_string(column) + ": " + message);
    }
#endif

    string GetErrors() const {
        string result;
        for (const auto &err : errors) {
            result += err + "\n";
        }
        return result;
    }

    bool HasErrors() const {
        return !errors.empty();
    }

private:
    vector<string> errors;
};

// A .proto file with its imports, which resolve against the file's directory and protobuf's well-known types
struct ProtoSchema {
    DiskSourceTree source_tree;
    ProtobufErrorCollector errors;
    unique_ptr<SourceTreeDescriptorDatabase> source_db;
    unique_ptr<DescriptorPoolDatabase> well_known_db;
    unique_ptr<MergedDescriptorDatabase> merged_db;
    unique_ptr<DescriptorPool> pool;

    explicit ProtoSchema(const string &directory) {
        // Referencing the well-known types links their descriptors into the generated pool
        google::protobuf::Any::descriptor();
        google::protobuf::Duration::descriptor();
        google::protobuf::Empty::descriptor();
        google::protobuf::FieldMask::descriptor();
        google::protobuf::Struct::descriptor();
        google::protobuf::Timestamp::descriptor();
        google::protobuf::DoubleValue::descriptor();

        source_tree.MapPath("", directory);
        source_db = make_uniq<SourceTreeDescriptorDatabase>(&source_tree);
        source_db->RecordErrorsTo(&errors);
        well_known_db = make_uniq<DescriptorPoolDatabase>(*DescriptorPool::generated_pool());
        merged_db = make_uniq<MergedDescriptorDatabase>(well_known_db.get(), source_db.get());
        pool = make_uniq<DescriptorPool>(merged_db.get(), source_db->GetValidationErrorCollector());
        pool->EnforceWeakDependencies(true);
    }
};

struct YyjsonDocDeleter {
    void operator()(yyjson_doc *doc) const {
        yyjson_doc_free(doc);
    }
    void operator()(yyjson_mut_doc *doc) const {
        yyjson_mut_doc_free(doc);
    }
};
using YyjsonDoc = unique_ptr<yyjson_doc, YyjsonDocDeleter>;
using YyjsonMutDoc = unique_ptr<yyjson_mut_doc, YyjsonDocDeleter>;

struct NatsScanBindData : public TableFunctionData {
    string stream_name;
    string subject_filter;
    string nats_url = "nats://localhost:4222";
    uint64_t start_seq = 0;
    uint64_t end_seq = NumericLimits<uint64_t>::Maximum();
    bool has_start_time = false;
    bool has_end_time = false;
    int64_t start_time_us = 0;
    int64_t end_time_us = 0;

    // Each json_extract entry split on '.', for nested lookups
    vector<string> json_fields;
    vector<vector<string>> json_paths;

    string proto_file;
    string proto_message;
    vector<string> proto_fields;
    // Fields along each proto_extract path; the last one is the extracted field
    vector<vector<const FieldDescriptor *>> proto_paths;

    // Must outlive the query — proto_descriptor and proto_paths are owned by the schema's pool
    shared_ptr<ProtoSchema> proto_schema;
    const Descriptor* proto_descriptor = nullptr;
};

static vector<string> SplitFieldPath(const string& field_path) {
    vector<string> parts;
    size_t start = 0;
    size_t end = field_path.find('.');
    while (end != string::npos) {
        parts.push_back(field_path.substr(start, end - start));
        start = end + 1;
        end = field_path.find('.', start);
    }
    parts.push_back(field_path.substr(start));
    return parts;
}

// Helper function to map protobuf field type to DuckDB LogicalType
static LogicalType ProtobufTypeToDuckDBType(const FieldDescriptor* field) {
    // Repeated and map fields are serialized as JSON
    if (field->is_repeated()) {
        return LogicalType(LogicalTypeId::VARCHAR);
    }
    switch (field->type()) {
        case FieldDescriptor::TYPE_STRING:
            return LogicalType(LogicalTypeId::VARCHAR);
        case FieldDescriptor::TYPE_BYTES:
            return LogicalType(LogicalTypeId::BLOB);
        case FieldDescriptor::TYPE_INT32:
        case FieldDescriptor::TYPE_SINT32:
        case FieldDescriptor::TYPE_SFIXED32:
            return LogicalType(LogicalTypeId::INTEGER);
        case FieldDescriptor::TYPE_INT64:
        case FieldDescriptor::TYPE_SINT64:
        case FieldDescriptor::TYPE_SFIXED64:
            return LogicalType(LogicalTypeId::BIGINT);
        case FieldDescriptor::TYPE_UINT32:
        case FieldDescriptor::TYPE_FIXED32:
            return LogicalType(LogicalTypeId::UINTEGER);
        case FieldDescriptor::TYPE_UINT64:
        case FieldDescriptor::TYPE_FIXED64:
            return LogicalType(LogicalTypeId::UBIGINT);
        case FieldDescriptor::TYPE_FLOAT:
            return LogicalType(LogicalTypeId::FLOAT);
        case FieldDescriptor::TYPE_DOUBLE:
            return LogicalType(LogicalTypeId::DOUBLE);
        case FieldDescriptor::TYPE_BOOL:
            return LogicalType(LogicalTypeId::BOOLEAN);
        case FieldDescriptor::TYPE_ENUM:
            // Enums are represented as VARCHAR with the enum name
            return LogicalType(LogicalTypeId::VARCHAR);
        default:
            // Messages are serialized as JSON objects
            return LogicalType(LogicalTypeId::VARCHAR);
    }
}

static vector<const FieldDescriptor *> ResolveProtoPath(const Descriptor *root, const string &field_path) {
    auto path_parts = SplitFieldPath(field_path);
    vector<const FieldDescriptor *> fields;
    const Descriptor *current_desc = root;
    for (idx_t i = 0; i < path_parts.size(); i++) {
        const FieldDescriptor *field = current_desc->FindFieldByName(path_parts[i]);
        if (!field) {
            throw BinderException("Field '%s' not found in message type '%s' (field path: %s)", path_parts[i],
                                  string(current_desc->name()), field_path);
        }
        if (i + 1 < path_parts.size()) {
            if (field->cpp_type() != FieldDescriptor::CPPTYPE_MESSAGE) {
                throw BinderException("Field '%s' is not a message type, cannot navigate to '%s' (field path: %s)",
                                      path_parts[i], path_parts[i + 1], field_path);
            }
            if (field->is_repeated()) {
                throw BinderException("Field '%s' is repeated, cannot navigate to '%s' (field path: %s); "
                                      "extract '%s' to get it as a JSON array",
                                      path_parts[i], path_parts[i + 1], field_path, path_parts[i]);
            }
            current_desc = field->message_type();
        }
        fields.push_back(field);
    }
    return fields;
}

static string GetStringOption(const Value &value, const string &option) {
    if (value.IsNull()) {
        throw BinderException("nats_scan: %s cannot be NULL", option);
    }
    return StringValue::Get(value);
}

static vector<string> GetFieldListOption(const Value &value, const string &option) {
    if (value.IsNull()) {
        throw BinderException("nats_scan: %s cannot be NULL", option);
    }
    vector<string> fields;
    for (auto &child : ListValue::GetChildren(value)) {
        if (child.IsNull()) {
            throw BinderException("nats_scan: %s cannot contain NULL", option);
        }
        auto field = StringValue::Get(child);
        if (field.empty()) {
            throw BinderException("nats_scan: %s cannot contain an empty field name", option);
        }
        fields.push_back(std::move(field));
    }
    return fields;
}

// Field columns must not shadow the base columns or each other; repeats get a numeric suffix
static void AddColumn(vector<Identifier> &names, case_insensitive_set_t &used, const string &name) {
    string column_name = name;
    for (idx_t suffix = 1; used.count(column_name) > 0; suffix++) {
        column_name = name + "_" + std::to_string(suffix);
    }
    used.insert(column_name);
    names.emplace_back(column_name);
}

static unique_ptr<FunctionData> NatsScanBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<Identifier> &names) {
    if (!Settings::Get<EnableExternalAccessSetting>(context)) {
        throw PermissionException("nats_scan is disabled because enable_external_access is false");
    }

    auto bind_data = make_uniq<NatsScanBindData>();
    bind_data->stream_name = GetStringOption(input.inputs[0], "stream");
    if (bind_data->stream_name.empty()) {
        throw BinderException("nats_scan: stream cannot be empty");
    }

    bool has_seq_range = false;
    for (auto &kv : input.named_parameters) {
        if (kv.first == "subject") {
            bind_data->subject_filter = GetStringOption(kv.second, "subject");
        } else if (kv.first == "url") {
            bind_data->nats_url = GetStringOption(kv.second, "url");
        } else if (kv.first == "start_seq") {
            if (kv.second.IsNull()) {
                throw BinderException("nats_scan: start_seq cannot be NULL");
            }
            bind_data->start_seq = UBigIntValue::Get(kv.second);
            has_seq_range = true;
        } else if (kv.first == "end_seq") {
            if (kv.second.IsNull()) {
                throw BinderException("nats_scan: end_seq cannot be NULL");
            }
            bind_data->end_seq = UBigIntValue::Get(kv.second);
            has_seq_range = true;
        } else if (kv.first == "start_time") {
            if (kv.second.IsNull()) {
                throw BinderException("nats_scan: start_time cannot be NULL");
            }
            bind_data->start_time_us = TimestampValue::Get(kv.second).value;
            bind_data->has_start_time = true;
        } else if (kv.first == "end_time") {
            if (kv.second.IsNull()) {
                throw BinderException("nats_scan: end_time cannot be NULL");
            }
            bind_data->end_time_us = TimestampValue::Get(kv.second).value;
            bind_data->has_end_time = true;
        } else if (kv.first == "json_extract") {
            bind_data->json_fields = GetFieldListOption(kv.second, "json_extract");
        } else if (kv.first == "proto_file") {
            bind_data->proto_file = GetStringOption(kv.second, "proto_file");
        } else if (kv.first == "proto_message") {
            bind_data->proto_message = GetStringOption(kv.second, "proto_message");
        } else if (kv.first == "proto_extract") {
            bind_data->proto_fields = GetFieldListOption(kv.second, "proto_extract");
        }
    }

    // The filter is sent to the server inside a JSON request, so it must be a plain subject
    for (char c : bind_data->subject_filter) {
        if (c == '"' || c == '\\' || StringUtil::CharacterIsSpace(c)) {
            throw BinderException("nats_scan: invalid subject filter '%s'", bind_data->subject_filter);
        }
    }

    if (has_seq_range && (bind_data->has_start_time || bind_data->has_end_time)) {
        throw BinderException("Cannot mix sequence-based (start_seq/end_seq) and time-based (start_time/end_time) parameters");
    }

    auto &json_fields = bind_data->json_fields;
    auto &proto_fields = bind_data->proto_fields;

    if (!json_fields.empty() && !proto_fields.empty()) {
        throw BinderException("Cannot use both json_extract and proto_extract parameters");
    }

    if (!proto_fields.empty()) {
        if (bind_data->proto_file.empty()) {
            throw BinderException("proto_file parameter is required when using proto_extract");
        }
        if (bind_data->proto_message.empty()) {
            throw BinderException("proto_message parameter is required when using proto_extract");
        }

        std::filesystem::path proto_path(bind_data->proto_file);
        string proto_dir = proto_path.parent_path().string();
        string proto_filename = proto_path.filename().string();

        if (proto_dir.empty()) {
            proto_dir = ".";
        }

        auto schema = make_shared_ptr<ProtoSchema>(proto_dir);
        const FileDescriptor* file_desc = schema->pool->FindFileByName(proto_filename);
        if (!file_desc) {
            string error_msg = "Failed to import protobuf schema file: " + bind_data->proto_file;
            if (schema->errors.HasErrors()) {
                error_msg += "\n" + schema->errors.GetErrors();
            }
            throw IOException(error_msg);
        }

        const Descriptor *descriptor = file_desc->FindMessageTypeByName(bind_data->proto_message);
        if (!descriptor) {
            throw BinderException("Message type '%s' not found in %s", bind_data->proto_message, bind_data->proto_file);
        }

        for (const auto &field_path : proto_fields) {
            bind_data->proto_paths.push_back(ResolveProtoPath(descriptor, field_path));
        }

        bind_data->proto_schema = schema;
        bind_data->proto_descriptor = descriptor;
    }

    for (const auto &field : json_fields) {
        bind_data->json_paths.push_back(SplitFieldPath(field));
    }

    case_insensitive_set_t used_names;
    AddColumn(names, used_names, "stream");
    return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
    AddColumn(names, used_names, "subject");
    return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
    AddColumn(names, used_names, "seq");
    return_types.emplace_back(LogicalType(LogicalTypeId::UBIGINT));
    AddColumn(names, used_names, "ts_nats");
    return_types.emplace_back(LogicalType(LogicalTypeId::TIMESTAMP));

    // BLOB unless json_extract is specified, to avoid validation errors on binary data
    AddColumn(names, used_names, "payload");
    if (json_fields.empty()) {
        return_types.emplace_back(LogicalType(LogicalTypeId::BLOB));
    } else {
        return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
    }

    // Field columns: dot notation -> underscores
    for (const auto &field : json_fields) {
        string column_name = field;
        std::replace(column_name.begin(), column_name.end(), '.', '_');
        AddColumn(names, used_names, column_name);
        return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
    }
    for (idx_t i = 0; i < proto_fields.size(); i++) {
        string column_name = proto_fields[i];
        std::replace(column_name.begin(), column_name.end(), '.', '_');
        AddColumn(names, used_names, column_name);
        return_types.emplace_back(ProtobufTypeToDuckDBType(bind_data->proto_paths[i].back()));
    }

    return std::move(bind_data);
}

// A stored message, from either a direct get (msg owns the buffers) or a STREAM.MSG.GET reply (decoded into the strings)
struct NatsStoredMsg {
    uint64_t seq = 0;
    int64_t time_ns = 0;
    const char *subject = "";
    idx_t subject_len = 0;
    const char *data = "";
    idx_t data_len = 0;

    natsMsg *msg = nullptr;
    string subject_buf;
    string data_buf;

    void Clear() {
        if (msg != nullptr) {
            natsMsg_Destroy(msg);
            msg = nullptr;
        }
    }

    ~NatsStoredMsg() {
        Clear();
    }
};

struct NatsScanGlobalState : public GlobalTableFunctionState {
    natsConnection *conn = nullptr;
    jsCtx *js = nullptr;
    jsStreamInfo *stream_info = nullptr;
    bool allow_direct = false;
    string msg_get_subject;
    uint64_t next_seq = 1;
    uint64_t last_seq = 0;
    bool done = false;

    shared_ptr<DynamicMessageFactory> proto_factory;
    const Message* proto_prototype = nullptr;
    util::JsonPrintOptions json_options;

    ~NatsScanGlobalState() {
        if (stream_info != nullptr) {
            jsStreamInfo_Destroy(stream_info);
            stream_info = nullptr;
        }
        if (js != nullptr) {
            jsCtx_Destroy(js);
            js = nullptr;
        }
        if (conn != nullptr) {
            natsConnection_Destroy(conn);
            conn = nullptr;
        }
    }

    idx_t MaxThreads() const override {
        return 1; // Single-threaded for now
    }
};

struct NatsScanLocalState : public LocalTableFunctionState {
};

static string NatsErrorText(natsStatus s) {
    const char *text = nats_GetLastError(nullptr);
    if (text == nullptr || *text == '\0') {
        return natsStatus_GetText(s);
    }
    // cnats prefixes the text with its source location: "(file.c:123): text"
    string result(text);
    auto pos = result.find("): ");
    if (!result.empty() && result[0] == '(' && pos != string::npos) {
        result = result.substr(pos + 3);
    }
    return result;
}

// Parses a STREAM.MSG.GET reply. Returns false if the server found no message.
static bool ParseMsgGetReply(natsMsg *reply, const string &stream_name, NatsStoredMsg &out) {
    YyjsonDoc doc(yyjson_read(natsMsg_GetData(reply), natsMsg_GetDataLength(reply), 0));
    yyjson_val *root = doc ? yyjson_doc_get_root(doc.get()) : nullptr;
    if (!yyjson_is_obj(root)) {
        throw IOException("nats_scan: invalid message get response from stream '%s'", stream_name);
    }
    yyjson_val *error = yyjson_obj_get(root, "error");
    if (error) {
        if (yyjson_get_int(yyjson_obj_get(error, "code")) == 404) {
            return false;
        }
        const char *description = yyjson_get_str(yyjson_obj_get(error, "description"));
        throw IOException("nats_scan: failed to read stream '%s': %s", stream_name,
                          string(description ? description : "unknown error"));
    }
    yyjson_val *message = yyjson_obj_get(root, "message");
    yyjson_val *subject = yyjson_obj_get(message, "subject");
    yyjson_val *seq = yyjson_obj_get(message, "seq");
    yyjson_val *time = yyjson_obj_get(message, "time");
    yyjson_val *data = yyjson_obj_get(message, "data");
    timestamp_ns_t time_ns;
    if (!yyjson_is_str(subject) || !yyjson_is_uint(seq) || !yyjson_is_str(time) ||
        Timestamp::TryConvertTimestamp(yyjson_get_str(time), yyjson_get_len(time), time_ns, true) !=
            TimestampCastResult::SUCCESS) {
        throw IOException("nats_scan: invalid message get response from stream '%s'", stream_name);
    }

    out.seq = yyjson_get_uint(seq);
    out.time_ns = Timestamp::GetEpochNanoSeconds(time_ns);
    out.subject_buf.assign(yyjson_get_str(subject), yyjson_get_len(subject));
    out.data_buf.clear();
    if (yyjson_is_str(data) && yyjson_get_len(data) > 0) {
        string_t encoded(yyjson_get_str(data), UnsafeNumericCast<uint32_t>(yyjson_get_len(data)));
        out.data_buf.resize(Blob::FromBase64Size(encoded));
        Blob::FromBase64(encoded, data_ptr_cast(&out.data_buf[0]), out.data_buf.size());
    }
    out.subject = out.subject_buf.c_str();
    out.subject_len = out.subject_buf.size();
    out.data = out.data_buf.c_str();
    out.data_len = out.data_buf.size();
    return true;
}

// Loads the first message at or after seq whose subject matches filter ('>' matches all).
// Returns false if there is none.
static bool LoadNextMsg(NatsScanGlobalState &state, const string &stream_name, uint64_t seq, const string &filter,
                        NatsStoredMsg &out) {
    out.Clear();
    if (state.allow_direct) {
        jsDirectGetMsgOptions opts;
        jsDirectGetMsgOptions_Init(&opts);
        opts.Sequence = seq;
        opts.NextBySubject = filter.c_str();

        natsStatus s = js_DirectGetMsg(&out.msg, state.js, stream_name.c_str(), nullptr, &opts);
        if (s == NATS_NOT_FOUND) {
            return false;
        }
        if (s != NATS_OK) {
            throw IOException("nats_scan: failed to read stream '%s' at sequence %d: %s", stream_name, seq,
                              NatsErrorText(s));
        }
        out.seq = natsMsg_GetSequence(out.msg);
        out.time_ns = natsMsg_GetTime(out.msg);
        out.subject = natsMsg_GetSubject(out.msg);
        out.subject_len = strlen(out.subject);
        out.data = natsMsg_GetData(out.msg);
        out.data_len = UnsafeNumericCast<idx_t>(natsMsg_GetDataLength(out.msg));
        if (out.data == nullptr) {
            out.data = "";
        }
        return true;
    }

    // Streams without allow_direct only answer the JetStream API, which js_GetMsg exposes without next_by_subj
    string request = "{\"seq\":" + std::to_string(seq) + ",\"next_by_subj\":\"" + filter + "\"}";
    natsMsg *reply = nullptr;
    natsStatus s = natsConnection_Request(&reply, state.conn, state.msg_get_subject.c_str(), request.c_str(),
                                          UnsafeNumericCast<int>(request.size()), NATS_TIMEOUT_MS);
    if (s != NATS_OK) {
        throw IOException("nats_scan: failed to read stream '%s' at sequence %d: %s", stream_name, seq,
                          NatsErrorText(s));
    }
    try {
        bool found = ParseMsgGetReply(reply, stream_name, out);
        natsMsg_Destroy(reply);
        return found;
    } catch (...) {
        natsMsg_Destroy(reply);
        throw;
    }
}

// Binary search for the first message at or after the given time; sequence gaps are skipped by LoadNextMsg.
// Assumes message times increase with sequence, which holds unless the server's clock moved back. first_seq must be >= 1.
static bool FindFirstSeqAtTime(NatsScanGlobalState &state, const string &stream_name, int64_t time_us,
                               uint64_t first_seq, uint64_t last_seq, uint64_t &result) {
    NatsStoredMsg msg;
    bool found = false;
    uint64_t left = first_seq;
    uint64_t right = last_seq;
    while (left <= right) {
        uint64_t mid = left + (right - left) / 2;
        if (!LoadNextMsg(state, stream_name, mid, ">", msg)) {
            right = mid - 1;
            continue;
        }
        if (msg.time_ns / 1000 >= time_us) {
            result = msg.seq;
            found = true;
            right = mid - 1;
        } else {
            left = msg.seq + 1;
        }
    }
    return found;
}

static unique_ptr<GlobalTableFunctionState> NatsScanInitGlobal(ClientContext &context,
                                                                 TableFunctionInitInput &input) {
    auto &bind_data = input.bind_data->Cast<NatsScanBindData>();
    auto state = make_uniq<NatsScanGlobalState>();

    if (bind_data.proto_descriptor != nullptr) {
        state->proto_factory = make_shared_ptr<DynamicMessageFactory>();
        state->proto_prototype = state->proto_factory->GetPrototype(bind_data.proto_descriptor);
        state->json_options.preserve_proto_field_names = true;
        state->json_options.unquote_int64_if_possible = true;
    }

    // Connect to NATS and validate stream at init time (fail-fast on typos / unreachable server)
    natsOptions *opts = nullptr;
    natsStatus s = natsOptions_Create(&opts);
    if (s != NATS_OK) {
        throw IOException("Failed to create NATS options: %s", NatsErrorText(s));
    }

    s = natsOptions_SetTimeout(opts, NATS_TIMEOUT_MS);
    if (s == NATS_OK) {
        s = natsOptions_SetURL(opts, bind_data.nats_url.c_str());
    }
    if (s != NATS_OK) {
        natsOptions_Destroy(opts);
        throw IOException("Invalid NATS URL '%s': %s", bind_data.nats_url, NatsErrorText(s));
    }

    s = natsConnection_Connect(&state->conn, opts);
    natsOptions_Destroy(opts);
    if (s != NATS_OK) {
        throw IOException("Failed to connect to NATS at '%s': %s", bind_data.nats_url, NatsErrorText(s));
    }

    s = natsConnection_JetStream(&state->js, state->conn, nullptr);
    if (s != NATS_OK) {
        throw IOException("Failed to create JetStream context: %s", NatsErrorText(s));
    }

    s = js_GetStreamInfo(&state->stream_info, state->js, bind_data.stream_name.c_str(), nullptr, nullptr);
    if (s == NATS_NOT_FOUND) {
        throw IOException("Stream '%s' not found", bind_data.stream_name);
    }
    if (s != NATS_OK) {
        throw IOException("Failed to get stream info for '%s': %s", bind_data.stream_name, NatsErrorText(s));
    }

    state->allow_direct = state->stream_info->Config != nullptr && state->stream_info->Config->AllowDirect;
    state->msg_get_subject = "$JS.API.STREAM.MSG.GET." + bind_data.stream_name;

    // The scan stops at the stream's last message as of now, so it ends on a busy stream
    auto &stream_state = state->stream_info->State;
    if (stream_state.Msgs == 0) {
        state->done = true;
        return std::move(state);
    }
    state->next_seq = MaxValue<uint64_t>(MaxValue<uint64_t>(bind_data.start_seq, stream_state.FirstSeq), 1);
    state->last_seq = MinValue<uint64_t>(bind_data.end_seq, stream_state.LastSeq);

    if (bind_data.has_start_time) {
        uint64_t seq = 0;
        if (!FindFirstSeqAtTime(*state, bind_data.stream_name, bind_data.start_time_us, state->next_seq,
                                state->last_seq, seq)) {
            state->done = true;
            return std::move(state);
        }
        state->next_seq = seq;
    }

    return std::move(state);
}

static unique_ptr<LocalTableFunctionState> NatsScanInitLocal(ExecutionContext &context,
                                                               TableFunctionInitInput &input,
                                                               GlobalTableFunctionState *global_state) {
    return make_uniq<NatsScanLocalState>();
}

// Navigates a dot path through objects (and arrays, by index). A key containing dots is matched literally first.
static yyjson_val *LookupJsonPath(yyjson_val *root, const string &field, const vector<string> &path) {
    if (yyjson_is_obj(root)) {
        yyjson_val *val = yyjson_obj_getn(root, field.c_str(), field.size());
        if (val || path.size() == 1) {
            return val;
        }
    }
    yyjson_val *current = root;
    for (const auto &part : path) {
        if (yyjson_is_obj(current)) {
            current = yyjson_obj_getn(current, part.c_str(), part.size());
        } else if (yyjson_is_arr(current) && !part.empty() &&
                   std::all_of(part.begin(), part.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            current = part.size() <= 9 ? yyjson_arr_get(current, std::stoul(part)) : nullptr;
        } else {
            return nullptr;
        }
        if (!current) {
            return nullptr;
        }
    }
    return current;
}

static void WriteJsonValue(yyjson_val *val, VectorScatterWriter<string_t> &writer, idx_t row) {
    if (!val || yyjson_is_null(val)) {
        writer.SetInvalid(row);
    } else if (yyjson_is_str(val)) {
        writer[row] = string_t(yyjson_get_str(val), UnsafeNumericCast<uint32_t>(yyjson_get_len(val)));
    } else if (yyjson_is_raw(val)) {
        // Numbers are read as raw text, so they keep the payload's exact digits
        writer[row] = string_t(yyjson_get_raw(val), UnsafeNumericCast<uint32_t>(yyjson_get_len(val)));
    } else if (yyjson_is_bool(val)) {
        writer[row] = string_t(yyjson_get_bool(val) ? "true" : "false");
    } else {
        size_t len = 0;
        char *json_str = yyjson_val_write(val, YYJSON_WRITE_NOFLAG, &len);
        if (json_str) {
            writer[row] = string_t(json_str, UnsafeNumericCast<uint32_t>(len));
            free(json_str);
        } else {
            writer.SetInvalid(row);
        }
    }
}

// Converts one protobuf field value (an element when index >= 0) to JSON
static yyjson_mut_val *ProtoFieldToJson(yyjson_mut_doc *doc, const Message &message, const FieldDescriptor *field,
                                        int index, const util::JsonPrintOptions &json_options) {
    const Reflection *reflection = message.GetReflection();
#define PROTO_GET(TYPE)                                                                                                \
    (index < 0 ? reflection->Get##TYPE(message, field) : reflection->GetRepeated##TYPE(message, field, index))
    switch (field->cpp_type()) {
        case FieldDescriptor::CPPTYPE_INT32:
            return yyjson_mut_sint(doc, PROTO_GET(Int32));
        case FieldDescriptor::CPPTYPE_INT64:
            return yyjson_mut_sint(doc, PROTO_GET(Int64));
        case FieldDescriptor::CPPTYPE_UINT32:
            return yyjson_mut_uint(doc, PROTO_GET(UInt32));
        case FieldDescriptor::CPPTYPE_UINT64:
            return yyjson_mut_uint(doc, PROTO_GET(UInt64));
        case FieldDescriptor::CPPTYPE_BOOL:
            return yyjson_mut_bool(doc, PROTO_GET(Bool));
        case FieldDescriptor::CPPTYPE_FLOAT:
        case FieldDescriptor::CPPTYPE_DOUBLE: {
            // Shortest text that reads back to the same value, as DuckDB prints it
            string text;
            if (field->cpp_type() == FieldDescriptor::CPPTYPE_FLOAT) {
                float value = PROTO_GET(Float);
                text = std::isfinite(value) ? Value::FLOAT(value).ToString() : "";
            } else {
                double value = PROTO_GET(Double);
                text = std::isfinite(value) ? Value::DOUBLE(value).ToString() : "";
            }
            return text.empty() ? yyjson_mut_null(doc) : yyjson_mut_rawncpy(doc, text.c_str(), text.size());
        }
        case FieldDescriptor::CPPTYPE_ENUM: {
            auto name = PROTO_GET(Enum)->name();
            return yyjson_mut_strncpy(doc, name.data(), name.size());
        }
        case FieldDescriptor::CPPTYPE_STRING: {
            string value = PROTO_GET(String);
            if (field->type() == FieldDescriptor::TYPE_BYTES) {
                value = Blob::ToBase64(string_t(value));
            }
            return yyjson_mut_strncpy(doc, value.c_str(), value.size());
        }
        case FieldDescriptor::CPPTYPE_MESSAGE: {
            string json;
            if (!util::MessageToJsonString(PROTO_GET(Message), &json, json_options).ok()) {
                return yyjson_mut_null(doc);
            }
            return yyjson_mut_rawncpy(doc, json.c_str(), json.size());
        }
        default:
            return yyjson_mut_null(doc);
    }
#undef PROTO_GET
}

static string ProtoMapKeyToString(const Message &entry, const FieldDescriptor *key_field) {
    const Reflection *reflection = entry.GetReflection();
    switch (key_field->cpp_type()) {
        case FieldDescriptor::CPPTYPE_INT32:
            return std::to_string(reflection->GetInt32(entry, key_field));
        case FieldDescriptor::CPPTYPE_INT64:
            return std::to_string(reflection->GetInt64(entry, key_field));
        case FieldDescriptor::CPPTYPE_UINT32:
            return std::to_string(reflection->GetUInt32(entry, key_field));
        case FieldDescriptor::CPPTYPE_UINT64:
            return std::to_string(reflection->GetUInt64(entry, key_field));
        case FieldDescriptor::CPPTYPE_BOOL:
            return reflection->GetBool(entry, key_field) ? "true" : "false";
        default:
            return reflection->GetString(entry, key_field);
    }
}

// Repeated fields become JSON arrays and map fields JSON objects
static Value ProtoRepeatedToJson(const Message &message, const FieldDescriptor *field,
                                 const util::JsonPrintOptions &json_options) {
    YyjsonMutDoc doc(yyjson_mut_doc_new(nullptr));
    const Reflection *reflection = message.GetReflection();
    int count = reflection->FieldSize(message, field);
    yyjson_mut_val *root;
    if (field->is_map()) {
        root = yyjson_mut_obj(doc.get());
        const FieldDescriptor *key_field = field->message_type()->map_key();
        const FieldDescriptor *value_field = field->message_type()->map_value();
        for (int i = 0; i < count; i++) {
            const Message &entry = reflection->GetRepeatedMessage(message, field, i);
            string key = ProtoMapKeyToString(entry, key_field);
            yyjson_mut_obj_add(root, yyjson_mut_strncpy(doc.get(), key.c_str(), key.size()),
                               ProtoFieldToJson(doc.get(), entry, value_field, -1, json_options));
        }
    } else {
        root = yyjson_mut_arr(doc.get());
        for (int i = 0; i < count; i++) {
            yyjson_mut_arr_append(root, ProtoFieldToJson(doc.get(), message, field, i, json_options));
        }
    }
    yyjson_mut_doc_set_root(doc.get(), root);

    size_t len = 0;
    char *json = yyjson_mut_write(doc.get(), YYJSON_WRITE_NOFLAG, &len);
    if (!json) {
        return Value();
    }
    Value result(string(json, len));
    free(json);
    return result;
}

static Value ExtractProtobufValue(const Message &root, const vector<const FieldDescriptor *> &path,
                                  const util::JsonPrintOptions &json_options) {
    const Message *message = &root;
    for (idx_t i = 0; i + 1 < path.size(); i++) {
        const Reflection *reflection = message->GetReflection();
        if (!reflection->HasField(*message, path[i])) {
            return Value();
        }
        message = &reflection->GetMessage(*message, path[i]);
    }

    const FieldDescriptor *field = path.back();
    const Reflection *reflection = message->GetReflection();
    if (field->is_repeated()) {
        return ProtoRepeatedToJson(*message, field, json_options);
    }
    // Fields that track presence (messages, proto3 optional, oneof members) are NULL when unset;
    // other proto3 fields read as their default value
    if (field->has_presence() && !reflection->HasField(*message, field)) {
        return Value();
    }

    switch (field->cpp_type()) {
        case FieldDescriptor::CPPTYPE_STRING: {
            string value = reflection->GetString(*message, field);
            if (field->type() == FieldDescriptor::TYPE_BYTES) {
                return Value::BLOB(const_data_ptr_cast(value.data()), value.size());
            }
            // proto2 does not validate UTF-8
            return Utf8Proc::IsValid(value.c_str(), value.size()) ? Value(std::move(value)) : Value();
        }
        case FieldDescriptor::CPPTYPE_INT32:
            return Value::INTEGER(reflection->GetInt32(*message, field));
        case FieldDescriptor::CPPTYPE_INT64:
            return Value::BIGINT(reflection->GetInt64(*message, field));
        case FieldDescriptor::CPPTYPE_UINT32:
            return Value::UINTEGER(reflection->GetUInt32(*message, field));
        case FieldDescriptor::CPPTYPE_UINT64:
            return Value::UBIGINT(reflection->GetUInt64(*message, field));
        case FieldDescriptor::CPPTYPE_FLOAT:
            return Value::FLOAT(reflection->GetFloat(*message, field));
        case FieldDescriptor::CPPTYPE_DOUBLE:
            return Value::DOUBLE(reflection->GetDouble(*message, field));
        case FieldDescriptor::CPPTYPE_BOOL:
            return Value::BOOLEAN(reflection->GetBool(*message, field));
        case FieldDescriptor::CPPTYPE_ENUM:
            return Value(string(reflection->GetEnum(*message, field)->name()));
        case FieldDescriptor::CPPTYPE_MESSAGE: {
            string json;
            if (!util::MessageToJsonString(reflection->GetMessage(*message, field), &json, json_options).ok()) {
                return Value();
            }
            return Value(std::move(json));
        }
        default:
            return Value();
    }
}

static void NatsScanExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    auto &bind_data = data_p.bind_data->Cast<NatsScanBindData>();
    auto &global_state = data_p.global_state->Cast<NatsScanGlobalState>();

    if (global_state.done) {
        output.SetChildCardinality(0);
        return;
    }

    idx_t count = 0;
    const idx_t max_rows = STANDARD_VECTOR_SIZE;
    const string &filter = bind_data.subject_filter.empty() ? string(">") : bind_data.subject_filter;
    const bool payload_is_text = !bind_data.json_fields.empty();

    unique_ptr<Message> proto_msg;
    if (global_state.proto_prototype != nullptr) {
        proto_msg.reset(global_state.proto_prototype->New());
    }

    // Direct writes (avoids per-cell virtual dispatch via SetValue)
    auto stream_data = FlatVector::ScatterWriter<string_t>(output.data[0]);
    auto subject_data = FlatVector::ScatterWriter<string_t>(output.data[1]);
    auto seq_data = FlatVector::ScatterWriter<uint64_t>(output.data[2]);
    auto ts_data = FlatVector::ScatterWriter<timestamp_t>(output.data[3]);
    auto payload_data = FlatVector::ScatterWriter<string_t>(output.data[4]);
    vector<VectorScatterWriter<string_t>> json_data;
    json_data.reserve(bind_data.json_fields.size());
    for (idx_t i = 0; i < bind_data.json_fields.size(); i++) {
        json_data.push_back(FlatVector::ScatterWriter<string_t>(output.data[5 + i]));
    }

    NatsStoredMsg msg;
    while (count < max_rows) {
        if (context.IsInterrupted()) {
            break;
        }

        if (global_state.next_seq > global_state.last_seq ||
            !LoadNextMsg(global_state, bind_data.stream_name, global_state.next_seq, filter, msg) ||
            msg.seq > global_state.last_seq ||
            (bind_data.has_end_time && msg.time_ns / 1000 > bind_data.end_time_us)) {
            global_state.done = true;
            break;
        }
        global_state.next_seq = msg.seq + 1;

        stream_data[count] = string_t(bind_data.stream_name.c_str(), UnsafeNumericCast<uint32_t>(bind_data.stream_name.size()));
        subject_data[count] = string_t(msg.subject, UnsafeNumericCast<uint32_t>(msg.subject_len));
        seq_data[count] = msg.seq;
        ts_data[count] = timestamp_t(msg.time_ns / 1000); // nanos -> micros

        // A VARCHAR payload must be valid UTF-8; binary payloads are NULL there
        if (payload_is_text && !Utf8Proc::IsValid(msg.data, msg.data_len)) {
            payload_data.SetInvalid(count);
        } else {
            payload_data[count] = string_t(msg.data, UnsafeNumericCast<uint32_t>(msg.data_len));
        }

        if (!bind_data.json_fields.empty()) {
            YyjsonDoc doc(yyjson_read(msg.data, msg.data_len, YYJSON_READ_NUMBER_AS_RAW));
            yyjson_val *root = doc ? yyjson_doc_get_root(doc.get()) : nullptr;
            for (idx_t i = 0; i < bind_data.json_fields.size(); i++) {
                yyjson_val *val = root ? LookupJsonPath(root, bind_data.json_fields[i], bind_data.json_paths[i]) : nullptr;
                WriteJsonValue(val, json_data[i], count);
            }
        }

        if (proto_msg) {
            proto_msg->Clear();
            bool parse_success = proto_msg->ParseFromArray(msg.data, UnsafeNumericCast<int>(msg.data_len));

            for (idx_t i = 0; i < bind_data.proto_fields.size(); i++) {
                idx_t col_idx = 5 + i;
                if (parse_success) {
                    output.data[col_idx].SetValue(count, ExtractProtobufValue(*proto_msg, bind_data.proto_paths[i],
                                                                              global_state.json_options));
                } else {
                    FlatVector::SetNull(output.data[col_idx], count, true);
                }
            }
        }

        count++;
    }

    output.SetChildCardinality(count);
}

void NatsScanFunction::Register(ExtensionLoader &loader) {
    TableFunction nats_scan(Identifier("nats_scan"),
                            FunctionSignature().AddPositionalOnly("stream", LogicalType(LogicalTypeId::VARCHAR)),
                            NatsScanExecute, NatsScanBind, NatsScanInitGlobal, NatsScanInitLocal);

    nats_scan.GetSignature().WithTypedKwargs("options", [](TypedKwargs &options) {
        options.Add("subject", LogicalType(LogicalTypeId::VARCHAR));
        options.Add("url", LogicalType(LogicalTypeId::VARCHAR));
        options.Add("start_seq", LogicalType(LogicalTypeId::UBIGINT));
        options.Add("end_seq", LogicalType(LogicalTypeId::UBIGINT));
        options.Add("start_time", LogicalType(LogicalTypeId::TIMESTAMP));
        options.Add("end_time", LogicalType(LogicalTypeId::TIMESTAMP));
        options.Add("json_extract", LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR)));
        options.Add("proto_file", LogicalType(LogicalTypeId::VARCHAR));
        options.Add("proto_message", LogicalType(LogicalTypeId::VARCHAR));
        options.Add("proto_extract", LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR)));
    });

    loader.RegisterFunction(nats_scan);
}

} // namespace duckdb
