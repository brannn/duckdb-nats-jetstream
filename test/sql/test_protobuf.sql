-- Test suite for Protocol Buffers support in nats_scan
-- Prerequisites:
--   1. NATS server running (docker-compose up -d)
--   2. Protobuf test data published (python3 test/proto/generate_protobuf_data.py)
--
-- Run with: duckdb -unsigned :memory: < test/sql/test_protobuf.sql

LOAD 'build/release/nats_js.duckdb_extension';

.print ========================================
.print Test 1: Basic protobuf field extraction
.print ========================================

SELECT 
    seq,
    device_id,
    timestamp,
    online,
    firmware_version
FROM nats_scan('telemetry_proto', 
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id', 'timestamp', 'online', 'firmware_version']
)
LIMIT 5;

.print
.print ========================================
.print Test 2: Nested message field extraction
.print ========================================

SELECT 
    seq,
    device_id,
    location_zone,
    location_rack,
    location_building
FROM nats_scan('telemetry_proto', 
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id', 'location.zone', 'location.rack', 'location.building']
)
LIMIT 5;

.print
.print ========================================
.print Test 3: Numeric field types (double)
.print ========================================

SELECT 
    seq,
    device_id,
    metrics_kw,
    metrics_pf,
    metrics_voltage,
    metrics_current,
    metrics_frequency
FROM nats_scan('telemetry_proto', 
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := [
        'device_id',
        'metrics.kw',
        'metrics.pf',
        'metrics.voltage',
        'metrics.current',
        'metrics.frequency'
    ]
)
LIMIT 5;

.print
.print ========================================
.print Test 4: Mixed field types
.print ========================================

SELECT 
    device_id,
    timestamp,
    location_zone,
    metrics_kw,
    online
FROM nats_scan('telemetry_proto', 
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := [
        'device_id',
        'timestamp',
        'location.zone',
        'metrics.kw',
        'online'
    ]
)
LIMIT 5;

.print
.print ========================================
.print Test 5: Payload column is BLOB type
.print ========================================

SELECT
    seq,
    typeof(payload) as payload_type,
    octet_length(payload) as payload_size
FROM nats_scan('telemetry_proto',
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id']
)
LIMIT 5;

.print
.print ========================================
.print Test 6: Aggregation on numeric fields
.print ========================================

SELECT 
    device_id,
    COUNT(*) as reading_count,
    ROUND(AVG(metrics_kw), 2) as avg_kw,
    ROUND(MIN(metrics_kw), 2) as min_kw,
    ROUND(MAX(metrics_kw), 2) as max_kw,
    ROUND(AVG(metrics_voltage), 2) as avg_voltage
FROM nats_scan('telemetry_proto', 
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id', 'metrics.kw', 'metrics.voltage']
)
GROUP BY device_id
ORDER BY device_id;

.print
.print ========================================
.print Test 7: Filtering on string fields
.print ========================================

SELECT 
    device_id,
    location_zone,
    location_rack,
    metrics_kw
FROM nats_scan('telemetry_proto', 
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id', 'location.zone', 'location.rack', 'metrics.kw']
)
WHERE location_zone = 'dc1'
LIMIT 10;

.print
.print ========================================
.print Test 8: Filtering on numeric fields
.print ========================================

SELECT 
    device_id,
    metrics_kw,
    metrics_voltage
FROM nats_scan('telemetry_proto', 
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id', 'metrics.kw', 'metrics.voltage']
)
WHERE metrics_kw > 5.3
LIMIT 10;

.print
.print ========================================
.print Test 9: Filtering on boolean fields
.print ========================================

SELECT 
    device_id,
    online,
    firmware_version
FROM nats_scan('telemetry_proto', 
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id', 'online', 'firmware_version']
)
WHERE online = true
LIMIT 10;

.print
.print ========================================
.print Test 10: Combined filtering
.print ========================================

SELECT 
    device_id,
    location_zone,
    metrics_kw,
    metrics_voltage,
    online
FROM nats_scan('telemetry_proto', 
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id', 'location.zone', 'metrics.kw', 'metrics.voltage', 'online']
)
WHERE location_zone = 'dc1' 
  AND metrics_kw > 5.2
  AND online = true
LIMIT 10;

.print
.print ========================================
.print Test 11: Sequence range filtering
.print ========================================

SELECT 
    seq,
    device_id,
    metrics_kw
FROM nats_scan('telemetry_proto', 
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id', 'metrics.kw'],
    start_seq := 10,
    end_seq := 20
);

.print
.print ========================================
.print Test 12: Subject filtering (exact match)
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
    subject := 'telemetry.dc1.power.pm5560.pm5560-001'
)
LIMIT 10;

.print
.print ========================================
.print Test 13: Count total messages
.print ========================================

SELECT COUNT(*) as total_messages
FROM nats_scan('telemetry_proto', 
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id']
);

.print
.print ========================================
.print Test 14: Group by nested field
.print ========================================

SELECT 
    location_zone,
    COUNT(*) as message_count,
    ROUND(AVG(metrics_kw), 2) as avg_kw
FROM nats_scan('telemetry_proto', 
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['location.zone', 'metrics.kw']
)
GROUP BY location_zone
ORDER BY location_zone;

.print
.print ========================================
.print All tests completed successfully!
.print ========================================

