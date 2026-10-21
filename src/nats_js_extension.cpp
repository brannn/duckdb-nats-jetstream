#include "nats_js_extension.hpp"
#include "nats_scan.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

void NatsJsExtension::Load(ExtensionLoader &loader) {
    NatsScanFunction::Register(loader);
}

std::string NatsJsExtension::Name() {
    return "nats_js";
}

std::string NatsJsExtension::Version() const {
#ifdef EXT_VERSION_NATS_JS
    return EXT_VERSION_NATS_JS;
#else
    return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(nats_js, loader) {
    duckdb::NatsJsExtension extension;
    extension.Load(loader);
}

}
