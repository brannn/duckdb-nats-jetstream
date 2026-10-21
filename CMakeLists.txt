cmake_minimum_required(VERSION 3.5...3.29)

# Set extension name
set(TARGET_NAME nats_js)

# Project configuration
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

# Require C++17 for std::filesystem and other modern features
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)

# Extension sources
set(EXTENSION_SOURCES src/nats_scan.cpp src/nats_js_extension.cpp)

# Build static and loadable extensions using DuckDB's build functions
build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# Find dependencies via vcpkg
find_package(cnats CONFIG REQUIRED)
find_package(Protobuf CONFIG REQUIRED)

# Link dependencies - use plain signature to match DuckDB's build functions
# cnats exports 'cnats::nats_static' target for static library
target_link_libraries(${EXTENSION_NAME} cnats::nats_static protobuf::libprotobuf protobuf::libprotoc)
target_link_libraries(${LOADABLE_EXTENSION_NAME} cnats::nats_static protobuf::libprotobuf protobuf::libprotoc)

# Targets that link the static extension (DuckDB's shell, unittest and tools, compiled as C++11 on DuckDB 1.5) need its
# libraries only to link. Passing on protobuf's C++17 requirement makes DuckDB >= 1.5.2 fail to link tools/plan_serializer.
get_target_property(NATS_JS_INTERFACE_LIBS ${EXTENSION_NAME} INTERFACE_LINK_LIBRARIES)
list(TRANSFORM NATS_JS_INTERFACE_LIBS REPLACE "^(.+)$" "$<LINK_ONLY:\\1>")
set_target_properties(${EXTENSION_NAME} PROPERTIES INTERFACE_LINK_LIBRARIES "${NATS_JS_INTERFACE_LIBS}")

# Install static library
install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

