-- Test suite for sequence range queries in nats_scan
-- Prerequisites:
--   1. NATS server running (docker-compose up -d)
--   2. Protobuf test data published (python3 test/proto/generate_protobuf_data.py)
--
-- Run with: duckdb -unsigned :memory: < test/sql/test_sequence_ranges.sql

LOAD 'build/release/nats_js.duckdb_extension';

.print ========================================
.print Test 1: Basic sequence range (already tested in test_protobuf.sql)
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
.print Test 2: First message only (sequence boundary)
.print ========================================

SELECT
    seq,
    device_id,
    location_zone
FROM nats_scan('telemetry_proto',
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id', 'location.zone'],
    start_seq := 1,
    end_seq := 1
);

.print
.print ========================================
.print Test 3: Last 10 messages
.print ========================================

-- Get the last sequence number first
CREATE TEMP TABLE stream_info AS
SELECT MAX(seq) as last_seq
FROM nats_scan('telemetry_proto',
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id']
);

-- Use the last sequence to query the last 10 messages
SELECT
    seq,
    device_id,
    metrics_kw
FROM nats_scan('telemetry_proto',
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id', 'metrics.kw']
)
WHERE seq >= (SELECT last_seq - 9 FROM stream_info)
ORDER BY seq;

.print
.print ========================================
.print Test 4: Large sequence range
.print ========================================

SELECT
    COUNT(*) as total_in_range,
    MIN(seq) as first_seq,
    MAX(seq) as last_seq
FROM nats_scan('telemetry_proto',
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id'],
    start_seq := 1,
    end_seq := 1000
);

.print
.print ========================================
.print Test 5: Detect sequence gaps
.print ========================================

WITH seq_with_gaps AS (
    SELECT
        seq,
        LAG(seq) OVER (ORDER BY seq) as prev_seq,
        seq - LAG(seq) OVER (ORDER BY seq) as gap
    FROM nats_scan('telemetry_proto',
        proto_file := 'test/proto/telemetry.proto',
        proto_message := 'Telemetry',
        proto_extract := ['device_id'],
        start_seq := 1,
        end_seq := 100
    )
)
SELECT seq, prev_seq, gap
FROM seq_with_gaps
WHERE gap > 1;

.print
.print ========================================
.print Test 6: start_seq only (all messages from sequence onward)
.print ========================================

SELECT
    COUNT(*) as count_from_seq,
    MIN(seq) as first_seq,
    MAX(seq) as last_seq
FROM nats_scan('telemetry_proto',
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id'],
    start_seq := 100
);

.print
.print ========================================
.print Test 7: end_seq only (all messages up to sequence)
.print ========================================

SELECT
    COUNT(*) as count_until_seq,
    MIN(seq) as first_seq,
    MAX(seq) as last_seq
FROM nats_scan('telemetry_proto',
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id'],
    end_seq := 50
);

.print
.print ========================================
.print Test 8: Sequence range with protobuf extraction
.print ========================================

SELECT
    seq,
    device_id,
    metrics_kw as kw,
    location_zone as zone
FROM nats_scan('telemetry_proto',
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id', 'metrics.kw', 'location.zone'],
    start_seq := 50,
    end_seq := 100
)
ORDER BY seq
LIMIT 10;

.print
.print ========================================
.print Test 9: Verify continuous sequence ordering
.print ========================================

WITH ordered AS (
    SELECT
        seq,
        ROW_NUMBER() OVER (ORDER BY seq) as expected_position
    FROM nats_scan('telemetry_proto',
        proto_file := 'test/proto/telemetry.proto',
        proto_message := 'Telemetry',
        proto_extract := ['device_id'],
        start_seq := 10,
        end_seq := 30
    )
)
SELECT
    COUNT(*) as total_messages,
    MIN(seq) as first_seq,
    MAX(seq) as last_seq,
    MAX(seq) - MIN(seq) + 1 as expected_count
FROM ordered;

.print
.print ========================================
.print Test 10: Sequence range + subject filter
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
LIMIT 10;

.print
.print ========================================
.print Test 11: Aggregation over sequence range
.print ========================================

SELECT
    location_zone,
    COUNT(*) as message_count,
    ROUND(AVG(metrics_kw), 2) as avg_kw,
    MIN(seq) as first_seq,
    MAX(seq) as last_seq
FROM nats_scan('telemetry_proto',
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['location.zone', 'metrics.kw'],
    start_seq := 1,
    end_seq := 200
)
GROUP BY location_zone
ORDER BY location_zone;

.print
.print ========================================
.print Test 12: Environmental stream sequence range
.print ========================================

-- Get the first sequence number from the environmental stream
CREATE TEMP TABLE env_stream_info AS
SELECT MIN(seq) as first_seq
FROM nats_scan('environmental',
    json_extract := ['device_id']
);

-- Query a range of 20 messages starting from the first sequence
SELECT
    seq,
    device_id,
    zone,
    temp_c::DOUBLE as temp_c
FROM nats_scan('environmental',
    json_extract := ['device_id', 'zone', 'temp_c']
)
WHERE seq >= (SELECT first_seq FROM env_stream_info)
  AND seq < (SELECT first_seq + 20 FROM env_stream_info)
ORDER BY seq;

.print
.print ========================================
.print All sequence range tests completed successfully!
.print ========================================

