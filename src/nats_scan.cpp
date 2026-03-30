#include "nats_scan.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/vector.hpp"
#include "yyjson.hpp"
#include <nats/nats.h>
#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/descriptor.h>
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

struct NatsScanBindData : public TableFunctionData {
    string stream_name;
    string subject_filter;
    string nats_url;
    uint64_t start_seq;
    uint64_t end_seq;
    int64_t start_time;  // nanoseconds since epoch, 0 = not set
    int64_t end_time;    // nanoseconds since epoch, 0 = not set
    vector<string> json_fields;
    string proto_file;
    string proto_message;
    vector<string> proto_fields;

    // Must outlive the query — proto_descriptor is owned by the importer's pool
    shared_ptr<DiskSourceTree> proto_source_tree;
    shared_ptr<ProtobufErrorCollector> proto_error_collector;
    shared_ptr<Importer> proto_importer;
    const Descriptor* proto_descriptor = nullptr;

    NatsScanBindData(string stream, string subject, string url, uint64_t start, uint64_t end,
                     int64_t start_ts, int64_t end_ts, vector<string> json_flds,
                     string proto_f, string proto_msg, vector<string> proto_flds)
        : stream_name(std::move(stream))
        , subject_filter(std::move(subject))
        , nats_url(std::move(url))
        , start_seq(start)
        , end_seq(end)
        , start_time(start_ts)
        , end_time(end_ts)
        , json_fields(std::move(json_flds))
        , proto_file(std::move(proto_f))
        , proto_message(std::move(proto_msg))
        , proto_fields(std::move(proto_flds)) {
    }
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

static const FieldDescriptor* GetFieldDescriptorForPath(const Descriptor* message_desc, const string& field_path) {
    auto path_parts = SplitFieldPath(field_path);
    const Descriptor* current_desc = message_desc;
    const FieldDescriptor* field = nullptr;

    for (size_t i = 0; i < path_parts.size(); i++) {
        field = current_desc->FindFieldByName(path_parts[i]);
        if (!field) {
            return nullptr;
        }
        if (i < path_parts.size() - 1) {
            if (field->type() != FieldDescriptor::TYPE_MESSAGE) {
                return nullptr;
            }
            current_desc = field->message_type();
        }
    }

    return field;
}

// Helper function to map protobuf field type to DuckDB LogicalType
static LogicalType ProtobufTypeToDuckDBType(const FieldDescriptor* field) {
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
        case FieldDescriptor::TYPE_MESSAGE:
            // Nested messages not supported as column types (should be extracted as fields)
            return LogicalType(LogicalTypeId::VARCHAR);
        default:
            // Unknown type - default to VARCHAR
            return LogicalType(LogicalTypeId::VARCHAR);
    }
}

struct NatsScanGlobalState : public GlobalTableFunctionState {
    natsConnection *conn = nullptr;
    jsCtx *js = nullptr;
    jsStreamInfo *stream_info = nullptr;
    uint64_t current_seq = 0;
    uint64_t end_seq = 0;
    bool done = false;

    shared_ptr<DynamicMessageFactory> proto_factory;
    const Message* proto_prototype = nullptr;

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

static unique_ptr<FunctionData> NatsScanBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
    if (input.inputs.empty()) {
        throw std::runtime_error("nats_scan requires at least one argument: stream_name");
    }

    auto stream_name = input.inputs[0].GetValue<string>();

    string subject_filter = "";
    string nats_url = "nats://localhost:4222";
    uint64_t start_seq = 0;
    uint64_t end_seq = UINT64_MAX;
    int64_t start_time = 0;
    int64_t end_time = 0;
    vector<string> json_fields;
    string proto_file = "";
    string proto_message = "";
    vector<string> proto_fields;

