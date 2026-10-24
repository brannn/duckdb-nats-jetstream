# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## [0.4.0] - 2026-10-24

### Upgrading from 0.3
- A `headers` column, `MAP(VARCHAR, VARCHAR[])`, comes before `payload`, so `SELECT *` returns one more column.
- Sequence and time options can be combined; they no longer raise an error.

### Added
- Batched reads: on nats-server 2.11 or later, a stream with `allow_direct` returns up to 2,048 messages per request, and a start time takes one request. On loopback, a scan reads about 700,000 small messages a second, against 30,000 before.
- The `headers` column
- `WHERE` clauses on `seq`, `ts_nats` and `subject` narrow the read like the options do, and `EXPLAIN` shows what the scan reads
- Payloads are decoded only for the field columns a query uses
- Secrets of type `nats` for user and password, token, credentials file or NKey, and TLS files, found by scope or named with the `secret` option
- DuckDB's progress bar for long scans
- CI runs the integration tests against nats-server 2.10 and 2.15

### Fixed
- `proto_message` accepts package-qualified names, nested types such as `Outer.Inner`, and types from imported files
- Direct gets read a message with its own `Status` header as the end of the stream or an error, and a message republished from another stream with that stream's sequence, subject and time
- `enable_external_access = false` also stops statements prepared before it was set

## [0.3.0] - 2026-10-21

### Upgrading from 0.2
- `subject` is a NATS subject filter, no longer a substring match: `'telemetry.dc1.power'` matches only that subject. Use wildcards, as in `'telemetry.dc1.power.>'`, or `WHERE subject LIKE ...`.
- `json_extract` dot paths read nested objects, and their columns use underscores (`location.zone` becomes `location_zone`), as protobuf columns do.
- JSON numbers come back as written in the message (`42`, not `42.000000`).
- Protobuf fields that track presence (messages, proto3 `optional`, `oneof` members) are NULL when unset. A message field returns a JSON object.
- A field column named like another column gets a numeric suffix (`subject_1`).
- Errors are DuckDB's IO, Binder and Permission errors.

### Changed
- Built for DuckDB v2.0.0, with cnats 3.13 and protobuf 6.33
- The server applies subject filters, so messages on other subjects are not read; `*` and `>` wildcards work
- Message reads ask for the next stored message, so deleted sequences cost nothing

### Added
- Streams without `allow_direct` are read through the JetStream API
- Array indexes in `json_extract` paths (`'phases.0.amps'`)
- Map fields as JSON objects; repeated bytes, enums and messages in JSON arrays
- `.proto` files can import protobuf's well-known types, such as `google/protobuf/timestamp.proto`
- `nats_scan` refuses to run when `enable_external_access` is false
- Tests against a real nats-server, `make test_integration`, from a committed JetStream store

### Fixed
- Time ranges returned the wrong messages on streams with deleted messages
- `end_time` included the first message after it
- Nested `json_extract` paths always returned NULL
- JSON numbers were rounded to six decimals and lost precision above 2^53
- Repeated string fields produced invalid JSON, and repeated floats were rounded to six decimals
- A `proto_extract` path through a repeated message field crashed the process
- A NULL option or list element raised an internal error
- A JSON field named like a base column failed with a duplicate column error

### Removed
- The Docker-based SQL test scripts, replaced by the sqllogictest suites under `test/sql`
- WebAssembly builds: a browser cannot open the TCP connection to a NATS server

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

