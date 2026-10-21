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
# cnats links OpenSSL and Threads but its CMake package does not find them
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(cnats CONFIG REQUIRED)
find_package(Protobuf CONFIG REQUIRED)

# Link dependencies - use plain signature to match DuckDB's build functions
# cnats exports 'cnats::nats_static' target for static library
# The .proto parser (compiler/importer.h) is part of libprotobuf; libprotoc holds only the code generators
target_link_libraries(${EXTENSION_NAME} cnats::nats_static protobuf::libprotobuf)
target_link_libraries(${LOADABLE_EXTENSION_NAME} cnats::nats_static protobuf::libprotobuf)

# Install static library
install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

