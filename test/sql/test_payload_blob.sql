-- Test that payload column is BLOB when no extraction parameters are specified
-- This prevents UTF-8 validation errors on binary data (e.g., protobuf)

LOAD 'build/release/extension/nats_js/nats_js.duckdb_extension';

.print ========================================
.print Test 1: Payload is BLOB without extraction
.print ========================================

-- Query without any extraction parameters should return BLOB payload
SELECT 
    seq,
    subject,
    typeof(payload) as payload_type,
    octet_length(payload) as payload_size
FROM nats_scan('telemetry')
LIMIT 5;

.print
.print ========================================
.print Test 2: Can query metadata without errors
.print ========================================

-- This query previously failed with UTF-8 validation error
SELECT seq, ts_nats, subject
FROM nats_scan('telemetry')
LIMIT 10;

.print
.print ========================================
.print Test 3: Payload is BLOB with protobuf extraction
.print ========================================

-- Protobuf extraction should also return BLOB payload
SELECT 
    seq,
    device_id,
    typeof(payload) as payload_type,
    octet_length(payload) as payload_size
FROM nats_scan('telemetry_proto',
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id'])
LIMIT 5;

.print
.print ========================================
.print Test 4: Can cast BLOB payload to VARCHAR if needed
.print ========================================

-- Users can manually cast BLOB to VARCHAR if they know it's valid UTF-8
SELECT 
    seq,
    subject,
    CAST(payload AS VARCHAR) as payload_text
FROM nats_scan('telemetry')
WHERE seq = 1;

.print
.print ========================================
.print All payload BLOB tests completed!
.print ========================================

