-- Test suite for Protocol Buffers error handling in nats_scan
-- Prerequisites:
--   1. NATS server running (docker-compose up -d)
--
-- Run with: duckdb -unsigned :memory: < test/sql/test_protobuf_errors.sql

LOAD 'build/release/nats_js.duckdb_extension';

.print ========================================
.print Test 1: Missing proto_file parameter
.print Expected: Error message
.print ========================================

SELECT * FROM nats_scan('telemetry_proto', 
    proto_message := 'Telemetry',
    proto_extract := ['device_id']
) LIMIT 1;

.print
.print ========================================
.print Test 2: Missing proto_message parameter
.print Expected: Error message
.print ========================================

SELECT * FROM nats_scan('telemetry_proto', 
    proto_file := 'test/proto/telemetry.proto',
    proto_extract := ['device_id']
) LIMIT 1;

.print
.print ========================================
.print Test 3: Invalid proto file path
.print Expected: Error message
.print ========================================

SELECT * FROM nats_scan('telemetry_proto', 
    proto_file := 'test/proto/nonexistent.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id']
) LIMIT 1;

.print
.print ========================================
.print Test 4: Invalid message type
.print Expected: Error message
.print ========================================

SELECT * FROM nats_scan('telemetry_proto', 
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'NonExistentMessage',
    proto_extract := ['device_id']
) LIMIT 1;

.print
.print ========================================
.print Test 5: Invalid field name
.print Expected: Error message
.print ========================================

SELECT * FROM nats_scan('telemetry_proto', 
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['nonexistent_field']
) LIMIT 1;

.print
.print ========================================
.print Test 6: Invalid nested field path
.print Expected: Error message
.print ========================================

SELECT * FROM nats_scan('telemetry_proto', 
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id.something']
) LIMIT 1;

.print
.print ========================================
.print Test 7: Invalid nested field name
.print Expected: Error message
.print ========================================

SELECT * FROM nats_scan('telemetry_proto', 
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['location.nonexistent']
) LIMIT 1;

.print
.print ========================================
.print Test 8: Mixing json_extract and proto_extract
.print Expected: Error message
.print ========================================

SELECT * FROM nats_scan('telemetry_proto', 
    json_extract := ['device_id'],
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id']
) LIMIT 1;

.print
.print ========================================
.print Error handling tests completed!
.print ========================================