    for (auto &kv : input.named_parameters) {
        if (kv.first == "subject") {
            subject_filter = StringValue::Get(kv.second);
        } else if (kv.first == "url") {
            nats_url = StringValue::Get(kv.second);
        } else if (kv.first == "start_seq") {
            start_seq = UBigIntValue::Get(kv.second);
        } else if (kv.first == "end_seq") {
            end_seq = UBigIntValue::Get(kv.second);
        } else if (kv.first == "start_time") {
            timestamp_t ts = TimestampValue::Get(kv.second);
            start_time = ts.value * 1000; // DuckDB microseconds -> nanoseconds
        } else if (kv.first == "end_time") {
            timestamp_t ts = TimestampValue::Get(kv.second);
            end_time = ts.value * 1000;
        } else if (kv.first == "json_extract") {
            auto list_children = ListValue::GetChildren(kv.second);
            for (auto &child : list_children) {
                json_fields.push_back(StringValue::Get(child));
            }
        } else if (kv.first == "proto_file") {
            proto_file = StringValue::Get(kv.second);
        } else if (kv.first == "proto_message") {
            proto_message = StringValue::Get(kv.second);
        } else if (kv.first == "proto_extract") {
            auto list_children = ListValue::GetChildren(kv.second);
            for (auto &child : list_children) {
                proto_fields.push_back(StringValue::Get(child));
            }
        }
    }

    if ((start_seq > 0 || end_seq != UINT64_MAX) && (start_time > 0 || end_time > 0)) {
        throw std::runtime_error("Cannot mix sequence-based (start_seq/end_seq) and time-based (start_time/end_time) parameters");
    }

    if (!json_fields.empty() && !proto_fields.empty()) {
        throw std::runtime_error("Cannot use both json_extract and proto_extract parameters");
    }

    if (!proto_fields.empty()) {
        if (proto_file.empty()) {
            throw std::runtime_error("proto_file parameter is required when using proto_extract");
        }
        if (proto_message.empty()) {
            throw std::runtime_error("proto_message parameter is required when using proto_extract");
        }
    }

    shared_ptr<DiskSourceTree> source_tree;
    shared_ptr<ProtobufErrorCollector> error_collector;
    shared_ptr<Importer> importer;
    const Descriptor* descriptor = nullptr;

    if (!proto_fields.empty()) {
        source_tree = make_shared_ptr<DiskSourceTree>();

        std::filesystem::path proto_path(proto_file);
        string proto_dir = proto_path.parent_path().string();
        string proto_filename = proto_path.filename().string();

        if (proto_dir.empty()) {
            proto_dir = ".";
        }

        source_tree->MapPath("", proto_dir);

        error_collector = make_shared_ptr<ProtobufErrorCollector>();
        importer = make_shared_ptr<Importer>(source_tree.get(), error_collector.get());

        const FileDescriptor* file_desc = importer->Import(proto_filename);
        if (!file_desc) {
            string error_msg = "Failed to import protobuf schema file: " + proto_file;
            if (error_collector->HasErrors()) {
                error_msg += "\n" + error_collector->GetErrors();
            }
            throw std::runtime_error(error_msg);
        }

        descriptor = file_desc->FindMessageTypeByName(proto_message);
        if (!descriptor) {
            throw std::runtime_error("Message type '" + proto_message + "' not found in " + proto_file);
        }

        // Validate that all requested fields exist in the schema
        for (const auto &field_path : proto_fields) {
            auto path_parts = SplitFieldPath(field_path);
            const Descriptor* current_desc = descriptor;
            for (size_t i = 0; i < path_parts.size(); i++) {
                const FieldDescriptor* field = current_desc->FindFieldByName(path_parts[i]);
                if (!field) {
                    throw std::runtime_error("Field '" + path_parts[i] + "' not found in message type '" +
                                           string(current_desc->name()) + "' (field path: " + field_path + ")");
                }
                if (i < path_parts.size() - 1) {
                    if (field->type() != FieldDescriptor::TYPE_MESSAGE) {
                        throw std::runtime_error("Field '" + path_parts[i] + "' is not a message type, cannot navigate to '" +
                                               path_parts[i+1] + "' (field path: " + field_path + ")");
                    }
                    current_desc = field->message_type();
                }
            }
        }
    }

    names.emplace_back("stream");
    return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
    names.emplace_back("subject");
    return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
    names.emplace_back("seq");
    return_types.emplace_back(LogicalType(LogicalTypeId::UBIGINT));
    names.emplace_back("ts_nats");
    return_types.emplace_back(LogicalType(LogicalTypeId::TIMESTAMP));

