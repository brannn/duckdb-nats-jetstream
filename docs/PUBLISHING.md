# Publishing the NATS JetStream Extension

This document outlines the requirements and process for officially publishing the `nats_js` DuckDB extension through the DuckDB Community Extensions repository.

## Overview

DuckDB extensions can be published through the **DuckDB Community Extensions Repository**, which provides:
- Automatic building for all supported platforms (Linux, macOS, Windows, WebAssembly)
- Official signing of extension binaries
- Centralized distribution via `INSTALL extension_name FROM community`
- Automatic documentation generation
- Version management across DuckDB releases

## Requirements for Community Extensions

### 1. Repository Requirements

- ✅ **Public repository** - Must be hosted on GitHub
- ✅ **Open source** - Must have an open-source license (we use MIT)
- ✅ **CMake build system** - Must use CMake for building
- ✅ **Extension template structure** - Should follow the DuckDB extension template structure

### 2. Build Requirements

The extension must be buildable using the standard DuckDB extension template CI pipeline:

- ✅ **CMake-based build** - Our `CMakeLists.txt` follows the template pattern
- ✅ **DuckDB submodule** - We have DuckDB as a submodule at `duckdb/`
- ✅ **Extension config** - We have `extension_config.cmake`
- ⚠️  **CI/CD workflows** - Need to add GitHub Actions workflows (see below)
- ⚠️  **Tests** - Need to ensure tests run in CI environment

### 3. External Dependencies

Our extension has external dependencies that need special handling:

#### NATS C Client Library (`cnats`)
- **Type**: System library
- **Installation**: `brew install cnats` (macOS), `apt-get install libnats-dev` (Linux)
- **CMake**: Uses `pkg-config` to find the library
- **Requirement**: Must specify `requires_toolchains` in descriptor

#### Protocol Buffers (`protobuf`)
- **Type**: System library  
- **Installation**: `brew install protobuf` (macOS), `apt-get install libprotobuf-dev` (Linux)
- **CMake**: Uses `find_package(Protobuf REQUIRED)`
- **Requirement**: Must specify `requires_toolchains` in descriptor

### 4. Submission Requirements

To submit to the Community Extensions repository, we need to create a `description.yml` file:

```yaml
extension:
  name: nats_js
  description: Query NATS JetStream message streams directly with SQL
  version: 1.0.0
  language: C++
  build: cmake
  license: MIT
  maintainers:
    - brannn
  requires_toolchains: nats-c, protobuf

repo:
  github: brannn/duckdb-nats-jetstream
  ref: <commit-hash-of-stable-release>

docs:
  hello_world: |
    -- Query messages from a NATS JetStream stream
    SELECT subject, seq, ts_nats, payload
    FROM nats_scan('telemetry')
    WHERE seq BETWEEN 1 AND 100;
    
    -- Extract JSON fields
    SELECT device_id, zone, kw
    FROM nats_scan('telemetry',
        json_extract := ['device_id', 'zone', 'kw']
    );
    
    -- Extract Protocol Buffers fields
    SELECT device_id, location_zone, metrics_kw
    FROM nats_scan('telemetry',
        proto_file := 'telemetry.proto',
        proto_message := 'Telemetry',
        proto_extract := ['device_id', 'location.zone', 'metrics.kw']
    );
  
  extended_description: |
    The NATS JetStream extension enables direct SQL querying of NATS JetStream 
    message streams without establishing durable consumers. It provides:
    
    - **Bounded historical queries** using Direct Get API
    - **Sequence-based range queries** (start_seq, end_seq)
    - **Timestamp-based range queries** with binary search (start_time, end_time)
    - **Subject filtering** for targeted message retrieval
    - **JSON payload extraction** with field mapping
    - **Protocol Buffers support** with runtime schema parsing and nested message navigation
    
    Perfect for ETL workflows, analytics, and ad-hoc querying of message streams.

    GitHub: https://github.com/brannn/duckdb-nats-jetstream
    Documentation: https://github.com/brannn/duckdb-nats-jetstream/blob/main/README.md
```

## Pre-Submission Checklist

Before submitting to the Community Extensions repository, we need to complete:

### Code & Build

- [x] Extension builds successfully with CMake
- [x] Extension follows DuckDB extension template structure
- [x] All source code is in `src/` directory
- [x] Headers are in `include/` directory
- [x] Tests are in `test/sql/` directory
- [ ] **TODO**: Add GitHub Actions CI workflows
- [ ] **TODO**: Ensure tests pass in CI environment
- [ ] **TODO**: Handle external dependencies in CI (nats-c, protobuf)

