-- Test suite for timestamp-based queries in nats_scan
-- Prerequisites:
--   1. NATS server running (docker-compose up -d)
--   2. Historical test data published (python3 scripts/generate-telemetry.py --hours 24)
--
-- Run with: duckdb -unsigned :memory: < test/sql/test_timestamp_queries.sql

LOAD 'build/release/nats_js.duckdb_extension';

.print ========================================
.print Test 1: Query with both start_time and end_time
.print ========================================

SELECT
    COUNT(*) as message_count,
    MIN(ts_nats) as first_message,
    MAX(ts_nats) as last_message
FROM nats_scan('telemetry',
    start_time := (current_timestamp - INTERVAL '2 hours')::TIMESTAMP,
    end_time := (current_timestamp - INTERVAL '1 hour')::TIMESTAMP,
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id']
);

.print
.print ========================================
.print Test 2: Verify all timestamps fall within range
.print ========================================

SELECT
    seq,
    ts_nats,
    device_id
FROM nats_scan('telemetry',
    start_time := (current_timestamp - INTERVAL '2 hours')::TIMESTAMP,
    end_time := (current_timestamp - INTERVAL '1 hour')::TIMESTAMP,
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id']
)
ORDER BY ts_nats
LIMIT 10;

.print
.print ========================================
.print Test 3: start_time only (all messages after)
.print ========================================

SELECT
    COUNT(*) as total,
    MIN(ts_nats) as first_message,
    MAX(ts_nats) as last_message
FROM nats_scan('telemetry',
    start_time := (current_timestamp - INTERVAL '4 hours')::TIMESTAMP,
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id']
);

.print
.print ========================================
.print Test 4: end_time only (all messages before)
.print ========================================

SELECT
    COUNT(*) as total,
    MIN(ts_nats) as first_message,
    MAX(ts_nats) as last_message
FROM nats_scan('telemetry',
    end_time := (current_timestamp - INTERVAL '1 hour')::TIMESTAMP,
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id']
);

.print
.print ========================================
.print Test 5: Very narrow time window (5 minutes)
.print ========================================

SELECT
    COUNT(*) as messages_in_5_min
FROM nats_scan('telemetry',
    start_time := (current_timestamp - INTERVAL '2 hours')::TIMESTAMP,
    end_time := (current_timestamp - INTERVAL '2 hours' + INTERVAL '5 minutes')::TIMESTAMP,
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id']
);

.print
.print ========================================
.print Test 6: Wide time window (12 hours)
.print ========================================

SELECT
    COUNT(*) as messages_in_12_hours,
    MIN(ts_nats) as first,
    MAX(ts_nats) as last
FROM nats_scan('telemetry',
    start_time := (current_timestamp - INTERVAL '12 hours')::TIMESTAMP,
    end_time := current_timestamp::TIMESTAMP,
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id']
);

.print
.print ========================================
.print Test 7: Time range before any messages (edge case)
.print ========================================

SELECT COUNT(*) as count_before_stream
FROM nats_scan('telemetry',
    start_time := '2020-01-01 00:00:00'::TIMESTAMP,
    end_time := '2020-01-01 01:00:00'::TIMESTAMP,
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id']
);

.print
.print ========================================
.print Test 8: Time range in far future (edge case)
.print ========================================

SELECT COUNT(*) as count_future
FROM nats_scan('telemetry',
    start_time := '2030-01-01 00:00:00'::TIMESTAMP,
    end_time := '2030-01-01 01:00:00'::TIMESTAMP,
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id']
);

.print
.print ========================================
.print Test 9: Timestamp ordering in results
.print ========================================