    // BLOB unless json_extract is specified (known valid UTF-8), to avoid validation errors on binary data
    names.emplace_back("payload");
    if (!proto_fields.empty() || json_fields.empty()) {
        return_types.emplace_back(LogicalType(LogicalTypeId::BLOB));
    } else {
        return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
    }

    for (const auto &field : json_fields) {
        names.emplace_back(field);
        return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
    }

    // Protobuf columns: dot notation -> underscores, types from schema
    for (const auto &field_path : proto_fields) {
        string column_name = field_path;
        std::replace(column_name.begin(), column_name.end(), '.', '_');
        names.emplace_back(column_name);

        const FieldDescriptor* field_desc = GetFieldDescriptorForPath(descriptor, field_path);
        if (field_desc) {
            return_types.emplace_back(ProtobufTypeToDuckDBType(field_desc));
        } else {
            return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
        }
    }

    auto bind_data = make_uniq<NatsScanBindData>(stream_name, subject_filter, nats_url, start_seq, end_seq,
                                                  start_time, end_time, json_fields, proto_file, proto_message, proto_fields);

    if (!proto_fields.empty()) {
        bind_data->proto_source_tree = source_tree;
        bind_data->proto_error_collector = error_collector;
        bind_data->proto_importer = importer;
        bind_data->proto_descriptor = descriptor;
    }

    return bind_data;
}

// Binary search for the first sequence at or after the given timestamp.
static uint64_t ResolveTimestampToSequence(jsCtx *js, const char *stream_name,
                                           int64_t timestamp_ns,
                                           uint64_t first_seq, uint64_t last_seq) {
    natsStatus s;
    uint64_t left = first_seq;
    uint64_t right = last_seq;
    uint64_t result_seq = UINT64_MAX;

    while (left <= right) {
        uint64_t mid = left + (right - left) / 2;

        natsMsg *msg = nullptr;
        jsDirectGetMsgOptions opts;
        memset(&opts, 0, sizeof(opts));
        opts.Sequence = mid;

        s = js_DirectGetMsg(&msg, js, stream_name, nullptr, &opts);

        if (s == NATS_NOT_FOUND) {
            left = mid + 1;
            continue;
        }

        if (s != NATS_OK) {
            throw std::runtime_error(std::string("Failed to fetch message at sequence ") +
                                   std::to_string(mid) + " for timestamp resolution: " + natsStatus_GetText(s));
        }

        int64_t msg_time_ns = natsMsg_GetTime(msg);
        natsMsg_Destroy(msg);

        if (msg_time_ns >= timestamp_ns) {
            result_seq = mid;
            right = mid - 1;
        } else {
            left = mid + 1;
        }
    }

    return result_seq;
}

