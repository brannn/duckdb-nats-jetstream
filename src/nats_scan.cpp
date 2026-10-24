#include "nats_scan.hpp"
#include "nats_secret.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
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
#include <atomic>
#include <cmath>
#include <filesystem>
#include <map>

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
// A batched read asks for this many messages or this many bytes, whichever comes first
static constexpr idx_t NATS_BATCH_MSGS = STANDARD_VECTOR_SIZE;
static constexpr idx_t NATS_BATCH_BYTES = 8 << 20;

// The base columns, in order; field columns follow
static constexpr idx_t COL_STREAM = 0;
static constexpr idx_t COL_SUBJECT = 1;
static constexpr idx_t COL_SEQ = 2;
static constexpr idx_t COL_TS = 3;
static constexpr idx_t COL_HEADERS = 4;
static constexpr idx_t COL_PAYLOAD = 5;
static constexpr idx_t COL_FIELDS = 6;

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

// The range to read comes from the options, narrowed by the query's WHERE clause
struct NatsScanBindData : public TableFunctionData {
    string stream_name;
    string subject_filter;
    // Set by the subject option, which a WHERE clause does not override
    bool subject_given = false;
    string nats_url = "nats://localhost:4222";
    NatsConnectionSettings connection;
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

// A message type by its name in the file's package ('Order', 'Order.Item'), or by its full name ('shop.Order'),
// including types the file imports
static const Descriptor *FindProtoMessage(const DescriptorPool &pool, const FileDescriptor &file, const string &name) {
    if (!file.package().empty()) {
        const Descriptor *descriptor = pool.FindMessageTypeByName(string(file.package()) + "." + name);
        if (descriptor) {
            return descriptor;
        }
    }
    return pool.FindMessageTypeByName(name);
}

// The filter is sent to the server inside a JSON request, so it must be a plain subject
static bool IsPlainSubject(const string &subject) {
    for (char c : subject) {
        if (c == '"' || c == '\\' || StringUtil::CharacterIsSpace(c) || static_cast<unsigned char>(c) < 0x20) {
            return false;
        }
    }
    return true;
}

// A subject without wildcard or empty tokens, which as a filter matches only itself. The server reads a '>' token
// before the end differently from version to version, so WHERE conditions push down only such subjects.
static bool IsLiteralSubject(const string &subject) {
    // The server's default max_control_line keeps stored subjects shorter
    static constexpr idx_t MAX_SUBJECT_LENGTH = 4096;
    if (subject.size() > MAX_SUBJECT_LENGTH || !IsPlainSubject(subject)) {
        return false;
    }
    idx_t start = 0;
    while (true) {
        auto end = subject.find('.', start);
        auto token = subject.substr(start, end == string::npos ? string::npos : end - start);
        if (token.empty() || token == "*" || token == ">") {
            return false;
        }
        if (end == string::npos) {
            return true;
        }
        start = end + 1;
    }
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

static void CheckExternalAccess(ClientContext &context) {
    if (!Settings::Get<EnableExternalAccessSetting>(context)) {
        throw PermissionException("nats_scan is disabled because enable_external_access is false");
    }
}

static unique_ptr<FunctionData> NatsScanBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<Identifier> &names) {
    CheckExternalAccess(context);

    auto bind_data = make_uniq<NatsScanBindData>();
    bind_data->stream_name = GetStringOption(input.inputs[0], "stream");
    if (bind_data->stream_name.empty()) {
        throw BinderException("nats_scan: stream cannot be empty");
    }

    string secret_name;
    for (auto &kv : input.named_parameters) {
        if (kv.first == "subject") {
            bind_data->subject_filter = GetStringOption(kv.second, "subject");
            bind_data->subject_given = true;
        } else if (kv.first == "url") {
            bind_data->nats_url = GetStringOption(kv.second, "url");
        } else if (kv.first == "secret") {
            secret_name = GetStringOption(kv.second, "secret");
        } else if (kv.first == "start_seq") {
            if (kv.second.IsNull()) {
                throw BinderException("nats_scan: start_seq cannot be NULL");
            }
            bind_data->start_seq = UBigIntValue::Get(kv.second);
        } else if (kv.first == "end_seq") {
            if (kv.second.IsNull()) {
                throw BinderException("nats_scan: end_seq cannot be NULL");
            }
            bind_data->end_seq = UBigIntValue::Get(kv.second);
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

    bind_data->connection = NatsSecret::Lookup(context, secret_name, bind_data->nats_url);

    if (!IsPlainSubject(bind_data->subject_filter)) {
        throw BinderException("nats_scan: invalid subject filter '%s'", bind_data->subject_filter);
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

        const Descriptor *descriptor = FindProtoMessage(*schema->pool, *file_desc, bind_data->proto_message);
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
    // A header can repeat, so each key maps to its values
    AddColumn(names, used_names, "headers");
    return_types.emplace_back(LogicalType::MAP(LogicalType(LogicalTypeId::VARCHAR),
                                               LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR))));

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

// A stored message, from either a direct get (msg owns the buffers and headers) or a STREAM.MSG.GET reply (decoded
// into the strings)
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
    // The message's header block, "NATS/1.0\r\nKey: Value\r\n...", when msg is not set
    string headers_buf;
    // How many of REPLY_HEADERS the server appended to msg
    idx_t reply_headers = 0;

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

// The headers the server appends to a direct get reply, after the message's own: the first four on every reply, all
// six on a batched one. A message can have the same keys itself (one republished from another stream does), so the
// server's value of each is the last.
static const char *const REPLY_HEADERS[] = {"Nats-Stream",     "Nats-Subject",     "Nats-Sequence",
                                            "Nats-Time-Stamp", "Nats-Num-Pending", "Nats-Last-Sequence"};

static const char *LastHeader(natsMsg *msg, const char *key) {
    const char **values = nullptr;
    int count = 0;
    if (natsMsgHeader_Values(msg, key, &values, &count) != NATS_OK || count == 0) {
        return nullptr;
    }
    // The array is the caller's, the strings the message's
    const char *last = values[count - 1];
    free((void *)values);
    return last;
}

static bool IsReplyHeader(const NatsStoredMsg &msg, const char *key) {
    for (idx_t i = 0; i < msg.reply_headers; i++) {
        if (strcmp(key, REPLY_HEADERS[i]) == 0) {
            return true;
        }
    }
    return false;
}

// Keys in order, each with its values in the order they were sent
using NatsHeaders = std::map<string, vector<Value>>;

// A key that is not valid UTF-8 is left out, and a value that is not is NULL
static void AddHeader(NatsHeaders &headers, string key, const char *value, idx_t value_len) {
    if (key.empty() || !Utf8Proc::IsValid(key.c_str(), key.size())) {
        return;
    }
    headers[std::move(key)].push_back(Utf8Proc::IsValid(value, value_len) ? Value(string(value, value_len))
                                                                          : Value(LogicalType(LogicalTypeId::VARCHAR)));
}

static void ParseHeaderBlock(const string &block, NatsHeaders &headers) {
    // The first line is the version, "NATS/1.0"; an empty line ends the block
    auto line_end = block.find("\r\n");
    while (line_end != string::npos) {
        auto start = line_end + 2;
        line_end = block.find("\r\n", start);
        if (line_end == string::npos || line_end == start) {
            break;
        }
        auto colon = block.find(':', start);
        if (colon == string::npos || colon > line_end) {
            continue;
        }
        auto value_start = colon + 1;
        auto value_end = line_end;
        while (value_start < value_end && (block[value_start] == ' ' || block[value_start] == '\t')) {
            value_start++;
        }
        while (value_end > value_start && (block[value_end - 1] == ' ' || block[value_end - 1] == '\t')) {
            value_end--;
        }
        AddHeader(headers, block.substr(start, colon - start), block.data() + value_start, value_end - value_start);
    }
}

static Value HeadersValue(NatsStoredMsg &msg) {
    NatsHeaders headers;
    const char **keys = nullptr;
    int key_count = 0;
    if (msg.msg == nullptr) {
        ParseHeaderBlock(msg.headers_buf, headers);
    } else if (natsMsgHeader_Keys(msg.msg, &keys, &key_count) == NATS_OK) {
        for (int i = 0; i < key_count; i++) {
            const char **values = nullptr;
            int value_count = 0;
            if (natsMsgHeader_Values(msg.msg, keys[i], &values, &value_count) != NATS_OK) {
                continue;
            }
            // Leave out the server's value
            if (IsReplyHeader(msg, keys[i])) {
                value_count--;
            }
            for (int j = 0; j < value_count; j++) {
                AddHeader(headers, keys[i], values[j], strlen(values[j]));
            }
            free((void *)values);
        }
        free((void *)keys);
    }

    vector<Value> map_keys;
    vector<Value> map_values;
    for (auto &header : headers) {
        map_keys.emplace_back(header.first);
        map_values.push_back(Value::LIST(LogicalType(LogicalTypeId::VARCHAR), std::move(header.second)));
    }
    return Value::MAP(LogicalType(LogicalTypeId::VARCHAR), LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR)),
                      std::move(map_keys), std::move(map_values));
}

// How messages are read: in batches (streams with allow_direct, on nats-server 2.11 or later), or one per request
// through a direct get or, for streams without allow_direct, the JetStream API
enum class NatsReadMode : uint8_t { BATCH, DIRECT_GET, MSG_GET };

// Connection options, freed however the scan's init ends
struct NatsOptions {
    natsOptions *opts = nullptr;
    ~NatsOptions() {
        if (opts != nullptr) {
            natsOptions_Destroy(opts);
        }
    }
};

struct NatsScanGlobalState : public GlobalTableFunctionState {
    // An NKEY secret's key, which conn signs with
    unique_ptr<NatsNKey> nkey;
    natsConnection *conn = nullptr;
    jsCtx *js = nullptr;
    jsStreamInfo *stream_info = nullptr;
    NatsReadMode mode = NatsReadMode::MSG_GET;
    // $JS.API.DIRECT.GET.<stream> for batches, $JS.API.STREAM.MSG.GET.<stream> for the JetStream API
    string get_subject;
    natsInbox *inbox = nullptr;
    natsSubscription *batch_sub = nullptr;
    // A batch was requested and its end has not been read
    bool batch_open = false;
    idx_t batch_msgs = 0;