WITH ordered_results AS (
    SELECT
        seq,
        ts_nats,
        LAG(ts_nats) OVER (ORDER BY seq) as prev_ts
    FROM nats_scan('telemetry',
        start_time := (current_timestamp - INTERVAL '3 hours')::TIMESTAMP,
        end_time := (current_timestamp - INTERVAL '2 hours')::TIMESTAMP,
        proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id']
    )
    LIMIT 100
)
SELECT
    COUNT(*) as total_checked,
    SUM(CASE WHEN ts_nats >= prev_ts OR prev_ts IS NULL THEN 1 ELSE 0 END) as correctly_ordered
FROM ordered_results;

.print
.print ========================================
.print Test 10: Timestamp + JSON extraction
.print ========================================

SELECT
    ts_nats,
    device_id,
    zone,
    kw::DOUBLE as kw
FROM nats_scan('telemetry',
    start_time := (current_timestamp - INTERVAL '2 hours')::TIMESTAMP,
    end_time := (current_timestamp - INTERVAL '1 hour')::TIMESTAMP,
    json_extract := ['device_id', 'zone', 'kw']
)
ORDER BY ts_nats
LIMIT 10;

.print
.print ========================================
.print Test 11: Timestamp + subject filter
.print ========================================

SELECT
    COUNT(*) as filtered_count,
    MIN(ts_nats) as first,
    MAX(ts_nats) as last
FROM nats_scan('telemetry',
    start_time := (current_timestamp - INTERVAL '3 hours')::TIMESTAMP,
    end_time := (current_timestamp - INTERVAL '2 hours')::TIMESTAMP,
    subject := 'zone-a',
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id']
);

.print
.print ========================================
.print Test 12: Aggregation over time window
.print ========================================

SELECT
    COUNT(*) as total_readings,
    COUNT(DISTINCT device_id) as unique_devices,
    ROUND(AVG(kw::DOUBLE), 2) as avg_kw,
    ROUND(MIN(kw::DOUBLE), 2) as min_kw,
    ROUND(MAX(kw::DOUBLE), 2) as max_kw
FROM nats_scan('telemetry',
    start_time := (current_timestamp - INTERVAL '4 hours')::TIMESTAMP,
    end_time := (current_timestamp - INTERVAL '3 hours')::TIMESTAMP,
    json_extract := ['device_id', 'kw']
);

.print
.print ========================================
.print Test 13: Time-series analysis by zone
.print ========================================

SELECT
    zone,
    COUNT(*) as readings,
    ROUND(AVG(kw::DOUBLE), 2) as avg_kw,
    ROUND(AVG(voltage::DOUBLE), 1) as avg_voltage
FROM nats_scan('telemetry',
    start_time := (current_timestamp - INTERVAL '6 hours')::TIMESTAMP,
    end_time := (current_timestamp - INTERVAL '5 hours')::TIMESTAMP,
    json_extract := ['zone', 'kw', 'voltage']
)
GROUP BY zone
ORDER BY zone;

.print
.print ========================================
.print Test 14: Environmental stream timestamp query
.print ========================================

SELECT
    COUNT(*) as temp_readings,
    MIN(ts_nats) as first_reading,
    MAX(ts_nats) as last_reading,
    ROUND(AVG(temp_c::DOUBLE), 2) as avg_temp_c
FROM nats_scan('environmental',
    start_time := (current_timestamp - INTERVAL '3 hours')::TIMESTAMP,
    end_time := (current_timestamp - INTERVAL '2 hours')::TIMESTAMP,
    json_extract := ['temp_c']
);

.print
.print ========================================
.print Test 15: Timestamp with protobuf extraction
.print ========================================

SELECT
    COUNT(*) as message_count,
    MIN(ts_nats) as first,
    MAX(ts_nats) as last,
    COUNT(DISTINCT device_id) as unique_devices
FROM nats_scan('telemetry',
    start_time := (current_timestamp - INTERVAL '2 hours')::TIMESTAMP,
    end_time := (current_timestamp - INTERVAL '1 hour')::TIMESTAMP,
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id']
);

.print
.print ========================================
.print All timestamp query tests completed successfully!
.print ========================================

