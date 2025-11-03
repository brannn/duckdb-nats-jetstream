// Test program to verify protobuf API usage
#include <iostream>
#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/descriptor.h>

using namespace google::protobuf;
using namespace google::protobuf::compiler;

class ErrorCollector : public MultiFileErrorCollector {
public:
    void RecordError(absl::string_view filename, int line, int column, absl::string_view message) override {
        std::cerr << filename << ":" << line << ":" << column << ": " << message << std::endl;
    }
};

int main() {
    // Set up source tree
    DiskSourceTree source_tree;
    source_tree.MapPath("", "test/proto");
    
    // Set up importer
    ErrorCollector error_collector;
    Importer importer(&source_tree, &error_collector);
    
    // Import the .proto file
    const FileDescriptor* file_desc = importer.Import("telemetry.proto");
    if (!file_desc) {
        std::cerr << "Failed to import telemetry.proto" << std::endl;
        return 1;
    }
    
    std::cout << "✓ Successfully imported telemetry.proto" << std::endl;
    
    // Find the Telemetry message type
    const Descriptor* message_desc = file_desc->FindMessageTypeByName("Telemetry");
    if (!message_desc) {
        std::cerr << "Failed to find Telemetry message" << std::endl;
        return 1;
    }
    
    std::cout << "✓ Found Telemetry message type" << std::endl;
    std::cout << "  Fields: " << message_desc->field_count() << std::endl;
    
    // List all fields
    for (int i = 0; i < message_desc->field_count(); i++) {
        const FieldDescriptor* field = message_desc->field(i);
        std::cout << "    - " << field->name() << " (" << field->type_name() << ")";
        if (field->type() == FieldDescriptor::TYPE_MESSAGE) {
            std::cout << " -> " << field->message_type()->name();
        }
        std::cout << std::endl;
    }
    
    // Test nested field access
    const FieldDescriptor* location_field = message_desc->FindFieldByName("location");
    if (location_field && location_field->type() == FieldDescriptor::TYPE_MESSAGE) {
        const Descriptor* location_desc = location_field->message_type();
        std::cout << "✓ Found nested Location message" << std::endl;
        std::cout << "  Fields: " << location_desc->field_count() << std::endl;
        
        for (int i = 0; i < location_desc->field_count(); i++) {
            const FieldDescriptor* field = location_desc->field(i);
            std::cout << "    - " << field->name() << " (" << field->type_name() << ")" << std::endl;
        }
    }
    
    // Test creating a dynamic message
    DynamicMessageFactory factory;
    const Message* prototype = factory.GetPrototype(message_desc);
    Message* message = prototype->New();
    
    std::cout << "✓ Created dynamic message instance" << std::endl;
    
    // Clean up
    delete message;
    
    std::cout << "\n✓ All API tests passed!" << std::endl;
    
    return 0;
}

