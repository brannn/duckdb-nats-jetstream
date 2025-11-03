# SQL Test Suite

This directory contains SQL test scripts for the NATS JetStream DuckDB extension.

## Test Files

### `test_protobuf.sql`
Comprehensive test suite for Protocol Buffers support covering:
- Basic field extraction (string, int64, boolean)
- Nested message field extraction
- Numeric field types (double)
- Mixed field types
- Payload column type verification (BLOB for protobuf)
- Aggregation queries (AVG, MIN, MAX, COUNT)
- Filtering on string, numeric, and boolean fields
- Combined filtering
- Sequence range filtering
- Subject filtering
- Group by operations

### `test_protobuf_errors.sql`
Error handling test suite covering:
- Missing required parameters
- Invalid proto file paths
- Invalid message types
- Invalid field names
- Invalid nested field paths
- Mixing json_extract and proto_extract

## Prerequisites

1. **NATS server running:**
   ```bash
   docker-compose up -d
   ```

2. **Create telemetry stream:**
   ```bash
   nats stream add telemetry --subjects "telemetry.>" --storage file --retention limits --discard old --max-msgs=-1 --max-age=-1 --max-bytes=-1 --defaults
   ```

3. **Publish protobuf test data:**
   ```bash
   python3 test/proto/generate_protobuf_data.py
   ```

## Running Tests

### Run all protobuf tests:
```bash
duckdb -unsigned :memory: < test/sql/test_protobuf.sql
```

### Run error handling tests:
```bash
duckdb -unsigned :memory: < test/sql/test_protobuf_errors.sql
```

### Run specific test:
```bash
duckdb -unsigned :memory: << 'EOF'
LOAD 'build/release/nats_js.duckdb_extension';

SELECT device_id, metrics_kw
FROM nats_scan('telemetry', 
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id', 'metrics.kw']
)
LIMIT 5;
EOF
```

## Expected Results

All tests should pass with:
- ✅ Correct data types for each field
- ✅ Accurate values extracted from protobuf messages
- ✅ Proper handling of nested messages
- ✅ Clear error messages for invalid inputs
- ✅ Correct filtering and aggregation behavior

## Test Data

The test data consists of 500 protobuf messages (5 devices × 100 time points) with:
- Device IDs: pm5560-001 through pm5560-005
- Locations: dc1, dc2, dc3 with racks A1, A2, B1, B2, C1
- Metrics: kw, pf, kva, voltage, current, frequency
- Timestamps: Sequential timestamps 10 seconds apart
- Online status: Alternating true/false
- Firmware versions: v2.1.0, v2.1.1, v2.2.0

## Troubleshooting

### "No response from stream" error
The telemetry stream doesn't exist. Create it with:
```bash
nats stream add telemetry --subjects "telemetry.>" --storage file --retention limits --discard old --max-msgs=-1 --max-age=-1 --max-bytes=-1 --defaults
```

### "0 rows" returned
No test data has been published. Run:
```bash
python3 test/proto/generate_protobuf_data.py
```

### "Failed to import protobuf schema file"
The .proto file path is incorrect or the file doesn't exist. Verify:
```bash
ls -la test/proto/telemetry.proto
```

### "Extension not found"
The extension hasn't been built. Build it with:
```bash
make build
```

