# DuckDB NATS JetStream Extension

[![CI](https://github.com/brannn/duckdb-nats-jetstream/actions/workflows/MainDistributionPipeline.yml/badge.svg)](https://github.com/brannn/duckdb-nats-jetstream/actions/workflows/MainDistributionPipeline.yml)
[![DuckDB Version](https://img.shields.io/badge/DuckDB-v1.4.1-blue)](https://github.com/duckdb/duckdb/releases/tag/v1.4.1)
[![License](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Linux%20%7C%20macOS%20%7C%20Windows%20%7C%20WebAssembly-lightgrey)](https://github.com/brannn/duckdb-nats-jetstream/actions)

A DuckDB extension that enables SQL queries over NATS JetStream message streams. This extension allows DuckDB to read messages directly from JetStream streams as table data, supporting sequence and timestamp-based range queries with JSON and Protocol Buffers payload extraction.

## Installation

### From DuckDB Community Extensions (Recommended)

Once published to the DuckDB Community Extensions repository, installation is simple:

```sql
INSTALL nats_js FROM community;
LOAD nats_js;
```

This method automatically downloads the pre-built extension binary for your platform. No compilation required.

**Note**: This extension requires the NATS C client library and Protocol Buffers library to be installed on your system:

```bash
# macOS
brew install cnats protobuf

# Ubuntu/Debian
sudo apt-get install libnats-dev libprotobuf-dev

# Fedora/RHEL
sudo dnf install cnats-devel protobuf-devel
```

### Building from Source

For development or if you need to build from source:

**Prerequisites:**
- DuckDB v1.4.1
- NATS C client library (v3.11.0 or later)
- Protocol Buffers library (v3.0 or later)
- CMake 3.15 or later
- C++17 compatible compiler

**Build steps:**

Clone the repository and initialize submodules:

```bash
git clone https://github.com/brannn/duckdb-nats-jetstream
cd duckdb-nats-jetstream
git submodule update --init --recursive
```

Install the required libraries. On macOS with Homebrew:

```bash
brew install cnats protobuf
```

On Ubuntu/Debian:

```bash
sudo apt-get install libnats-dev libprotobuf-dev protobuf-compiler
```

Build the extension:

```bash
make build
```

The compiled extension will be located at `build/release/nats_js.duckdb_extension`.

**Loading the extension:**

```sql
LOAD 'build/release/nats_js.duckdb_extension';
```

Note: When loading unsigned extensions, you may need to start DuckDB with the `-unsigned` flag:

```bash
duckdb -unsigned
```

### Runtime Requirements

A running NATS server with JetStream enabled is required for operation. The extension connects to `nats://localhost:4222` by default, which can be overridden using the `url` parameter.

## Query Capabilities

The extension provides the `nats_scan` table function, which returns messages from a JetStream stream. Each row represents a single message with columns for stream name, subject, sequence number, timestamp, and payload.

### Basic Message Retrieval

Query all messages from a stream:

```sql
SELECT stream, subject, seq, ts_nats, payload
FROM nats_scan('telemetry');
```

```
┌───────────┬──────────────────────────────────┬────────┬─────────────────────────┬──────────────────────────────────────┐
│  stream   │             subject              │  seq   │        ts_nats          │               payload                │
│  varchar  │             varchar              │ uint64 │       timestamp         │               varchar                │
├───────────┼──────────────────────────────────┼────────┼─────────────────────────┼──────────────────────────────────────┤
│ telemetry │ telemetry.dc1.power.pm5560-001   │      1 │ 2025-11-01 09:00:00     │ {"device_id":"pm5560-001","kw":42.5} │
│ telemetry │ telemetry.dc1.power.pm5560-002   │      2 │ 2025-11-01 09:00:01     │ {"device_id":"pm5560-002","kw":38.2} │
│ telemetry │ telemetry.dc1.power.pm5560-003   │      3 │ 2025-11-01 09:00:02     │ {"device_id":"pm5560-003","kw":51.7} │
└───────────┴──────────────────────────────────┴────────┴─────────────────────────┴──────────────────────────────────────┘
```

The function returns five base columns: `stream` (VARCHAR), `subject` (VARCHAR), `seq` (UBIGINT), `ts_nats` (TIMESTAMP), and `payload` (VARCHAR for JSON messages, BLOB for protobuf messages).

### Sequence Range Queries

Retrieve messages within a specific sequence number range:

```sql
SELECT seq, ts_nats, payload
FROM nats_scan('telemetry', start_seq := 1000, end_seq := 2000);
```

Sequence numbers are inclusive on both ends. The `start_seq` parameter defaults to 1, and `end_seq` defaults to the last message in the stream.

### Timestamp Range Queries

Query messages within a time range:

```sql
SELECT seq, ts_nats, subject, payload
FROM nats_scan('telemetry',
    start_time := '2025-11-01 09:00:00'::TIMESTAMP,
    end_time := '2025-11-01 17:00:00'::TIMESTAMP
);
```

The extension uses binary search to resolve timestamps to sequence numbers, providing O(log n) lookup performance. Timestamp parameters cannot be mixed with sequence parameters in the same query.

### Subject Filtering

Filter messages by subject pattern:

```sql
SELECT seq, subject, payload
FROM nats_scan('telemetry', subject := 'telemetry.dc1.power.pm5560-001');
```

The subject filter performs substring matching on message subjects. This filtering occurs after message retrieval, so it works efficiently when combined with sequence or timestamp ranges.

### Combined Queries

Combine multiple query parameters:

```sql
SELECT seq, ts_nats, subject, payload
FROM nats_scan('telemetry',
    subject := 'pm5560',
    start_time := '2025-11-01 09:00:00'::TIMESTAMP,
    end_time := '2025-11-01 17:00:00'::TIMESTAMP
);
```

## JSON Processing

The extension can extract fields from JSON payloads and expose them as additional columns. This feature is useful for IoT telemetry, application logs, and other structured message data.

### Extracting JSON Fields

Use the `json_extract` parameter to specify fields to extract:

```sql
SELECT device_id, kw, voltage, pf
FROM nats_scan('telemetry',
    start_seq := 1,
    end_seq := 100,
    json_extract := ['device_id', 'kw', 'voltage', 'pf']
);
```

```
┌──────────────┬─────────┬─────────┬─────────┐
│  device_id   │   kw    │ voltage │   pf    │
│   varchar    │ varchar │ varchar │ varchar │
├──────────────┼─────────┼─────────┼─────────┤
│ pm5560-001   │ 42.5    │ 480.2   │ 0.95    │
│ pm5560-002   │ 38.2    │ 479.8   │ 0.92    │
│ pm5560-003   │ 51.7    │ 481.1   │ 0.97    │
│ pm5560-004   │ 45.3    │ 480.5   │ 0.94    │
└──────────────┴─────────┴─────────┴─────────┘
```

This example extracts power monitoring data from a hypothetical IoT sensor stream. Each extracted field becomes a VARCHAR column in the result set.

### Type Handling

The JSON extraction handles multiple data types. String values are returned directly, numeric values are converted to strings, boolean values become "true" or "false", and null values produce SQL NULL. Complex types like objects and arrays are serialized as JSON strings.

### Analytics with Extracted Fields

Combine JSON extraction with DuckDB's analytical capabilities:

```sql
SELECT
    device_id,
    AVG(CAST(kw AS DOUBLE)) as avg_power,
    MAX(CAST(kw AS DOUBLE)) as peak_power,
    COUNT(*) as reading_count
FROM nats_scan('telemetry',
    start_time := '2025-11-01 00:00:00'::TIMESTAMP,
    end_time := '2025-11-01 23:59:59'::TIMESTAMP,
    json_extract := ['device_id', 'kw']
)
GROUP BY device_id
ORDER BY avg_power DESC;
```

```
┌──────────────┬───────────┬────────────┬───────────────┐
│  device_id   │ avg_power │ peak_power │ reading_count │
│   varchar    │  double   │   double   │     int64     │
├──────────────┼───────────┼────────────┼───────────────┤
│ pm5560-003   │     51.24 │      58.90 │          1440 │
│ pm5560-004   │     45.67 │      52.30 │          1440 │
│ pm5560-001   │     42.18 │      49.10 │          1440 │
│ pm5560-002   │     38.92 │      44.50 │          1440 │
└──────────────┴───────────┴────────────┴───────────────┘
```

This query analyzes power consumption across multiple devices, demonstrating how the extension integrates with DuckDB's aggregation and sorting functions.

## Protocol Buffers Processing

The extension supports extracting fields from Protocol Buffers (protobuf) encoded messages. This feature is essential for systems using protobuf for efficient binary serialization, common in IoT telemetry, microservices, and high-throughput data pipelines.

### Extracting Protobuf Fields

Use the `proto_extract` parameter along with `proto_file` and `proto_message` to specify fields to extract:

```sql
SELECT device_id, timestamp, location_zone, metrics_kw, online
FROM nats_scan('telemetry',
    proto_file := 'schemas/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id', 'timestamp', 'location.zone', 'metrics.kw', 'online']
);
```

```
┌──────────────┬─────────────────────┬───────────────┬────────────┬─────────┐
│  device_id   │      timestamp      │ location_zone │ metrics_kw │ online  │
│   varchar    │       int64         │    varchar    │   double   │ boolean │
├──────────────┼─────────────────────┼───────────────┼────────────┼─────────┤
│ pm5560-001   │ 1730455200000000000 │ zone-a        │      42.50 │ true    │
│ pm5560-002   │ 1730455201000000000 │ zone-a        │      38.20 │ true    │
│ pm5560-003   │ 1730455202000000000 │ zone-b        │      51.70 │ true    │
│ pm5560-004   │ 1730455203000000000 │ zone-b        │      45.30 │ false   │
└──────────────┴─────────────────────┴───────────────┴────────────┴─────────┘
```

This example extracts fields from a protobuf-encoded telemetry stream. The `proto_file` parameter specifies the path to the .proto schema file, `proto_message` specifies the message type name, and `proto_extract` lists the fields to extract.

### Nested Message Fields

Protobuf nested messages are accessed using dot notation in field paths. The extension automatically navigates through nested message structures:

```sql
SELECT device_id, location_zone, location_rack, metrics_kw, metrics_voltage
FROM nats_scan('telemetry',
    proto_file := 'schemas/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := [
        'device_id',
        'location.zone',
        'location.rack',
        'metrics.kw',
        'metrics.voltage'
    ]
);
```

For a schema with nested Location and Metrics messages, the extension extracts `location.zone` from the Location message and `metrics.kw` from the Metrics message. Column names use underscores instead of dots (`location_zone`, `metrics_kw`) for natural SQL syntax.

### Type Mapping

The extension maps protobuf types to appropriate DuckDB types:

| Protobuf Type | DuckDB Type |
|---------------|-------------|
| `string` | VARCHAR |
| `bytes` | BLOB |
| `int32`, `sint32`, `sfixed32` | INTEGER |
| `int64`, `sint64`, `sfixed64` | BIGINT |
| `uint32`, `fixed32` | UINTEGER |
| `uint64`, `fixed64` | UBIGINT |
| `float` | FLOAT |
| `double` | DOUBLE |
| `bool` | BOOLEAN |
| `enum` | VARCHAR (enum name) |

This type mapping enables direct use of numeric fields in calculations without type casting:

```sql
SELECT
    device_id,
    AVG(metrics_kw) as avg_power,
    MAX(metrics_voltage) as peak_voltage,
    COUNT(*) as reading_count
FROM nats_scan('telemetry',
    proto_file := 'schemas/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id', 'metrics.kw', 'metrics.voltage']
)
GROUP BY device_id;
```

```
┌──────────────┬───────────┬──────────────┬───────────────┐
│  device_id   │ avg_power │ peak_voltage │ reading_count │
│   varchar    │  double   │    double    │     int64     │
├──────────────┼───────────┼──────────────┼───────────────┤
│ pm5560-003   │     51.24 │       482.30 │          1440 │
│ pm5560-004   │     45.67 │       481.80 │          1440 │
│ pm5560-001   │     42.18 │       480.90 │          1440 │
│ pm5560-002   │     38.92 │       479.50 │          1440 │
└──────────────┴───────────┴──────────────┴───────────────┘
```

### Analytics with Protobuf Data

Combine protobuf extraction with DuckDB's analytical capabilities for complex queries:

```sql
SELECT
    location_zone,
    COUNT(*) as message_count,
    ROUND(AVG(metrics_kw), 2) as avg_kw,
    ROUND(MIN(metrics_kw), 2) as min_kw,
    ROUND(MAX(metrics_kw), 2) as max_kw
FROM nats_scan('telemetry',
    proto_file := 'schemas/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['location.zone', 'metrics.kw'],
    start_time := '2025-11-01 00:00:00'::TIMESTAMP,
    end_time := '2025-11-01 23:59:59'::TIMESTAMP
)
GROUP BY location_zone
ORDER BY avg_kw DESC;
```

This query analyzes power consumption by data center zone, demonstrating how protobuf extraction integrates with time-based queries and aggregations.

### Filtering on Protobuf Fields

Filter results using extracted protobuf fields:

```sql
SELECT device_id, location_zone, metrics_kw, metrics_voltage, online
FROM nats_scan('telemetry',
    proto_file := 'schemas/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id', 'location.zone', 'metrics.kw', 'metrics.voltage', 'online']
)
WHERE location_zone = 'dc1'
  AND metrics_kw > 5.0
  AND online = true;
```

The extension extracts and decodes protobuf fields, then DuckDB applies the WHERE clause filters efficiently.

### Payload Column Behavior

When using protobuf extraction, the `payload` column contains the raw binary protobuf data as a BLOB type (instead of VARCHAR for JSON messages). This prevents encoding errors when displaying binary data:

```sql
SELECT seq, typeof(payload) as payload_type, octet_length(payload) as payload_size
FROM nats_scan('telemetry',
    proto_file := 'schemas/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id']
)
LIMIT 5;
```

## Implementation Details

Understanding the extension's implementation approach helps explain its performance characteristics and operational behavior.

### Direct Get API

The extension uses NATS JetStream's Direct Get API for message retrieval. This API allows fetching individual messages by sequence number without establishing a consumer. Direct Get provides low-latency access to historical messages and avoids the overhead of consumer management for ad-hoc queries.

The extension does not create durable or ephemeral consumers for typical query operations. Each message fetch is an independent operation that retrieves a single message by sequence number. This approach is optimal for bounded historical queries where the query range is known in advance.

### Binary Search for Timestamp Resolution

When queries specify timestamp ranges using `start_time` or `end_time` parameters, the extension must resolve these timestamps to sequence numbers. The implementation uses binary search over the stream's sequence range to find the first message at or after the target timestamp.

The binary search algorithm uses Direct Get to fetch messages at the midpoint of the current search range, compares their timestamps to the target, and narrows the range accordingly. This provides O(log n) performance for timestamp resolution, where n is the number of messages in the stream. For a stream with one million messages, timestamp resolution requires approximately 20 message fetches.

After resolving timestamps to sequences, the extension uses the same Direct Get approach to retrieve messages in the resolved sequence range. Subject filtering, when specified, is applied during message iteration rather than during timestamp resolution.

### Resource Management

The extension manages NATS connections and JetStream contexts using RAII patterns. Connections are established during the table function's initialization phase and cleaned up automatically when the query completes. Connection timeouts are set to 5 seconds to prevent indefinite blocking on unreachable servers.

The extension uses the NATS C client library (cnats) for all NATS protocol operations and yyjson for JSON parsing. Both libraries are production-tested and provide the necessary performance for analytical workloads.

### Execution Model

The extension executes in a single-threaded model. Each query establishes one connection to the NATS server and fetches messages sequentially. Messages are returned to DuckDB in batches of up to 2048 rows (STANDARD_VECTOR_SIZE), allowing DuckDB to process results incrementally.

## API Reference

The `nats_scan` table function accepts the following parameters:

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `stream_name` | VARCHAR | Yes | - | Name of the JetStream stream to query |
| `url` | VARCHAR | No | `nats://localhost:4222` | NATS server URL |
| `subject` | VARCHAR | No | - | Subject filter (substring match) |
| `start_seq` | UBIGINT | No | 1 | Starting sequence number (inclusive) |
| `end_seq` | UBIGINT | No | Last message | Ending sequence number (inclusive) |
| `start_time` | TIMESTAMP | No | - | Starting timestamp (inclusive) |
| `end_time` | TIMESTAMP | No | - | Ending timestamp (inclusive) |
| `json_extract` | LIST(VARCHAR) | No | - | List of JSON field names to extract |
| `proto_file` | VARCHAR | No | - | Path to .proto schema file |
| `proto_message` | VARCHAR | No | - | Protobuf message type name |
| `proto_extract` | LIST(VARCHAR) | No | - | List of protobuf field paths to extract (supports dot notation for nested fields) |

### Parameter Constraints

Sequence-based parameters (`start_seq`, `end_seq`) cannot be combined with timestamp-based parameters (`start_time`, `end_time`) in the same query. The extension will return an error if both parameter types are specified.

The `json_extract` and `proto_extract` parameters are mutually exclusive. Use `json_extract` for JSON-encoded messages or `proto_extract` for protobuf-encoded messages, but not both in the same query.

When using `proto_extract`, both `proto_file` and `proto_message` parameters are required. The `proto_file` parameter specifies the path to the .proto schema file, and `proto_message` specifies the message type name within that file.

Extracted fields (JSON or protobuf) are appended as additional columns after the five base columns (`stream`, `subject`, `seq`, `ts_nats`, `payload`). Column names for nested protobuf fields use underscores instead of dots (e.g., `location.zone` becomes `location_zone`).

## Roadmap

### Current Capabilities (MVP)

- Bounded historical queries using Direct Get API
- Sequence-based range queries (`start_seq`, `end_seq`)
- Timestamp-based range queries with binary search (`start_time`, `end_time`)
- Subject filtering (exact match)
- JSON payload extraction with field mapping
- Protocol Buffers support:
  - Runtime .proto schema parsing
  - All primitive types (string, bytes, integers, floats, bool, enum)
  - Nested message navigation with dot notation
  - Automatic type mapping to DuckDB types

### Planned Features

#### Stateful Consumption
- **Durable consumers** - Message acknowledgement for reliable ETL workflows
- **Checkpoint management** - Resumable processing with state persistence
- **Consumer groups** - Distributed processing across multiple workers

#### Advanced Protocol Buffers
- **Repeated fields** - Array support with proper DuckDB LIST type mapping
- **Map fields** - Key-value map support with DuckDB MAP type
- **Oneof fields** - Union type handling
- **Any types** - Dynamic type resolution
- **Import resolution** - Support for .proto files with imports
- **Well-known types** - Native support for Timestamp, Duration, Struct, etc.

#### Additional Data Formats
- **Apache Avro** - Schema registry integration and binary encoding support
- **MessagePack** - Compact binary JSON alternative
- **CBOR** - Concise Binary Object Representation

#### Live Streaming
- **Tail function** - Unbounded reads for real-time data processing
- **Backpressure management** - Flow control for high-throughput streams
- **Push-based delivery** - Event-driven message consumption

#### Performance Enhancements
- **Parallel scanning** - Multi-threaded message retrieval for multi-subject streams
- **Vectorized decoding** - SIMD optimizations for JSON/protobuf parsing
- **Schema caching** - Reuse parsed schemas across queries
- **Connection pooling** - Reduce connection overhead for repeated queries

#### Configuration & Usability
- **Connection profiles** - Named connection configurations
- **Credential management** - Support for NATS authentication (tokens, JWT, NKeys)
- **TLS support** - Encrypted connections to NATS servers
- **Stream discovery** - Automatic stream and subject enumeration

### Development Resources

Detailed planning documentation is available in the `planning/` directory, including:
- Complete development roadmap with milestones
- Implementation specifications
- Technical architecture decisions
- Performance benchmarks and optimization strategies

## Development

Build the extension:

```bash
make build
```

Run tests:

```bash
make test
```

Clean build artifacts:

```bash
make clean
```

Additional documentation for contributors is available in the `planning/` directory, including implementation plans, technical architecture, and development roadmap.

## License

This project is licensed under the MIT License. See the LICENSE file for details.
