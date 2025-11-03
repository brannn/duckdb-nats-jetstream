#include "nats_scan.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/common/types/timestamp.hpp"
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

// Error collector for protobuf schema parsing
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

// Bind data structure to hold connection and stream information
struct NatsScanBindData : public TableFunctionData {
    string stream_name;
    string subject_filter;
    string nats_url;
    uint64_t start_seq;
    uint64_t end_seq;
    int64_t start_time;  // Nanoseconds since epoch, 0 means not set
    int64_t end_time;    // Nanoseconds since epoch, 0 means not set
    vector<string> json_fields;  // JSON fields to extract
    string proto_file;           // Path to .proto file
    string proto_message;        // Protobuf message type name
    vector<string> proto_fields; // Protobuf field paths to extract (with dot notation)

    // Protobuf schema objects (must be kept alive for the query duration)
    shared_ptr<DiskSourceTree> proto_source_tree;
    shared_ptr<ProtobufErrorCollector> proto_error_collector;
    shared_ptr<Importer> proto_importer;
    const Descriptor* proto_descriptor = nullptr;  // Owned by importer's descriptor pool

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

// Helper function to get the FieldDescriptor for a field path
static const FieldDescriptor* GetFieldDescriptorForPath(const Descriptor* message_desc, const string& field_path) {
    // Parse field path (e.g., "location.zone")
    vector<string> path_parts;
    size_t start = 0;
    size_t end = field_path.find('.');

    while (end != string::npos) {
        path_parts.push_back(field_path.substr(start, end - start));
        start = end + 1;
        end = field_path.find('.', start);
    }
    path_parts.push_back(field_path.substr(start));

    // Navigate through nested messages to find the final field
    const Descriptor* current_desc = message_desc;
    const FieldDescriptor* field = nullptr;

    for (size_t i = 0; i < path_parts.size(); i++) {
        field = current_desc->FindFieldByName(path_parts[i]);
        if (!field) {
            return nullptr;  // Field not found
        }

        // If not the last part, navigate to nested message
        if (i < path_parts.size() - 1) {
            if (field->type() != FieldDescriptor::TYPE_MESSAGE) {
                return nullptr;  // Can't navigate into non-message field
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

// Global state for the scan operation
struct NatsScanGlobalState : public GlobalTableFunctionState {
    natsConnection *conn = nullptr;
    jsCtx *js = nullptr;
    jsStreamInfo *stream_info = nullptr;
    uint64_t current_seq = 0;
    uint64_t end_seq = 0;
    bool done = false;
    bool timestamps_resolved = false;  // Track if we've resolved timestamps to sequences

    // Protobuf message factory (created once, reused for all messages)
    shared_ptr<DynamicMessageFactory> proto_factory;
    const Message* proto_prototype = nullptr;  // Owned by factory

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

// Local state for each thread
struct NatsScanLocalState : public LocalTableFunctionState {
};

// Bind function - validates parameters and creates bind data
static unique_ptr<FunctionData> NatsScanBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
    // Required parameters
    if (input.inputs.empty()) {
        throw std::runtime_error("nats_scan requires at least one argument: stream_name");
    }

    auto stream_name = input.inputs[0].GetValue<string>();

    // Optional named parameters with defaults
    string subject_filter = "";
    string nats_url = "nats://localhost:4222";
    uint64_t start_seq = 0;
    uint64_t end_seq = UINT64_MAX;
    int64_t start_time = 0;  // 0 means not set
    int64_t end_time = 0;    // 0 means not set
    vector<string> json_fields;  // JSON fields to extract
    string proto_file = "";      // Path to .proto file
    string proto_message = "";   // Protobuf message type name
    vector<string> proto_fields; // Protobuf field paths to extract

    // Check for named parameters
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
            // Convert DuckDB timestamp (microseconds) to nanoseconds
            timestamp_t ts = TimestampValue::Get(kv.second);
            start_time = ts.value * 1000;  // Convert microseconds to nanoseconds
        } else if (kv.first == "end_time") {
            // Convert DuckDB timestamp (microseconds) to nanoseconds
            timestamp_t ts = TimestampValue::Get(kv.second);
            end_time = ts.value * 1000;  // Convert microseconds to nanoseconds
        } else if (kv.first == "json_extract") {
            // Extract list of field names
            auto &list_value = kv.second;
            auto list_children = ListValue::GetChildren(list_value);
            for (auto &child : list_children) {
                json_fields.push_back(StringValue::Get(child));
            }
        } else if (kv.first == "proto_file") {
            proto_file = StringValue::Get(kv.second);
        } else if (kv.first == "proto_message") {
            proto_message = StringValue::Get(kv.second);
        } else if (kv.first == "proto_extract") {
            // Extract list of field paths
            auto &list_value = kv.second;
            auto list_children = ListValue::GetChildren(list_value);
            for (auto &child : list_children) {
                proto_fields.push_back(StringValue::Get(child));
            }
        }
    }

    // Validate that sequence and time parameters are not mixed
    if ((start_seq > 0 || end_seq != UINT64_MAX) && (start_time > 0 || end_time > 0)) {
        throw std::runtime_error("Cannot mix sequence-based (start_seq/end_seq) and time-based (start_time/end_time) parameters");
    }

    // Validate that json_extract and proto_extract are not both specified
    if (!json_fields.empty() && !proto_fields.empty()) {
        throw std::runtime_error("Cannot use both json_extract and proto_extract parameters");
    }

    // Validate protobuf parameters
    if (!proto_fields.empty()) {
        if (proto_file.empty()) {
            throw std::runtime_error("proto_file parameter is required when using proto_extract");
        }
        if (proto_message.empty()) {
            throw std::runtime_error("proto_message parameter is required when using proto_extract");
        }
    }

    // Parse protobuf schema if proto_extract is specified
    shared_ptr<DiskSourceTree> source_tree;
    shared_ptr<ProtobufErrorCollector> error_collector;
    shared_ptr<Importer> importer;
    const Descriptor* descriptor = nullptr;

    if (!proto_fields.empty()) {
        // Set up source tree to find .proto files
        source_tree = make_shared_ptr<DiskSourceTree>();

        // Get directory and filename from proto_file path
        std::filesystem::path proto_path(proto_file);
        string proto_dir = proto_path.parent_path().string();
        string proto_filename = proto_path.filename().string();

        // If no directory specified, use current directory
        if (proto_dir.empty()) {
            proto_dir = ".";
        }

        // Map empty virtual path to the directory containing the .proto file
        source_tree->MapPath("", proto_dir);

        // Set up error collector and importer
        error_collector = make_shared_ptr<ProtobufErrorCollector>();
        importer = make_shared_ptr<Importer>(source_tree.get(), error_collector.get());

        // Import the .proto file
        const FileDescriptor* file_desc = importer->Import(proto_filename);
        if (!file_desc) {
            string error_msg = "Failed to import protobuf schema file: " + proto_file;
            if (error_collector->HasErrors()) {
                error_msg += "\n" + error_collector->GetErrors();
            }
            throw std::runtime_error(error_msg);
        }

        // Find the message type
        descriptor = file_desc->FindMessageTypeByName(proto_message);
        if (!descriptor) {
            throw std::runtime_error("Message type '" + proto_message + "' not found in " + proto_file);
        }

        // Validate that all requested fields exist in the schema
        for (const auto &field_path : proto_fields) {
            // Parse field path (e.g., "location.zone")
            vector<string> path_parts;
            size_t start = 0;
            size_t end = field_path.find('.');

            while (end != string::npos) {
                path_parts.push_back(field_path.substr(start, end - start));
                start = end + 1;
                end = field_path.find('.', start);
            }
            path_parts.push_back(field_path.substr(start));

            // Navigate through nested messages to validate the path
            const Descriptor* current_desc = descriptor;
            for (size_t i = 0; i < path_parts.size(); i++) {
                const FieldDescriptor* field = current_desc->FindFieldByName(path_parts[i]);
                if (!field) {
                    throw std::runtime_error("Field '" + path_parts[i] + "' not found in message type '" +
                                           string(current_desc->name()) + "' (field path: " + field_path + ")");
                }

                // If not the last part, must be a nested message
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

    // Define return schema
    names.emplace_back("stream");
    return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));

    names.emplace_back("subject");
    return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));

    names.emplace_back("seq");
    return_types.emplace_back(LogicalType(LogicalTypeId::UBIGINT));

    names.emplace_back("ts_nats");
    return_types.emplace_back(LogicalType(LogicalTypeId::TIMESTAMP));

    names.emplace_back("payload");
    // Use BLOB for payload when using protobuf (binary data), VARCHAR for JSON
    if (!proto_fields.empty()) {
        return_types.emplace_back(LogicalType(LogicalTypeId::BLOB));
    } else {
        return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
    }

    // Add JSON field columns if json_extract is specified
    for (const auto &field : json_fields) {
        names.emplace_back(field);
        return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
    }

    // Add protobuf field columns if proto_extract is specified
    // Convert dot notation to underscores for column names
    // Determine actual DuckDB types from protobuf field types
    for (const auto &field_path : proto_fields) {
        string column_name = field_path;
        std::replace(column_name.begin(), column_name.end(), '.', '_');
        names.emplace_back(column_name);

        // Get the field descriptor and determine the DuckDB type
        const FieldDescriptor* field_desc = GetFieldDescriptorForPath(descriptor, field_path);
        if (field_desc) {
            return_types.emplace_back(ProtobufTypeToDuckDBType(field_desc));
        } else {
            // Should not happen since we validated fields earlier, but default to VARCHAR
            return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
        }
    }

    auto bind_data = make_uniq<NatsScanBindData>(stream_name, subject_filter, nats_url, start_seq, end_seq,
                                                  start_time, end_time, json_fields, proto_file, proto_message, proto_fields);

    // Store protobuf schema objects in bind data
    if (!proto_fields.empty()) {
        bind_data->proto_source_tree = source_tree;
        bind_data->proto_error_collector = error_collector;
        bind_data->proto_importer = importer;
        bind_data->proto_descriptor = descriptor;
    }

    return bind_data;
}

// Init global state
static unique_ptr<GlobalTableFunctionState> NatsScanInitGlobal(ClientContext &context,
                                                                 TableFunctionInitInput &input) {
    auto &bind_data = input.bind_data->Cast<NatsScanBindData>();
    auto state = make_uniq<NatsScanGlobalState>();

    // Initialize sequence range from bind data
    state->current_seq = bind_data.start_seq > 0 ? bind_data.start_seq : 1;

    // If end_seq is not specified (UINT64_MAX), we'll determine it when we connect
    // Otherwise use the user-specified value
    state->end_seq = bind_data.end_seq;
    state->done = false;

    // Initialize protobuf factory if proto_extract is specified
    if (!bind_data.proto_fields.empty() && bind_data.proto_descriptor != nullptr) {
        state->proto_factory = make_shared_ptr<DynamicMessageFactory>();
        state->proto_prototype = state->proto_factory->GetPrototype(bind_data.proto_descriptor);
    }

    return state;
}

// Init local state
static unique_ptr<LocalTableFunctionState> NatsScanInitLocal(ExecutionContext &context,
                                                               TableFunctionInitInput &input,
                                                               GlobalTableFunctionState *global_state) {
    return make_uniq<NatsScanLocalState>();
}

// Helper function to extract a protobuf field value and convert to DuckDB Value
static Value ExtractProtobufValue(const Message* message, const string& field_path, const Descriptor* root_descriptor) {
    // Parse field path (e.g., "location.zone")
    vector<string> path_parts;
    size_t start = 0;
    size_t end = field_path.find('.');

    while (end != string::npos) {
        path_parts.push_back(field_path.substr(start, end - start));
        start = end + 1;
        end = field_path.find('.', start);
    }
    path_parts.push_back(field_path.substr(start));

    // Navigate through nested messages to find the final field
    const Message* current_message = message;
    const Descriptor* current_desc = root_descriptor;
    const Reflection* reflection = message->GetReflection();

    for (size_t i = 0; i < path_parts.size(); i++) {
        const FieldDescriptor* field = current_desc->FindFieldByName(path_parts[i]);
        if (!field) {
            return Value();  // Field not found - return NULL
        }

        // If not the last part, navigate to nested message
        if (i < path_parts.size() - 1) {
            if (field->type() != FieldDescriptor::TYPE_MESSAGE) {
                return Value();  // Can't navigate into non-message field - return NULL
            }

            // Check if the nested message field is set
            if (!reflection->HasField(*current_message, field)) {
                return Value();  // Nested message not set - return NULL
            }

            // Get the nested message
            current_message = &reflection->GetMessage(*current_message, field);
            current_desc = field->message_type();
            reflection = current_message->GetReflection();
        } else {
            // Last part - extract the value
            // Check if field is set (for proto3, primitive fields are always "set" with default values)
            if (!reflection->HasField(*current_message, field) && field->type() == FieldDescriptor::TYPE_MESSAGE) {
                return Value();  // Message field not set - return NULL
            }

            // Extract value based on type
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

// Helper function to resolve a timestamp to a sequence number using binary search
// Searches through messages to find the first one at or after the given timestamp
static uint64_t ResolveTimestampToSequence(jsCtx *js, const char *stream_name,
                                           int64_t timestamp_ns,
                                           uint64_t first_seq, uint64_t last_seq) {
    natsStatus s;
    uint64_t left = first_seq;
    uint64_t right = last_seq;
    uint64_t result_seq = UINT64_MAX;  // Default: no message found

    // Binary search for the first message at or after the timestamp
    while (left <= right) {
        uint64_t mid = left + (right - left) / 2;

        // Fetch message at mid sequence
        natsMsg *msg = nullptr;
        jsDirectGetMsgOptions opts;
        memset(&opts, 0, sizeof(opts));
        opts.Sequence = mid;

        s = js_DirectGetMsg(&msg, js, stream_name, nullptr, &opts);

        if (s == NATS_NOT_FOUND) {
            // Message not found at this sequence, try next
            left = mid + 1;
            continue;
        }

        if (s != NATS_OK) {
            throw std::runtime_error(std::string("Failed to fetch message at sequence ") +
                                   std::to_string(mid) + " for timestamp resolution: " + natsStatus_GetText(s));
        }

        // Get message timestamp
        int64_t msg_time_ns = natsMsg_GetTime(msg);

        natsMsg_Destroy(msg);

        if (msg_time_ns >= timestamp_ns) {
            // This message is at or after the target timestamp
            result_seq = mid;
            right = mid - 1;  // Try to find an earlier one
        } else {
            // This message is before the target timestamp
            left = mid + 1;
        }
    }

    return result_seq;
}

// Main scan function - retrieves data from NATS
static void NatsScanExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    auto &bind_data = data_p.bind_data->Cast<NatsScanBindData>();
    auto &global_state = data_p.global_state->Cast<NatsScanGlobalState>();
    auto &local_state = data_p.local_state->Cast<NatsScanLocalState>();

    // If we're done, return empty chunk
    if (global_state.done) {
        output.SetCardinality(0);
        return;
    }

    // Create connection if it doesn't exist
    if (global_state.conn == nullptr) {
        natsOptions *opts = nullptr;
        natsStatus s = natsOptions_Create(&opts);
        if (s != NATS_OK) {
            throw std::runtime_error(std::string("Failed to create NATS options: ") + natsStatus_GetText(s));
        }

        // Set connection timeout to 5 seconds
        s = natsOptions_SetTimeout(opts, 5000); // 5000 milliseconds
        if (s != NATS_OK) {
            natsOptions_Destroy(opts);
            throw std::runtime_error(std::string("Failed to set NATS timeout: ") + natsStatus_GetText(s));
        }

        s = natsOptions_SetURL(opts, bind_data.nats_url.c_str());
        if (s != NATS_OK) {
            natsOptions_Destroy(opts);
            throw std::runtime_error(std::string("Failed to set NATS URL: ") + natsStatus_GetText(s));
        }

        s = natsConnection_Connect(&global_state.conn, opts);
        natsOptions_Destroy(opts);

        if (s != NATS_OK) {
            throw std::runtime_error(std::string("Failed to connect to NATS: ") + natsStatus_GetText(s));
        }
    }

    // Create JetStream context if it doesn't exist
    if (global_state.js == nullptr) {
        natsStatus s = natsConnection_JetStream(&global_state.js, global_state.conn, nullptr);

        if (s != NATS_OK) {
            throw std::runtime_error(std::string("Failed to create JetStream context: ") + natsStatus_GetText(s));
        }

        // Get stream info (needed for end_seq and timestamp resolution)
        if (global_state.stream_info == nullptr) {
            s = js_GetStreamInfo(&global_state.stream_info, global_state.js, bind_data.stream_name.c_str(), nullptr, nullptr);

            if (s != NATS_OK) {
                throw std::runtime_error(std::string("Failed to get stream info: ") + natsStatus_GetText(s));
            }
        }

        // Set end_seq if not specified
        if (global_state.end_seq == UINT64_MAX) {
            global_state.end_seq = global_state.stream_info->State.LastSeq;
        }
    }

    // Resolve timestamps to sequences if needed (only once)
    if (!global_state.timestamps_resolved && (bind_data.start_time > 0 || bind_data.end_time > 0)) {
        // Resolve start_time to start sequence
        if (bind_data.start_time > 0) {
            uint64_t resolved_seq = ResolveTimestampToSequence(
                global_state.js,
                bind_data.stream_name.c_str(),
                bind_data.start_time,
                global_state.stream_info->State.FirstSeq,
                global_state.stream_info->State.LastSeq
            );

            // If resolved_seq is UINT64_MAX, it means no messages exist at or after this timestamp
            if (resolved_seq == UINT64_MAX) {
                global_state.done = true;
                global_state.timestamps_resolved = true;
                output.SetCardinality(0);
                return;
            }

            global_state.current_seq = resolved_seq;
        }

        // Resolve end_time to end sequence
        if (bind_data.end_time > 0) {
            uint64_t resolved_seq = ResolveTimestampToSequence(
                global_state.js,
                bind_data.stream_name.c_str(),
                bind_data.end_time,
                global_state.stream_info->State.FirstSeq,
                global_state.stream_info->State.LastSeq
            );

            // If resolved_seq is UINT64_MAX, use the last sequence in the stream
            if (resolved_seq != UINT64_MAX) {
                global_state.end_seq = resolved_seq;
            }
        }

        global_state.timestamps_resolved = true;
    }

    if (global_state.current_seq > global_state.end_seq) {
        global_state.done = true;
        output.SetCardinality(0);
        return;
    }

    idx_t count = 0;
    const idx_t max_rows = STANDARD_VECTOR_SIZE;

    // Fetch messages one at a time up to max_rows
    while (count < max_rows && global_state.current_seq <= global_state.end_seq) {
        // Fetch message by sequence number
        natsMsg *msg = nullptr;

        // Use direct get to fetch message by sequence
        jsDirectGetMsgOptions opts;
        memset(&opts, 0, sizeof(opts));
        opts.Sequence = global_state.current_seq;

        natsStatus s = js_DirectGetMsg(&msg, global_state.js,
                                       bind_data.stream_name.c_str(), nullptr, &opts);

        if (s == NATS_NOT_FOUND) {
            // Message not found at this sequence, skip to next
            global_state.current_seq++;
            continue;
        }

        if (s != NATS_OK) {
            // Other error - throw exception
            throw std::runtime_error(std::string("Failed to fetch message at sequence ") +
                                   std::to_string(global_state.current_seq) + ": " + natsStatus_GetText(s));
        }

        // For direct get messages, extract basic message info
        const char *subject = natsMsg_GetSubject(msg);

        // Check if subject matches filter (if specified)
        if (!bind_data.subject_filter.empty() &&
            string(subject).find(bind_data.subject_filter) == string::npos) {
            natsMsg_Destroy(msg);
            global_state.current_seq++;
            continue;
        }

        // Get message timestamp and convert from nanoseconds to microseconds
        int64_t timestamp_us = natsMsg_GetTime(msg) / 1000;

        // Column 0: stream
        output.SetValue(0, count, Value(bind_data.stream_name));

        // Column 1: subject
        output.SetValue(1, count, Value(subject));

        // Column 2: seq
        output.SetValue(2, count, Value::UBIGINT(global_state.current_seq));

        // Column 3: ts_nats
        output.SetValue(3, count, Value::TIMESTAMP(timestamp_t(timestamp_us)));

        // Column 4: payload (raw bytes)
        const char *data = natsMsg_GetData(msg);
        int data_len = natsMsg_GetDataLength(msg);

        // Use BLOB for protobuf (binary data), VARCHAR for JSON/text
        if (!bind_data.proto_fields.empty()) {
            output.SetValue(4, count, Value::BLOB(const_data_ptr_cast(data), data_len));
        } else {
            string payload_str(data, data_len);
            output.SetValue(4, count, Value(payload_str));
        }

        // Extract JSON fields if requested
        if (!bind_data.json_fields.empty()) {
            // Parse JSON payload
            yyjson_doc *doc = yyjson_read(data, data_len, 0);

            if (doc) {
                yyjson_val *root = yyjson_doc_get_root(doc);

                // Extract each requested field
                for (size_t i = 0; i < bind_data.json_fields.size(); i++) {
                    const char *field_name = bind_data.json_fields[i].c_str();
                    yyjson_val *field_val = yyjson_obj_get(root, field_name);

                    // Column index is 5 + i (after stream, subject, seq, ts_nats, payload)
                    idx_t col_idx = 5 + i;

                    if (field_val) {
                        // Convert value to string based on type
                        if (yyjson_is_str(field_val)) {
                            const char *str_val = yyjson_get_str(field_val);
                            output.SetValue(col_idx, count, Value(str_val));
                        } else if (yyjson_is_num(field_val)) {
                            // Convert number to string
                            double num_val = yyjson_get_num(field_val);
                            output.SetValue(col_idx, count, Value(std::to_string(num_val)));
                        } else if (yyjson_is_bool(field_val)) {
                            bool bool_val = yyjson_get_bool(field_val);
                            output.SetValue(col_idx, count, Value(bool_val ? "true" : "false"));
                        } else if (yyjson_is_null(field_val)) {
                            output.SetValue(col_idx, count, Value());  // NULL
                        } else {
                            // For objects/arrays, convert to JSON string
                            char *json_str = yyjson_val_write(field_val, 0, nullptr);
                            if (json_str) {
                                output.SetValue(col_idx, count, Value(json_str));
                                free(json_str);
                            } else {
                                output.SetValue(col_idx, count, Value());  // NULL on error
                            }
                        }
                    } else {
                        // Field not found - set to NULL
                        output.SetValue(col_idx, count, Value());
                    }
                }

                yyjson_doc_free(doc);
            } else {
                // JSON parsing failed - set all JSON fields to NULL
                for (size_t i = 0; i < bind_data.json_fields.size(); i++) {
                    idx_t col_idx = 5 + i;
                    output.SetValue(col_idx, count, Value());
                }
            }
        }

        // Extract protobuf fields if requested
        if (!bind_data.proto_fields.empty() && global_state.proto_prototype != nullptr) {
            // Create a new message instance
            Message* proto_message = global_state.proto_prototype->New();

            // Parse the protobuf message from the payload
            bool parse_success = proto_message->ParseFromArray(data, data_len);

            if (parse_success) {
                // Extract each requested field
                for (size_t i = 0; i < bind_data.proto_fields.size(); i++) {
                    const string& field_path = bind_data.proto_fields[i];

                    // Column index is 5 + i (after stream, subject, seq, ts_nats, payload)
                    idx_t col_idx = 5 + i;

                    // Extract the value
                    Value field_value = ExtractProtobufValue(proto_message, field_path, bind_data.proto_descriptor);
                    output.SetValue(col_idx, count, field_value);
                }
            } else {
                // Protobuf parsing failed - set all protobuf fields to NULL
                for (size_t i = 0; i < bind_data.proto_fields.size(); i++) {
                    idx_t col_idx = 5 + i;
                    output.SetValue(col_idx, count, Value());
                }
            }

            // Clean up the message
            delete proto_message;
        }

        // Clean up
        natsMsg_Destroy(msg);

        count++;
        global_state.current_seq++;
    }

    // Mark as done if we've reached the end
    if (global_state.current_seq > global_state.end_seq) {
        global_state.done = true;
    }

    output.SetCardinality(count);
}

void NatsScanFunction::Register(ExtensionLoader &loader) {
    TableFunction nats_scan("nats_scan", {LogicalType(LogicalTypeId::VARCHAR)}, NatsScanExecute, NatsScanBind,
                            NatsScanInitGlobal, NatsScanInitLocal);

    // Add optional parameters
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

    // Register the function using the ExtensionLoader API
    loader.RegisterFunction(nats_scan);
}

} // namespace duckdb

