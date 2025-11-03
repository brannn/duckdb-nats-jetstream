#include "nats_js_extension.hpp"
#include "nats_scan.hpp"
#include <nats/nats.h>

namespace duckdb {

void NatsJsExtension::Load(ExtensionLoader &loader) {
    // Register table functions
    NatsScanFunction::Register(loader);
}

std::string NatsJsExtension::Name() {
    return "nats_js";
}

std::string NatsJsExtension::Version() const {
    return "0.1.0";
}

} // namespace duckdb

// Extension entry point
extern "C" {

DUCKDB_EXTENSION_API void nats_js_duckdb_cpp_init(duckdb::ExtensionLoader &loader) {
    duckdb::NatsJsExtension extension;
    extension.Load(loader);
}

DUCKDB_EXTENSION_API const char *nats_js_duckdb_cpp_version() {
    return duckdb::DuckDB::LibraryVersion();
}

}