    // The scan reads first_seq to last_seq; next_seq and done are also read by the progress bar
    uint64_t first_seq = 1;
    std::atomic<uint64_t> next_seq {1};
    uint64_t last_seq = 0;
    std::atomic<bool> done {false};

    // Where each base and field column goes in the output, or INVALID_INDEX if the query does not use it
    idx_t out_base[COL_FIELDS];
    vector<idx_t> out_fields;

    shared_ptr<DynamicMessageFactory> proto_factory;
    const Message* proto_prototype = nullptr;
    util::JsonPrintOptions json_options;

    ~NatsScanGlobalState() {
        if (batch_sub != nullptr) {
            natsSubscription_Destroy(batch_sub);
            batch_sub = nullptr;
        }
        if (inbox != nullptr) {
            natsInbox_Destroy(inbox);
            inbox = nullptr;
        }
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

static void DecodeBase64(yyjson_val *encoded_val, string &out) {
    out.clear();
    if (!yyjson_is_str(encoded_val) || yyjson_get_len(encoded_val) == 0) {
        return;
    }
    string_t encoded(yyjson_get_str(encoded_val), UnsafeNumericCast<uint32_t>(yyjson_get_len(encoded_val)));
    out.resize(Blob::FromBase64Size(encoded));
    Blob::FromBase64(encoded, data_ptr_cast(&out[0]), out.size());
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
    yyjson_val *hdrs = yyjson_obj_get(message, "hdrs");
    timestamp_ns_t time_ns;
    if (!yyjson_is_str(subject) || !yyjson_is_uint(seq) || !yyjson_is_str(time) ||
        Timestamp::TryConvertTimestamp(yyjson_get_str(time), yyjson_get_len(time), time_ns, true) !=
            TimestampCastResult::SUCCESS) {
        throw IOException("nats_scan: invalid message get response from stream '%s'", stream_name);
    }

    out.seq = yyjson_get_uint(seq);
    out.time_ns = Timestamp::GetEpochNanoSeconds(time_ns);
    out.subject_buf.assign(yyjson_get_str(subject), yyjson_get_len(subject));
    DecodeBase64(data, out.data_buf);
    DecodeBase64(hdrs, out.headers_buf);
    out.subject = out.subject_buf.c_str();
    out.subject_len = out.subject_buf.size();
    out.data = out.data_buf.c_str();
    out.data_len = out.data_buf.size();
    return true;
}

static bool GetUIntHeader(natsMsg *msg, const char *key, uint64_t &result) {
    const char *value = LastHeader(msg, key);
    if (value == nullptr || !StringUtil::CharacterIsDigit(*value)) {
        return false;
    }
    char *end = nullptr;
    errno = 0;
    result = strtoull(value, &end, 10);
    return errno == 0 && *end == '\0';
}

// Parses a direct get reply, which out takes over. A stored message always comes with the server's Nats-Sequence
// header; any other reply is a status, such as 404 (no message) or 204 (end of batch), returned in status with its
// description in status_text. A stored message's own Status header is not one.
static bool ParseDirectGetReply(natsMsg *reply, bool batched, const string &stream_name, NatsStoredMsg &out,
                                string &status, string &status_text) {
    out.Clear();
    out.msg = reply;
    out.reply_headers = batched ? 6 : 4;
    if (LastHeader(reply, "Nats-Sequence") == nullptr) {
        const char *code = nullptr;
        const char *description = nullptr;
        natsMsgHeader_Get(reply, "Status", &code);
        natsMsgHeader_Get(reply, "Description", &description);
        if (code == nullptr) {
            throw IOException("nats_scan: invalid direct get response from stream '%s'", stream_name);
        }
        status = code;
        status_text = description ? status + " " + description : status;
        return false;
    }

    const char *subject = LastHeader(reply, "Nats-Subject");
    const char *time = LastHeader(reply, "Nats-Time-Stamp");
    timestamp_ns_t time_ns;
    if (!GetUIntHeader(reply, "Nats-Sequence", out.seq) || subject == nullptr || time == nullptr ||
        Timestamp::TryConvertTimestamp(time, strlen(time), time_ns, true) != TimestampCastResult::SUCCESS) {
        throw IOException("nats_scan: invalid direct get response from stream '%s'", stream_name);
    }
    out.time_ns = Timestamp::GetEpochNanoSeconds(time_ns);
    out.subject = subject;
    out.subject_len = strlen(subject);
    out.data = natsMsg_GetData(reply);
    out.data_len = UnsafeNumericCast<idx_t>(natsMsg_GetDataLength(reply));
    if (out.data == nullptr) {
        out.data = "";
    }
    return true;
}

// Loads the first message at or after seq whose subject matches filter ('>' matches all).
// Returns false if there is none.
static bool LoadNextMsg(NatsScanGlobalState &state, const string &stream_name, uint64_t seq, const string &filter,
                        NatsStoredMsg &out) {
    out.Clear();
    // A direct get, or for streams without allow_direct the JetStream API, which js_GetMsg exposes without
    // next_by_subj
    string request = "{\"seq\":" + std::to_string(seq) + ",\"next_by_subj\":\"" + filter + "\"}";
    natsMsg *reply = nullptr;
    natsStatus s = natsConnection_Request(&reply, state.conn, state.get_subject.c_str(), request.c_str(),
                                          UnsafeNumericCast<int>(request.size()), NATS_TIMEOUT_MS);
    if (s != NATS_OK) {
        throw IOException("nats_scan: failed to read stream '%s' at sequence %d: %s", stream_name, seq,
                          NatsErrorText(s));
    }
    if (state.mode == NatsReadMode::DIRECT_GET) {
        string status;
        string status_text;
        if (ParseDirectGetReply(reply, false, stream_name, out, status, status_text)) {
            return true;
        }
        out.Clear();
        if (status == "404") {
            return false;
        }
        throw IOException("nats_scan: failed to read stream '%s' at sequence %d: %s", stream_name, seq, status_text);
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

// Batched reads: one direct get request returns up to a number of messages, then an end-of-batch status

static void SendBatchRequest(NatsScanGlobalState &state, const string &stream_name, const string &request) {
    natsStatus s = natsConnection_PublishRequest(state.conn, state.get_subject.c_str(), state.inbox, request.c_str(),
                                                 UnsafeNumericCast<int>(request.size()));
    if (s != NATS_OK) {
        throw IOException("nats_scan: failed to read stream '%s': %s", stream_name, NatsErrorText(s));
    }
    state.batch_open = true;
    state.batch_msgs = 0;
}

// Reads the open batch's next message. At the end of the batch, returns false with the number of matching messages
// that follow it in the stream.
static bool ReadBatchMsg(NatsScanGlobalState &state, const string &stream_name, NatsStoredMsg &out,
                         uint64_t &pending) {
    out.Clear();
    natsMsg *reply = nullptr;
    natsStatus s = natsSubscription_NextMsg(&reply, state.batch_sub, NATS_TIMEOUT_MS);
    if (s != NATS_OK) {
        throw IOException("nats_scan: failed to read stream '%s': %s", stream_name, NatsErrorText(s));
    }

    string status;
    string status_text;
    if (!ParseDirectGetReply(reply, true, stream_name, out, status, status_text)) {
        state.batch_open = false;
        // An end of batch after no message would ask for the same batch again
        bool more = status == "204" && state.batch_msgs > 0 && GetUIntHeader(out.msg, "Nats-Num-Pending", pending);
        out.Clear();
        if (status == "204" || status == "404") {
            if (!more) {
                pending = 0;
            }
            return false;
        }
        throw IOException("nats_scan: failed to read stream '%s': %s", stream_name, status_text);
    }
    state.batch_msgs++;

    // A server without batched reads answers with this one message, and no end of batch follows
    uint64_t unused;
    if (!GetUIntHeader(out.msg, "Nats-Num-Pending", unused)) {
        state.batch_open = false;
    }
    return true;
}

// Loads the scan's next message at or after next_seq. Returns false if there is none up to last_seq.
static bool ReadNextMsg(NatsScanGlobalState &state, const string &stream_name, const string &filter,
                        NatsStoredMsg &msg) {
    if (state.mode != NatsReadMode::BATCH) {
        return state.next_seq <= state.last_seq && LoadNextMsg(state, stream_name, state.next_seq, filter, msg);
    }
    while (true) {
        if (!state.batch_open) {
            uint64_t next_seq = state.next_seq;
            if (next_seq > state.last_seq) {
                return false;
            }
            auto batch = MinValue<uint64_t>(NATS_BATCH_MSGS, state.last_seq - next_seq + 1);
            SendBatchRequest(state, stream_name,
                             "{\"seq\":" + std::to_string(next_seq) + ",\"next_by_subj\":\"" + filter +
                                 "\",\"batch\":" + std::to_string(batch) +
                                 ",\"max_bytes\":" + std::to_string(NATS_BATCH_BYTES) + "}");
        }
        uint64_t pending = 0;
        if (ReadBatchMsg(state, stream_name, msg, pending)) {
            return true;
        }
        if (pending == 0) {
            return false;
        }
    }
}

// The first message at or after the given time whose subject matches filter, which the server finds itself.
// time_us must be from 1970 to 2262-04-11 23:47:16.854775, the range the server reads.
static bool FindFirstSeqAtTimeBatched(NatsScanGlobalState &state, const string &stream_name, int64_t time_us,
                                      const string &filter, uint64_t &result) {
    auto time = Timestamp::ToString(timestamp_t(time_us));
    time[10] = 'T';
    SendBatchRequest(state, stream_name,
                     "{\"start_time\":\"" + time + "Z\",\"next_by_subj\":\"" + filter + "\",\"batch\":1}");
    NatsStoredMsg msg;
    uint64_t pending = 0;
    bool found = false;
    while (state.batch_open && ReadBatchMsg(state, stream_name, msg, pending)) {
        if (!found) {
            result = msg.seq;
            found = true;
        }
    }
    return found;
}

// Servers from 2.11 on report their API level in stream metadata; level 1 added batched direct gets
static bool ServerReadsBatches(const jsStreamInfo &info) {
    if (info.Config == nullptr) {
        return false;
    }
    auto &metadata = info.Config->Metadata;
    for (int i = 0; i < metadata.Count; i++) {
        if (strcmp(metadata.List[2 * i], "_nats.level") == 0) {
            return atoi(metadata.List[2 * i + 1]) >= 1;
        }
    }
    return false;
}

static unique_ptr<GlobalTableFunctionState> NatsScanInitGlobal(ClientContext &context,
                                                                 TableFunctionInitInput &input) {
    // Also here, for a statement prepared before the setting changed
    CheckExternalAccess(context);
    auto &bind_data = input.bind_data->Cast<NatsScanBindData>();
    auto state = make_uniq<NatsScanGlobalState>();

    std::fill_n(state->out_base, COL_FIELDS, DConstants::INVALID_INDEX);
    state->out_fields.assign(bind_data.json_fields.size() + bind_data.proto_fields.size(), DConstants::INVALID_INDEX);
    for (idx_t i = 0; i < input.column_ids.size(); i++) {
        auto column_id = input.column_ids[i];
        if (IsVirtualColumn(column_id)) {
            continue;
        }
        if (column_id < COL_FIELDS) {
            state->out_base[column_id] = i;
        } else {
            state->out_fields[column_id - COL_FIELDS] = i;
        }
    }

    if (bind_data.proto_descriptor != nullptr) {
        state->proto_factory = make_shared_ptr<DynamicMessageFactory>();
        state->proto_prototype = state->proto_factory->GetPrototype(bind_data.proto_descriptor);
        state->json_options.preserve_proto_field_names = true;
        state->json_options.unquote_int64_if_possible = true;
    }

    // Connect to NATS and validate stream at init time (fail-fast on typos / unreachable server)
    NatsOptions options;
    natsStatus s = natsOptions_Create(&options.opts);
    if (s != NATS_OK) {
        throw IOException("Failed to create NATS options: %s", NatsErrorText(s));
    }

    s = natsOptions_SetTimeout(options.opts, NATS_TIMEOUT_MS);
    if (s == NATS_OK) {
        s = natsOptions_SetURL(options.opts, bind_data.nats_url.c_str());
    }
    if (s != NATS_OK) {
        throw IOException("Invalid NATS URL '%s': %s", bind_data.nats_url, NatsErrorText(s));
    }
    try {
        s = NatsSecret::Apply(context, bind_data.connection, options.opts, state->nkey);
    } catch (std::exception &ex) {
        ErrorData error(ex);
        if (error.Type() != ExceptionType::IO) {
            throw;
        }
        throw IOException("Cannot use NATS secret '%s': %s", bind_data.connection.secret_name, error.RawMessage());
    }
    if (s != NATS_OK) {
        throw IOException("Cannot use NATS secret '%s': %s", bind_data.connection.secret_name, NatsErrorText(s));
    }

    s = natsConnection_Connect(&state->conn, options.opts);
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

    bool allow_direct = state->stream_info->Config != nullptr && state->stream_info->Config->AllowDirect;
    if (allow_direct && ServerReadsBatches(*state->stream_info)) {
        state->mode = NatsReadMode::BATCH;
        state->get_subject = "$JS.API.DIRECT.GET." + bind_data.stream_name;
        s = natsInbox_Create(&state->inbox);
        if (s == NATS_OK) {
            s = natsConnection_SubscribeSync(&state->batch_sub, state->conn, state->inbox);
        }
        // A batch is bounded by NATS_BATCH_BYTES, but one message can be larger, so the library must not drop any
        if (s == NATS_OK) {
            s = natsSubscription_SetPendingLimits(state->batch_sub, -1, -1);
        }
        if (s != NATS_OK) {
            throw IOException("Failed to subscribe for replies from NATS: %s", NatsErrorText(s));
        }
    } else if (allow_direct) {
        state->mode = NatsReadMode::DIRECT_GET;
        state->get_subject = "$JS.API.DIRECT.GET." + bind_data.stream_name;
    } else {
        state->mode = NatsReadMode::MSG_GET;
        state->get_subject = "$JS.API.STREAM.MSG.GET." + bind_data.stream_name;
    }

    // The scan stops at the stream's last message as of now, so it ends on a busy stream
    auto &stream_state = state->stream_info->State;
    uint64_t next_seq = MaxValue<uint64_t>(MaxValue<uint64_t>(bind_data.start_seq, stream_state.FirstSeq), 1);
    state->last_seq = MinValue<uint64_t>(bind_data.end_seq, stream_state.LastSeq);
    if (stream_state.Msgs == 0 || next_seq > state->last_seq) {
        state->done = true;
        return std::move(state);
    }

    if (bind_data.has_start_time) {
        // The server keeps message times in int64 nanoseconds, so none is after 2262-04-11 23:47:16.854775, and it
        // reads a later start time wrong
        static constexpr int64_t TIME_LIMIT_US = NumericLimits<int64_t>::Maximum() / 1000;
        const string &filter = bind_data.subject_filter.empty() ? string(">") : bind_data.subject_filter;
        uint64_t seq = next_seq;
        bool found = true;
        if (bind_data.start_time_us > TIME_LIMIT_US) {
            found = false;
        } else if (state->mode != NatsReadMode::BATCH) {
            found = FindFirstSeqAtTime(*state, bind_data.stream_name, bind_data.start_time_us, next_seq,
                                       state->last_seq, seq);
        } else if (bind_data.start_time_us > 0) {
            found = FindFirstSeqAtTimeBatched(*state, bind_data.stream_name, bind_data.start_time_us, filter, seq);
        }
        if (!found) {
            state->done = true;
            return std::move(state);
        }
        next_seq = MaxValue<uint64_t>(next_seq, seq);
    }
    state->first_seq = next_seq;
    state->next_seq = next_seq;
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

// A writer for the output column, or none if the query does not use the column
template <class T>
static unique_ptr<VectorScatterWriter<T>> ColumnWriter(DataChunk &output, idx_t out_idx) {
    if (out_idx == DConstants::INVALID_INDEX) {
        return nullptr;
    }
    return make_uniq<VectorScatterWriter<T>>(output.data[out_idx]);
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
    const idx_t headers_idx = global_state.out_base[COL_HEADERS];
    auto &out_fields = global_state.out_fields;

    // Direct writes (avoids per-cell virtual dispatch via SetValue)
    auto stream_data = ColumnWriter<string_t>(output, global_state.out_base[COL_STREAM]);
    auto subject_data = ColumnWriter<string_t>(output, global_state.out_base[COL_SUBJECT]);
    auto seq_data = ColumnWriter<uint64_t>(output, global_state.out_base[COL_SEQ]);
    auto ts_data = ColumnWriter<timestamp_t>(output, global_state.out_base[COL_TS]);
    auto payload_data = ColumnWriter<string_t>(output, global_state.out_base[COL_PAYLOAD]);
    vector<unique_ptr<VectorScatterWriter<string_t>>> json_data;
    bool read_json = false;
    for (idx_t i = 0; i < bind_data.json_fields.size(); i++) {
        json_data.push_back(ColumnWriter<string_t>(output, out_fields[i]));
        read_json = read_json || json_data.back();
    }

    unique_ptr<Message> proto_msg;
    if (global_state.proto_prototype != nullptr &&
        std::any_of(out_fields.begin(), out_fields.end(), [](idx_t i) { return i != DConstants::INVALID_INDEX; })) {
        proto_msg.reset(global_state.proto_prototype->New());
    }

    NatsStoredMsg msg;
    while (count < max_rows) {
        if (context.IsInterrupted()) {
            break;
        }

        if (!ReadNextMsg(global_state, bind_data.stream_name, filter, msg) || msg.seq > global_state.last_seq ||
            (bind_data.has_end_time && msg.time_ns / 1000 > bind_data.end_time_us)) {
            global_state.done = true;
            break;
        }
        global_state.next_seq = msg.seq + 1;

        if (stream_data) {
            (*stream_data)[count] =
                string_t(bind_data.stream_name.c_str(), UnsafeNumericCast<uint32_t>(bind_data.stream_name.size()));
        }
        if (subject_data) {
            (*subject_data)[count] = string_t(msg.subject, UnsafeNumericCast<uint32_t>(msg.subject_len));
        }
        if (seq_data) {
            (*seq_data)[count] = msg.seq;
        }
        if (ts_data) {
            (*ts_data)[count] = timestamp_t(msg.time_ns / 1000); // nanos -> micros
        }
        if (headers_idx != DConstants::INVALID_INDEX) {
            output.data[headers_idx].SetValue(count, HeadersValue(msg));
        }

        // A VARCHAR payload must be valid UTF-8; binary payloads are NULL there
        if (payload_data && payload_is_text && !Utf8Proc::IsValid(msg.data, msg.data_len)) {
            payload_data->SetInvalid(count);
        } else if (payload_data) {
            (*payload_data)[count] = string_t(msg.data, UnsafeNumericCast<uint32_t>(msg.data_len));
        }

        if (read_json) {
            YyjsonDoc doc(yyjson_read(msg.data, msg.data_len, YYJSON_READ_NUMBER_AS_RAW));
            yyjson_val *root = doc ? yyjson_doc_get_root(doc.get()) : nullptr;
            for (idx_t i = 0; i < bind_data.json_fields.size(); i++) {
                if (!json_data[i]) {
                    continue;
                }
                yyjson_val *val = root ? LookupJsonPath(root, bind_data.json_fields[i], bind_data.json_paths[i]) : nullptr;
                WriteJsonValue(val, *json_data[i], count);
            }
        }

        if (proto_msg) {
            proto_msg->Clear();
            bool parse_success = proto_msg->ParseFromArray(msg.data, UnsafeNumericCast<int>(msg.data_len));

            for (idx_t i = 0; i < bind_data.proto_fields.size(); i++) {
                idx_t col_idx = out_fields[i];
                if (col_idx == DConstants::INVALID_INDEX) {
                    continue;
                }
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

// Filter pushdown: a WHERE clause on seq, ts_nats or subject narrows the range the scan reads. DuckDB still applies
// every filter to the rows, so a filter only has to describe a range that contains its rows.

// The scan column an expression is, or INVALID_INDEX if it is something else
static idx_t FilterColumn(const LogicalGet &get, const Expression &expr) {
    if (expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
        return DConstants::INVALID_INDEX;
    }
    auto &binding = expr.Cast<BoundColumnRefExpression>().Binding();
    if (binding.table_index != get.table_index) {
        return DConstants::INVALID_INDEX;
    }
    auto &column_index = get.GetColumnIndex(binding);
    return column_index.IsVirtualColumn() ? DConstants::INVALID_INDEX : column_index.GetPrimaryIndex();
}

static bool FilterConstant(ClientContext &context, const Expression &expr, Value &result) {
    return expr.IsFoldable() && ExpressionExecutor::TryEvaluateScalar(context, expr, result) && !result.IsNull();
}

// Narrows the range by "column <comparison> constant"
static void PushdownComparison(NatsScanBindData &bind_data, idx_t column, ExpressionType comparison,
                               const Value &constant) {
    bool lower = comparison == ExpressionType::COMPARE_EQUAL || comparison == ExpressionType::COMPARE_GREATERTHAN ||
                 comparison == ExpressionType::COMPARE_GREATERTHANOREQUALTO;
    bool upper = comparison == ExpressionType::COMPARE_EQUAL || comparison == ExpressionType::COMPARE_LESSTHAN ||
                 comparison == ExpressionType::COMPARE_LESSTHANOREQUALTO;
    bool strict = comparison == ExpressionType::COMPARE_GREATERTHAN || comparison == ExpressionType::COMPARE_LESSTHAN;

    if (column == COL_SEQ && constant.type().id() == LogicalTypeId::UBIGINT) {
        auto seq = UBigIntValue::Get(constant);
        if (strict && (lower ? seq == NumericLimits<uint64_t>::Maximum() : seq == 0)) {
            // Nothing is greater than the largest sequence or less than 0
            bind_data.start_seq = NumericLimits<uint64_t>::Maximum();
            bind_data.end_seq = 0;
            return;
        }
        seq = strict ? (lower ? seq + 1 : seq - 1) : seq;
        if (lower) {
            bind_data.start_seq = MaxValue(bind_data.start_seq, seq);
        }
        if (upper) {
            bind_data.end_seq = MinValue(bind_data.end_seq, seq);
        }
    } else if (column == COL_TS && constant.type().id() == LogicalTypeId::TIMESTAMP) {
        auto time_us = TimestampValue::Get(constant).value;
        if (strict && (time_us == NumericLimits<int64_t>::Maximum() || time_us == NumericLimits<int64_t>::Minimum())) {
            return;
        }
        time_us = strict ? (lower ? time_us + 1 : time_us - 1) : time_us;
        if (lower && (!bind_data.has_start_time || time_us > bind_data.start_time_us)) {
            bind_data.start_time_us = time_us;
            bind_data.has_start_time = true;
        }
        if (upper && (!bind_data.has_end_time || time_us < bind_data.end_time_us)) {
            bind_data.end_time_us = time_us;
            bind_data.has_end_time = true;
        }
    } else if (column == COL_SUBJECT && comparison == ExpressionType::COMPARE_EQUAL && !bind_data.subject_given) {
        auto subject = StringValue::Get(constant);
        if (IsLiteralSubject(subject)) {
            bind_data.subject_filter = subject;
        }
    }
}

static void PushdownFilter(ClientContext &context, const LogicalGet &get, NatsScanBindData &bind_data,
                           const Expression &filter) {
    if (filter.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
        return;
    }
    auto &function = filter.Cast<BoundFunctionExpression>();
    Value constant;
    if (filter.GetExpressionType() == ExpressionType::COMPARE_BETWEEN) {
        if (!BoundBetweenExpression::HasValidBindData(function)) {
            return;
        }
        auto column = FilterColumn(get, BoundBetweenExpression::Input(function));
        if (column == DConstants::INVALID_INDEX) {
            return;
        }
        if (FilterConstant(context, BoundBetweenExpression::LowerBound(function), constant)) {
            PushdownComparison(bind_data, column, BoundBetweenExpression::LowerComparisonType(function), constant);
        }
        if (FilterConstant(context, BoundBetweenExpression::UpperBound(function), constant)) {
            PushdownComparison(bind_data, column, BoundBetweenExpression::UpperComparisonType(function), constant);
        }
    } else if (BoundComparisonExpression::IsComparison(filter)) {
        auto comparison = filter.GetExpressionType();
        auto &left = BoundComparisonExpression::Left(function);
        auto &right = BoundComparisonExpression::Right(function);
        auto column = FilterColumn(get, left);
        if (column != DConstants::INVALID_INDEX && FilterConstant(context, right, constant)) {
            PushdownComparison(bind_data, column, comparison, constant);
        } else if ((column = FilterColumn(get, right)) != DConstants::INVALID_INDEX &&
                   FilterConstant(context, left, constant)) {
            PushdownComparison(bind_data, column, FlipComparisonExpression(comparison), constant);
        }
    } else {
        // subject LIKE 'orders.eu.%' arrives as a prefix, which the subject filter orders.eu.> contains
        auto &name = function.Function().GetName();
        if ((name == "prefix" || name == "starts_with") && function.GetChildren().size() == 2 &&
            FilterColumn(get, *function.GetChildren()[0]) == COL_SUBJECT && bind_data.subject_filter.empty() &&
            FilterConstant(context, *function.GetChildren()[1], constant)) {
            auto prefix = StringValue::Get(constant);
            if (prefix.size() > 1 && prefix.back() == '.' && IsLiteralSubject(prefix.substr(0, prefix.size() - 1))) {
                bind_data.subject_filter = prefix + ">";
            }
        }
    }
}

static void NatsScanPushdownComplexFilter(ClientContext &context, LogicalGet &get, FunctionData *bind_data_p,
                                          vector<unique_ptr<Expression>> &filters) {
    auto &bind_data = bind_data_p->Cast<NatsScanBindData>();
    for (auto &filter : filters) {
        PushdownFilter(context, get, bind_data, *filter);
    }
}

// The share of the sequence range read so far
static double NatsScanProgress(ClientContext &context, const FunctionData *bind_data,
                               const GlobalTableFunctionState *global_state) {
    auto &state = global_state->Cast<NatsScanGlobalState>();
    if (state.done) {
        return 100.0;
    }
    uint64_t next_seq = state.next_seq;
    if (state.last_seq < state.first_seq || next_seq <= state.first_seq) {
        return 0.0;
    }
    return 100.0 * double(next_seq - state.first_seq) / double(state.last_seq - state.first_seq + 1);
}

// The range a query reads, in EXPLAIN
static InsertionOrderPreservingMap<string> NatsScanToString(TableFunctionToStringInput &input) {
    auto &bind_data = input.bind_data->Cast<NatsScanBindData>();
    InsertionOrderPreservingMap<string> result;
    result["Stream"] = bind_data.stream_name;
    if (!bind_data.subject_filter.empty()) {
        result["Subject"] = bind_data.subject_filter;
    }
    if (bind_data.start_seq > 0 || bind_data.end_seq < NumericLimits<uint64_t>::Maximum()) {
        string range = bind_data.start_seq > 0 ? "from " + std::to_string(bind_data.start_seq) : "";
        if (bind_data.end_seq < NumericLimits<uint64_t>::Maximum()) {
            range += string(range.empty() ? "" : " ") + "to " + std::to_string(bind_data.end_seq);
        }
        result["Sequences"] = range;
    }
    if (bind_data.has_start_time || bind_data.has_end_time) {
        string range =
            bind_data.has_start_time ? "from " + Timestamp::ToString(timestamp_t(bind_data.start_time_us)) : "";
        if (bind_data.has_end_time) {
            range += string(range.empty() ? "" : " ") + "to " + Timestamp::ToString(timestamp_t(bind_data.end_time_us));
        }
        result["Times"] = range;
    }
    return result;
}

void NatsScanFunction::Register(ExtensionLoader &loader) {
    TableFunction nats_scan(Identifier("nats_scan"),
                            FunctionSignature().AddPositionalOnly("stream", LogicalType(LogicalTypeId::VARCHAR)),
                            NatsScanExecute, NatsScanBind, NatsScanInitGlobal, NatsScanInitLocal);
    nats_scan.projection_pushdown = true;
    nats_scan.pushdown_complex_filter = NatsScanPushdownComplexFilter;
    nats_scan.table_scan_progress = NatsScanProgress;
    nats_scan.to_string = NatsScanToString;

    nats_scan.GetSignature().WithTypedKwargs("options", [](TypedKwargs &options) {
        options.Add("subject", LogicalType(LogicalTypeId::VARCHAR));
        options.Add("url", LogicalType(LogicalTypeId::VARCHAR));
        options.Add("secret", LogicalType(LogicalTypeId::VARCHAR));
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
