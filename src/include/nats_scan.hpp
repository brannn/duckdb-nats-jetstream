#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {

class ExtensionLoader;

class NatsScanFunction {
public:
    static void Register(ExtensionLoader &loader);
};

} // namespace duckdb