static unique_ptr<GlobalTableFunctionState> NatsScanInitGlobal(ClientContext &context,
                                                                 TableFunctionInitInput &input) {
    auto &bind_data = input.bind_data->Cast<NatsScanBindData>();
    auto state = make_uniq<NatsScanGlobalState>();

    state->current_seq = bind_data.start_seq > 0 ? bind_data.start_seq : 1;
    state->end_seq = bind_data.end_seq;
    state->done = false;

    if (!bind_data.proto_fields.empty() && bind_data.proto_descriptor != nullptr) {
        state->proto_factory = make_shared_ptr<DynamicMessageFactory>();
        state->proto_prototype = state->proto_factory->GetPrototype(bind_data.proto_descriptor);
    }

    // Connect to NATS and validate stream at init time (fail-fast on typos / unreachable server)
    natsOptions *opts = nullptr;
    natsStatus s = natsOptions_Create(&opts);
    if (s != NATS_OK) {
        throw std::runtime_error(std::string("Failed to create NATS options: ") + natsStatus_GetText(s));
    }

    s = natsOptions_SetTimeout(opts, 5000);
    if (s != NATS_OK) {
        natsOptions_Destroy(opts);
        throw std::runtime_error(std::string("Failed to set NATS timeout: ") + natsStatus_GetText(s));
    }

    s = natsOptions_SetURL(opts, bind_data.nats_url.c_str());
    if (s != NATS_OK) {
        natsOptions_Destroy(opts);
        throw std::runtime_error(std::string("Failed to set NATS URL: ") + natsStatus_GetText(s));
    }

    s = natsConnection_Connect(&state->conn, opts);
    natsOptions_Destroy(opts);
    if (s != NATS_OK) {
        throw std::runtime_error(std::string("Failed to connect to NATS: ") + natsStatus_GetText(s));
    }

    s = natsConnection_JetStream(&state->js, state->conn, nullptr);
    if (s != NATS_OK) {
        throw std::runtime_error(std::string("Failed to create JetStream context: ") + natsStatus_GetText(s));
    }

    s = js_GetStreamInfo(&state->stream_info, state->js, bind_data.stream_name.c_str(), nullptr, nullptr);
    if (s != NATS_OK) {
        throw std::runtime_error(std::string("Failed to get stream info for '") + bind_data.stream_name + "': " + natsStatus_GetText(s));
    }

    if (state->end_seq == UINT64_MAX) {
        state->end_seq = state->stream_info->State.LastSeq;
    }

    // Resolve timestamps to sequence numbers
    if (bind_data.start_time > 0 || bind_data.end_time > 0) {
        if (bind_data.start_time > 0) {
            uint64_t resolved_seq = ResolveTimestampToSequence(
                state->js, bind_data.stream_name.c_str(), bind_data.start_time,
                state->stream_info->State.FirstSeq, state->stream_info->State.LastSeq
            );
            if (resolved_seq == UINT64_MAX) {
                state->done = true;
                return state;
            }
            state->current_seq = resolved_seq;
        }

        if (bind_data.end_time > 0) {
            uint64_t resolved_seq = ResolveTimestampToSequence(
                state->js, bind_data.stream_name.c_str(), bind_data.end_time,
                state->stream_info->State.FirstSeq, state->stream_info->State.LastSeq
            );
            if (resolved_seq != UINT64_MAX) {
                state->end_seq = resolved_seq;
            }
        }
    }

    return state;
}

static unique_ptr<LocalTableFunctionState> NatsScanInitLocal(ExecutionContext &context,
                                                               TableFunctionInitInput &input,
                                                               GlobalTableFunctionState *global_state) {
    return make_uniq<NatsScanLocalState>();
}

static Value ExtractProtobufValue(const Message* message, const string& field_path, const Descriptor* root_descriptor) {
    auto path_parts = SplitFieldPath(field_path);
    const Message* current_message = message;
    const Descriptor* current_desc = root_descriptor;
    const Reflection* reflection = message->GetReflection();

    for (size_t i = 0; i < path_parts.size(); i++) {
        const FieldDescriptor* field = current_desc->FindFieldByName(path_parts[i]);
        if (!field) {
            return Value();
        }

        if (i < path_parts.size() - 1) {
            if (field->type() != FieldDescriptor::TYPE_MESSAGE) {
                return Value();
            }
            if (!reflection->HasField(*current_message, field)) {
                return Value();
            }
            current_message = &reflection->GetMessage(*current_message, field);
            current_desc = field->message_type();
            reflection = current_message->GetReflection();
        } else {
            // proto3 primitives always have defaults; only check message fields for presence
            if (!reflection->HasField(*current_message, field) && field->type() == FieldDescriptor::TYPE_MESSAGE) {
                return Value();
            }

            switch (field->type()) {
                case FieldDescriptor::TYPE_STRING:
                    return Value(reflection->GetString(*current_message, field));
                case FieldDescriptor::TYPE_BYTES: {
                    string bytes = reflection->GetString(*current_message, field);
                    return Value::BLOB(const_data_ptr_cast(bytes.data()), bytes.size());
                }
                case FieldDescriptor::TYPE_INT32:
                case FieldDescriptor::TYPE_SINT32:
                case FieldDescriptor::TYPE_SFIXED32:
                    return Value::INTEGER(reflection->GetInt32(*current_message, field));
                case FieldDescriptor::TYPE_INT64:
                case FieldDescriptor::TYPE_SINT64:
                case FieldDescriptor::TYPE_SFIXED64:
                    return Value::BIGINT(reflection->GetInt64(*current_message, field));
                case FieldDescriptor::TYPE_UINT32:
                case FieldDescriptor::TYPE_FIXED32:
                    return Value::UINTEGER(reflection->GetUInt32(*current_message, field));
                case FieldDescriptor::TYPE_UINT64:
                case FieldDescriptor::TYPE_FIXED64:
                    return Value::UBIGINT(reflection->GetUInt64(*current_message, field));
                case FieldDescriptor::TYPE_FLOAT:
                    return Value::FLOAT(reflection->GetFloat(*current_message, field));
                case FieldDescriptor::TYPE_DOUBLE:
                    return Value::DOUBLE(reflection->GetDouble(*current_message, field));
                case FieldDescriptor::TYPE_BOOL:
                    return Value::BOOLEAN(reflection->GetBool(*current_message, field));
                case FieldDescriptor::TYPE_ENUM: {
                    const EnumValueDescriptor* enum_val = reflection->GetEnum(*current_message, field);
                    return Value(string(enum_val->name()));
                }
                case FieldDescriptor::TYPE_MESSAGE:
                    // Nested messages should have been extracted as separate fields
                    return Value();
                default:
                    return Value();  // Unknown type - return NULL
            }
        }
    }

    return Value();  // Should not reach here
}