### Documentation

- [x] README.md with comprehensive documentation
- [x] LICENSE file (MIT)
- [x] Code examples in README
- [x] API reference documentation
- [ ] **TODO**: Ensure README renders well on GitHub

### Testing

- [x] SQL tests in `test/sql/`
- [x] Tests cover core functionality
- [x] Tests cover error cases
- [ ] **TODO**: Tests run successfully in CI
- [ ] **TODO**: Tests work without local NATS server (or provide setup instructions)

### Repository Hygiene

- [x] No hardcoded user-specific paths
- [x] Proper `.gitignore` file
- [x] No build artifacts committed
- [x] Clean git history
- [ ] **TODO**: Tag a stable release version

## GitHub Actions CI Requirements

The Community Extensions repository expects extensions to have CI workflows that:

1. **Build the extension** on multiple platforms
2. **Run tests** to verify functionality
3. **Produce loadable extension binaries**

### Required Workflows

Based on the extension-template, we need:

#### 1. Main Distribution Pipeline (`.github/workflows/MainDistributionPipeline.yml`)

This workflow:
- Builds extension for all platforms (Linux, macOS, Windows, WebAssembly)
- Runs tests on each platform
- Uploads extension binaries as artifacts
- Triggered on: push to main, pull requests, releases

#### 2. Test Workflow (`.github/workflows/test.yml`)

This workflow:
- Runs SQL tests
- Validates extension loads correctly
- Checks for regressions
- Triggered on: push, pull requests

### Platform-Specific Considerations

#### Linux
- Install `libnats-dev` and `libprotobuf-dev` via apt
- Use Ubuntu 20.04 or later

#### macOS
- Install `cnats` and `protobuf` via Homebrew
- Support both x86_64 and arm64 architectures

#### Windows
- May need to build nats-c from source or use vcpkg
- Protobuf available via vcpkg

#### WebAssembly
- May not be feasible due to external dependencies
- Can use `excluded_platforms` to skip WASM

## External Dependencies Strategy

Since our extension requires `cnats` and `protobuf`, we have two options:

### Option 1: System Libraries (Current Approach)
- **Pros**: Simple, uses existing packages
- **Cons**: Requires `requires_toolchains` flag, users must install dependencies
- **Status**: This is what we currently use

### Option 2: Vendored Dependencies
- **Pros**: Self-contained, no external dependencies
- **Cons**: Complex build, larger binary size, maintenance burden
- **Status**: Not implemented

**Recommendation**: Stick with Option 1 (system libraries) and use `requires_toolchains` in the descriptor.

## Submission Process

Once all requirements are met:

1. **Create a stable release**
   ```bash
   git tag -a v1.0.0 -m "Release v1.0.0"
   git push origin v1.0.0
   ```

2. **Fork the community-extensions repository**
   ```bash
   git clone https://github.com/duckdb/community-extensions.git
   cd community-extensions
   ```

3. **Create extension descriptor**
   ```bash
   mkdir -p extensions/nats_js
   # Create extensions/nats_js/description.yml (see template above)
   ```

4. **Submit pull request**
   - Create PR to `duckdb/community-extensions`
   - CI will automatically build and test the extension
   - DuckDB maintainers will review and approve

5. **After approval**
   - Extension will be available via `INSTALL nats_js FROM community`
   - Documentation will be auto-generated at `https://duckdb.org/community_extensions/extensions/nats_js`

## Next Steps

To prepare for submission, we need to:

1. **Add GitHub Actions CI workflows** (highest priority)
2. **Ensure tests run in CI environment** (may need Docker for NATS)
3. **Handle external dependencies in CI** (install cnats and protobuf)
4. **Test on multiple platforms** (Linux, macOS, Windows)
5. **Create a stable release tag** (v1.0.0)
6. **Submit to community-extensions repository**

## References

- [DuckDB Community Extensions Announcement](https://duckdb.org/2024/07/05/community-extensions.html)
- [Community Extensions Repository](https://github.com/duckdb/community-extensions)
- [Community Extensions Documentation](https://duckdb.org/community_extensions/documentation.html)
- [Extension Template](https://github.com/duckdb/extension-template)
- [List of Community Extensions](https://duckdb.org/community_extensions/list_of_extensions.html)

