-- Test suite for subject filtering in nats_scan
-- Prerequisites:
--   1. NATS server running (docker-compose up -d)
--   2. Test data published with various subject patterns
--
-- Run with: duckdb -unsigned :memory: < test/sql/test_subject_filtering.sql

LOAD 'build/release/nats_js.duckdb_extension';

.print ========================================
.print Test 1: Exact subject match (from test_protobuf.sql)
.print ========================================

SELECT
    seq,
    subject,
    device_id,
    location_zone
FROM nats_scan('telemetry_proto',
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id', 'location.zone'],
    subject := 'telemetry_proto.dc1.power.pm5560.pm5560-001'
)
LIMIT 10;

.print
.print ========================================
.print Test 2: Partial subject match (substring)
.print ========================================

SELECT DISTINCT
    subject,
    COUNT(*) as message_count
FROM nats_scan('telemetry_proto',
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id'],
    subject := 'zone-a'
)
GROUP BY subject
ORDER BY subject;

.print
.print ========================================
.print Test 3: Subject filter on device type
.print ========================================

SELECT
    subject,
    device_id,
    COUNT(*) as readings
FROM nats_scan('telemetry_proto',
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id'],
    subject := 'pm5560'
)
GROUP BY subject, device_id
ORDER BY subject, device_id
LIMIT 10;

.print
.print ========================================
.print Test 4: Subject filter with no matches
.print ========================================

SELECT COUNT(*) as no_match_count
FROM nats_scan('telemetry_proto',
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id'],
    subject := 'nonexistent.subject.pattern'
);

.print
.print ========================================
.print Test 5: Subject filter + sequence range
.print ========================================

SELECT
    seq,
    subject,
    device_id
FROM nats_scan('telemetry_proto',
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id'],
    subject := 'pm5560',
    start_seq := 1,
    end_seq := 100
)
ORDER BY seq
LIMIT 10;

.print
.print ========================================
.print Test 6: Subject filter + timestamp range
.print ========================================

SELECT
    COUNT(*) as filtered_count,
    MIN(ts_nats) as first,
    MAX(ts_nats) as last
FROM nats_scan('telemetry_proto',
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id'],
    subject := 'pm5560-001',
    start_time := (current_timestamp - INTERVAL '3 hours')::TIMESTAMP,
    end_time := (current_timestamp - INTERVAL '2 hours')::TIMESTAMP
);

.print
.print ========================================
.print Test 7: Group by subject analysis
.print ========================================

SELECT
    subject,
    COUNT(*) as message_count,
    MIN(seq) as first_seq,
    MAX(seq) as last_seq
FROM nats_scan('telemetry_proto',
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id']
)
GROUP BY subject
ORDER BY message_count DESC
LIMIT 10;

.print
.print ========================================
.print Test 8: Multiple subject patterns using SQL WHERE
.print ========================================

SELECT
    subject,
    device_id,
    location_zone
FROM nats_scan('telemetry_proto',
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id', 'location.zone']
)
WHERE subject LIKE '%pm5560-001%' OR subject LIKE '%pm5560-002%'
LIMIT 10;

.print
.print ========================================
.print Test 9: Subject filter with protobuf extraction
.print ========================================

SELECT
    subject,
    device_id,
    location_zone as zone,
    metrics_kw as kw
FROM nats_scan('telemetry_proto',
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id', 'location.zone', 'metrics.kw'],
    subject := 'dc1'
)
LIMIT 10;

.print
.print ========================================
.print Test 10: Environmental stream subject filtering
.print ========================================

SELECT
    subject,
    device_id,
    location,
    temp_c::DOUBLE as temp_c
FROM nats_scan('environmental',
    json_extract := ['device_id', 'location', 'temp_c'],
    subject := 'temp'
)
LIMIT 10;

.print
.print ========================================
.print Test 11: Subject-based aggregation
.print ========================================

SELECT
    subject,
    COUNT(*) as readings,
    ROUND(AVG(metrics_kw), 2) as avg_kw,
    ROUND(MIN(metrics_kw), 2) as min_kw,
    ROUND(MAX(metrics_kw), 2) as max_kw
FROM nats_scan('telemetry_proto',
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['metrics.kw'],
    subject := 'pm5560'
)
GROUP BY subject
ORDER BY subject;

.print
.print ========================================
.print Test 12: Subject filter efficiency test
.print ========================================

SELECT
    COUNT(*) as total_filtered,
    COUNT(DISTINCT subject) as unique_subjects,
    COUNT(DISTINCT device_id) as unique_devices
FROM nats_scan('telemetry_proto',
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id'],
    subject := 'power'
);

.print
.print ========================================
.print All subject filtering tests completed successfully!
.print ========================================