static void NatsScanExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    auto &bind_data = data_p.bind_data->Cast<NatsScanBindData>();
    auto &global_state = data_p.global_state->Cast<NatsScanGlobalState>();

    if (global_state.done || global_state.current_seq > global_state.end_seq) {
        global_state.done = true;
        output.SetCardinality(0);
        return;
    }

    idx_t count = 0;
    const idx_t max_rows = STANDARD_VECTOR_SIZE;

    unique_ptr<Message> proto_msg;
    if (!bind_data.proto_fields.empty() && global_state.proto_prototype != nullptr) {
        proto_msg.reset(global_state.proto_prototype->New());
    }

    // Get vector references for direct writes (avoids per-cell virtual dispatch via SetValue)
    auto stream_data = FlatVector::GetData<string_t>(output.data[0]);
    auto subject_data = FlatVector::GetData<string_t>(output.data[1]);
    auto seq_data = FlatVector::GetData<uint64_t>(output.data[2]);
    auto ts_data = FlatVector::GetData<timestamp_t>(output.data[3]);
    auto payload_data = FlatVector::GetData<string_t>(output.data[4]);
    bool payload_is_blob = !bind_data.proto_fields.empty() || bind_data.json_fields.empty();

    while (count < max_rows && global_state.current_seq <= global_state.end_seq) {
        if (context.interrupted) {
            break;
        }

        natsMsg *msg = nullptr;
        jsDirectGetMsgOptions opts;
        memset(&opts, 0, sizeof(opts));
        opts.Sequence = global_state.current_seq;

        natsStatus s = js_DirectGetMsg(&msg, global_state.js,
                                       bind_data.stream_name.c_str(), nullptr, &opts);

        if (s == NATS_NOT_FOUND) {
            global_state.current_seq++;
            continue;
        }

        if (s != NATS_OK) {
            throw std::runtime_error(std::string("Failed to fetch message at sequence ") +
                                   std::to_string(global_state.current_seq) + ": " + natsStatus_GetText(s));
        }

        const char *subject = natsMsg_GetSubject(msg);

        if (!bind_data.subject_filter.empty() &&
            string(subject).find(bind_data.subject_filter) == string::npos) {
            natsMsg_Destroy(msg);
            global_state.current_seq++;
            continue;
        }

        int64_t timestamp_us = natsMsg_GetTime(msg) / 1000; // nanos -> micros
        const char *data = natsMsg_GetData(msg);
        int data_len = natsMsg_GetDataLength(msg);

        stream_data[count] = StringVector::AddString(output.data[0], bind_data.stream_name);
        subject_data[count] = StringVector::AddString(output.data[1], subject);
        seq_data[count] = global_state.current_seq;
        ts_data[count] = timestamp_t(timestamp_us);

        if (payload_is_blob) {
            payload_data[count] = StringVector::AddStringOrBlob(output.data[4], data, data_len);
        } else {
            payload_data[count] = StringVector::AddString(output.data[4], data, data_len);
        }

        if (!bind_data.json_fields.empty()) {
            yyjson_doc *doc = yyjson_read(data, data_len, 0);

            if (doc) {
                yyjson_val *root = yyjson_doc_get_root(doc);

                for (size_t i = 0; i < bind_data.json_fields.size(); i++) {
                    idx_t col_idx = 5 + i;
                    auto &vec = output.data[col_idx];
                    auto vec_data = FlatVector::GetData<string_t>(vec);
                    yyjson_val *field_val = yyjson_obj_get(root, bind_data.json_fields[i].c_str());

                    if (field_val) {
                        if (yyjson_is_str(field_val)) {
                            vec_data[count] = StringVector::AddString(vec, yyjson_get_str(field_val));
                        } else if (yyjson_is_num(field_val)) {
                            auto s = std::to_string(yyjson_get_num(field_val));
                            vec_data[count] = StringVector::AddString(vec, s);
                        } else if (yyjson_is_bool(field_val)) {
                            vec_data[count] = StringVector::AddString(vec, yyjson_get_bool(field_val) ? "true" : "false");
                        } else if (yyjson_is_null(field_val)) {
                            FlatVector::SetNull(vec, count, true);
                        } else {
                            char *json_str = yyjson_val_write(field_val, 0, nullptr);
                            if (json_str) {
                                vec_data[count] = StringVector::AddString(vec, json_str);
                                free(json_str);
                            } else {
                                FlatVector::SetNull(vec, count, true);
                            }
                        }
                    } else {
                        FlatVector::SetNull(vec, count, true);
                    }
                }

                yyjson_doc_free(doc);
            } else {
                for (size_t i = 0; i < bind_data.json_fields.size(); i++) {
                    FlatVector::SetNull(output.data[5 + i], count, true);
                }
            }
        }

        if (proto_msg) {
            proto_msg->Clear();
            bool parse_success = proto_msg->ParseFromArray(data, data_len);

            if (parse_success) {
                for (size_t i = 0; i < bind_data.proto_fields.size(); i++) {
                    idx_t col_idx = 5 + i;
                    Value field_value = ExtractProtobufValue(proto_msg.get(), bind_data.proto_fields[i], bind_data.proto_descriptor);
                    output.SetValue(col_idx, count, field_value);
                }
            } else {
                for (size_t i = 0; i < bind_data.proto_fields.size(); i++) {
                    FlatVector::SetNull(output.data[5 + i], count, true);
                }
            }
        }

        natsMsg_Destroy(msg);

        count++;
        global_state.current_seq++;
    }

    if (global_state.current_seq > global_state.end_seq) {
        global_state.done = true;
    }

    output.SetCardinality(count);
}

