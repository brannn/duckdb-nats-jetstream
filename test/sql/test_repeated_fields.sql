-- Test suite for repeated protobuf field support
-- Prerequisites:
--   1. NATS server running
--   2. Protobuf test data published (python3 test/proto/generate_protobuf_data.py)
--
-- Run with: duckdb -unsigned :memory: < test/sql/test_repeated_fields.sql

LOAD 'build/release/extension/nats_js/nats_js.duckdb_extension';

.print ========================================
.print Test 1: Repeated string field as JSON array
.print ========================================

SELECT seq, device_id, tags
FROM nats_scan('telemetry_proto',
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id', 'tags']
)
LIMIT 5;

.print
.print ========================================
.print Test 2: Repeated field type is VARCHAR
.print ========================================

SELECT typeof(tags) as tags_type
FROM nats_scan('telemetry_proto',
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['tags']
)
LIMIT 1;

.print
.print ========================================
.print Test 3: Filter on repeated field content
.print ========================================

SELECT device_id, tags
FROM nats_scan('telemetry_proto',
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id', 'tags']
)
WHERE tags LIKE '%active%'
LIMIT 5;

.print
.print ========================================
.print Test 4: Repeated field alongside singular fields
.print ========================================

SELECT device_id, location_zone, metrics_kw, online, tags
FROM nats_scan('telemetry_proto',
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id', 'location.zone', 'metrics.kw', 'online', 'tags']
)
LIMIT 5;

.print
.print ========================================
.print Test 5: Count by tag content
.print ========================================

SELECT
    CASE WHEN tags LIKE '%active%' THEN 'active' ELSE 'inactive' END as status,
    COUNT(*) as msg_count
FROM nats_scan('telemetry_proto',
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['tags']
)
GROUP BY status
ORDER BY status;

.print
.print ========================================
.print All repeated field tests completed!
.print ========================================
