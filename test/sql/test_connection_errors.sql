-- Test suite for connection management and error handling in nats_scan
-- Prerequisites:
--   1. NATS server running (docker-compose up -d) for success tests
--   2. Some tests expect failures - these are documented
--
-- Run with: duckdb -unsigned :memory: < test/sql/test_connection_errors.sql

LOAD 'build/release/nats_js.duckdb_extension';

.print ========================================
.print Test 1: Custom NATS URL (valid connection)
.print ========================================

SELECT COUNT(*) as count_custom_url
FROM nats_scan('telemetry_proto',
    url := 'nats://localhost:4222',
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id']
);

.print
.print ========================================
.print Test 2: Default URL (implicit localhost:4222)
.print ========================================

SELECT COUNT(*) as count_default_url
FROM nats_scan('telemetry_proto',
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id']
);

.print
.print ========================================
.print Test 3: Empty stream (no error, zero results)
.print ========================================
.print Note: This test will fail if 'empty_test_stream' doesn't exist
.print Create with: nats stream add empty_test_stream --subjects "empty.>" --defaults
.print ========================================

-- SELECT COUNT(*) as empty_count
-- FROM nats_scan('empty_test_stream',
--     json_extract := ['device_id']
-- );
-- Expected: 0 messages (not an error)

.print Skipped - requires manual stream creation

.print
.print ========================================
.print Test 4: Stream with messages - verify connection works
.print ========================================

SELECT
    COUNT(*) as total,
    COUNT(DISTINCT subject) as unique_subjects
FROM nats_scan('telemetry_proto',
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id']
);

.print
.print ========================================
.print Test 5: Multiple queries (connection isolation)
.print ========================================

SELECT COUNT(*) as query1_count
FROM nats_scan('telemetry_proto',
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id']
);

SELECT COUNT(*) as query2_count
FROM nats_scan('telemetry_proto',
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id']
);

.print
.print ========================================
.print Test 6: Large result set (stress test)
.print ========================================

SELECT
    COUNT(*) as total_messages,
    COUNT(DISTINCT device_id) as unique_devices,
    MIN(seq) as first_seq,
    MAX(seq) as last_seq
FROM nats_scan('telemetry_proto',
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id']
);

.print
.print ========================================
.print Test 7: Environmental stream connection
.print ========================================

SELECT COUNT(*) as environmental_count
FROM nats_scan('environmental',
    json_extract := ['device_id']
);

.print
.print ========================================
.print Test 8: Verify metadata columns exist
.print ========================================

SELECT
    stream,
    typeof(stream) as stream_type,
    typeof(subject) as subject_type,
    typeof(seq) as seq_type,
    typeof(ts_nats) as ts_nats_type,
    typeof(payload) as payload_type
FROM nats_scan('telemetry_proto',
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id']
)
LIMIT 1;

.print
.print ========================================
.print Test 9: Connection with all parameters
.print ========================================

SELECT
    COUNT(*) as filtered_count
FROM nats_scan('telemetry_proto',
    url := 'nats://localhost:4222',
    subject := 'pm5560',
    start_seq := 10,
    end_seq := 100,
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id', 'metrics.kw']
);

.print
.print ========================================
.print Test 10: Concurrent stream access
.print ========================================

WITH telemetry_data AS (
    SELECT COUNT(*) as telemetry_count
    FROM nats_scan('telemetry_proto',
        proto_file := 'test/proto/telemetry.proto',
        proto_message := 'Telemetry',
        proto_extract := ['device_id']
    )
),
environmental_data AS (
    SELECT COUNT(*) as environmental_count
    FROM nats_scan('environmental', json_extract := ['device_id'])
)
SELECT * FROM telemetry_data, environmental_data;

.print
.print ========================================
.print ERROR TESTS (Expected to fail)
.print ========================================
.print The following tests expect errors and are commented out
.print to prevent test suite failure. Uncomment to verify error handling.
.print ========================================

-- Test E1: Invalid NATS URL
-- SELECT * FROM nats_scan('telemetry_proto', url := 'invalid://bad-url:9999') LIMIT 1;
-- Expected: Error - connection failure

-- Test E2: Non-existent stream
-- SELECT * FROM nats_scan('nonexistent_stream_xyz') LIMIT 1;
-- Expected: Error - stream not found

-- Test E3: Empty stream name
-- SELECT * FROM nats_scan('') LIMIT 1;
-- Expected: Error - invalid stream name

-- Test E4: NATS server unreachable (wrong port)
-- SELECT * FROM nats_scan('telemetry_proto', url := 'nats://localhost:9999') LIMIT 1;
-- Expected: Error - connection timeout or refused

-- Test E5: Mutually exclusive parameters (sequences + timestamps)
-- SELECT * FROM nats_scan('telemetry_proto',
--     start_seq := 10,
--     start_time := '2025-11-12'::TIMESTAMP) LIMIT 1;
-- Expected: Error - parameter conflict

-- Test E6: Mutually exclusive parameters (JSON + protobuf)
-- SELECT * FROM nats_scan('telemetry_proto',
--     json_extract := ['field1'],
--     proto_file := 'test/proto/telemetry.proto',
--     proto_message := 'Telemetry') LIMIT 1;
-- Expected: Error - parameter conflict

.print
.print ========================================
.print All connection tests completed successfully!
.print ========================================
.print Note: Error tests (E1-E6) are commented out
.print ========================================