void NatsScanFunction::Register(ExtensionLoader &loader) {
    TableFunction nats_scan("nats_scan", {LogicalType(LogicalTypeId::VARCHAR)}, NatsScanExecute, NatsScanBind,
                            NatsScanInitGlobal, NatsScanInitLocal);

    nats_scan.named_parameters["subject"] = LogicalType(LogicalTypeId::VARCHAR);
    nats_scan.named_parameters["url"] = LogicalType(LogicalTypeId::VARCHAR);
    nats_scan.named_parameters["start_seq"] = LogicalType(LogicalTypeId::UBIGINT);
    nats_scan.named_parameters["end_seq"] = LogicalType(LogicalTypeId::UBIGINT);
    nats_scan.named_parameters["start_time"] = LogicalType(LogicalTypeId::TIMESTAMP);
    nats_scan.named_parameters["end_time"] = LogicalType(LogicalTypeId::TIMESTAMP);
    nats_scan.named_parameters["json_extract"] = LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR));
    nats_scan.named_parameters["proto_file"] = LogicalType(LogicalTypeId::VARCHAR);
    nats_scan.named_parameters["proto_message"] = LogicalType(LogicalTypeId::VARCHAR);
    nats_scan.named_parameters["proto_extract"] = LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR));

    loader.RegisterFunction(nats_scan);
}

} // namespace duckdb

