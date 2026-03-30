# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## [0.2.1] - 2026-03-30

### Added
- Support for repeated protobuf fields, serialized as JSON arrays
- Query cancellation support (Ctrl-C now interrupts long scans)

### Changed
- Deduplicate field path parsing into shared helper
- Reuse protobuf Message allocation per chunk instead of per row
- Replace per-cell SetValue with direct vector writes for envelope and JSON columns
- Move NATS connection and stream validation to init time (fail-fast on invalid streams)
- Remove dead code and redundant comments (-108 net lines)

## [0.2.0] - 2026-03-29

### Changed
- Upgrade to DuckDB v1.5.1 (from v1.4.2)
- Update extension-ci-tools to v1.5.1
- Update extension build output path for DuckDB 1.5 late linking layout

## [0.1.1] - 2025-11-05

### Fixed
- Payload column now returns BLOB instead of VARCHAR when no extraction parameters specified, preventing UTF-8 validation errors on binary data

## [0.1.0] - 2025-11-03

### Added
- Initial release of DuckDB NATS JetStream extension
- `nats_scan` table function for querying JetStream streams
- Sequence-based message retrieval with range queries
- Timestamp-based message retrieval with binary search
- JSON payload extraction with dot notation for nested fields
- Protocol Buffers payload extraction with schema support
- Support for Linux, macOS, Windows, and WebAssembly platforms
- CI/CD pipeline with multi-platform builds

