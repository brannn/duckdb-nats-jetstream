-- Test suite for JSON extraction support in nats_scan
-- Prerequisites:
--   1. NATS server running (docker-compose up -d)
--   2. JSON test data published (python3 scripts/generate-telemetry.py --hours 2)
--
-- Run with: duckdb -unsigned :memory: < test/sql/test_json_extraction.sql

LOAD 'build/release/nats_js.duckdb_extension';

.print ========================================
.print Test 1: Basic string field extraction
.print ========================================

SELECT
    seq,
    device_id,
    zone
FROM nats_scan('telemetry',
    json_extract := ['device_id', 'zone']
)
LIMIT 5;

.print
.print ========================================
.print Test 2: Extract numeric fields (as VARCHAR)
.print ========================================

SELECT
    device_id,
    kw,
    voltage,
    typeof(kw) as kw_type
FROM nats_scan('telemetry',
    json_extract := ['device_id', 'kw', 'voltage']
)
LIMIT 5;

.print
.print ========================================
.print Test 3: Numeric field casting and filtering
.print ========================================

SELECT
    device_id,
    kw::DOUBLE as kw,
    voltage::DOUBLE as voltage
FROM nats_scan('telemetry',
    json_extract := ['device_id', 'kw', 'voltage']
)
WHERE kw::DOUBLE > 50.0
LIMIT 10;

.print
.print ========================================
.print Test 4: Extract all available fields
.print ========================================

SELECT
    device_id,
    zone,
    kw,
    pf,
    kva,
    voltage,
    current,
    frequency
FROM nats_scan('telemetry',
    json_extract := ['device_id', 'zone', 'kw', 'pf', 'kva', 'voltage', 'current', 'frequency']
)
LIMIT 5;

.print
.print ========================================
.print Test 5: Aggregation on numeric fields
.print ========================================

SELECT
    zone,
    COUNT(*) as reading_count,
    ROUND(AVG(kw::DOUBLE), 2) as avg_kw,
    ROUND(MIN(kw::DOUBLE), 2) as min_kw,
    ROUND(MAX(kw::DOUBLE), 2) as max_kw,
    ROUND(AVG(voltage::DOUBLE), 2) as avg_voltage
FROM nats_scan('telemetry',
    json_extract := ['zone', 'kw', 'voltage']
)
GROUP BY zone
ORDER BY zone;

.print
.print ========================================
.print Test 6: Filtering on string fields
.print ========================================

SELECT
    device_id,
    zone,
    kw::DOUBLE as kw
FROM nats_scan('telemetry',
    json_extract := ['device_id', 'zone', 'kw']
)
WHERE zone = 'zone-a'
LIMIT 10;

.print
.print ========================================
.print Test 7: Combined filtering (string and numeric)
.print ========================================

SELECT
    device_id,
    zone,
    kw::DOUBLE as kw,
    voltage::DOUBLE as voltage
FROM nats_scan('telemetry',
    json_extract := ['device_id', 'zone', 'kw', 'voltage']
)
WHERE zone = 'zone-a'
  AND kw::DOUBLE > 60.0
  AND voltage::DOUBLE BETWEEN 475 AND 485
LIMIT 10;

.print
.print ========================================
.print Test 8: Non-existent field returns NULL
.print ========================================

SELECT
    device_id,
    nonexistent_field,
    typeof(nonexistent_field) as field_type
FROM nats_scan('telemetry',
    json_extract := ['device_id', 'nonexistent_field']
)
LIMIT 5;

.print
.print ========================================
.print Test 9: Payload is VARCHAR when json_extract is used
.print ========================================

SELECT
    seq,
    typeof(payload) as payload_type,
    length(payload) as payload_length
FROM nats_scan('telemetry',
    json_extract := ['device_id']
)
LIMIT 5;

.print
.print ========================================
.print Test 10: Access payload alongside extracted fields
.print ========================================

SELECT
    device_id,
    kw,
    payload
FROM nats_scan('telemetry',
    json_extract := ['device_id', 'kw']
)
LIMIT 3;

.print
.print ========================================
.print Test 11: Environmental stream (different JSON schema)
.print ========================================

SELECT
    device_id,
    zone,
    location,
    temp_c::DOUBLE as temp_c,
    temp_f::DOUBLE as temp_f,
    humidity::DOUBLE as humidity
FROM nats_scan('environmental',
    json_extract := ['device_id', 'zone', 'location', 'temp_c', 'temp_f', 'humidity']
)
LIMIT 5;

.print
.print ========================================
.print Test 12: Temperature aggregation by zone
.print ========================================

SELECT
    zone,
    location,
    COUNT(*) as reading_count,
    ROUND(AVG(temp_c::DOUBLE), 2) as avg_temp_c,
    ROUND(MIN(temp_c::DOUBLE), 2) as min_temp_c,
    ROUND(MAX(temp_c::DOUBLE), 2) as max_temp_c,
    ROUND(AVG(humidity::DOUBLE), 2) as avg_humidity
FROM nats_scan('environmental',
    json_extract := ['zone', 'location', 'temp_c', 'humidity']
)
GROUP BY zone, location
ORDER BY zone, location;

.print
.print ========================================
.print Test 13: Count total messages
.print ========================================

SELECT COUNT(*) as total_messages
FROM nats_scan('telemetry',
    json_extract := ['device_id']
);

.print
.print ========================================
.print Test 14: Order by numeric field
.print ========================================

SELECT
    device_id,
    kw::DOUBLE as kw
FROM nats_scan('telemetry',
    json_extract := ['device_id', 'kw']
)
ORDER BY kw::DOUBLE DESC
LIMIT 10;

.print
.print ========================================
.print Test 15: Join with DuckDB table
.print ========================================

CREATE TEMP TABLE device_metadata AS
SELECT 'pm5560-001' as device_id, 'Production' as environment, 100 as capacity_kw
UNION ALL
SELECT 'pm5560-002', 'Production', 100
UNION ALL
SELECT 'pm5560-003', 'Staging', 150;

SELECT
    t.device_id,
    m.environment,
    m.capacity_kw,
    COUNT(*) as reading_count,
    ROUND(AVG(t.kw::DOUBLE), 2) as avg_kw,
    ROUND(AVG(t.kw::DOUBLE) / m.capacity_kw * 100, 2) as avg_utilization_pct
FROM nats_scan('telemetry',
    json_extract := ['device_id', 'kw']
) t
JOIN device_metadata m ON t.device_id = m.device_id
GROUP BY t.device_id, m.environment, m.capacity_kw
ORDER BY t.device_id;

.print
.print ========================================
.print Test 16: Multi-field grouping and aggregation
.print ========================================

SELECT
    zone,
    device_id,
    COUNT(*) as readings,
    ROUND(AVG(kw::DOUBLE), 2) as avg_kw,
    ROUND(AVG(pf::DOUBLE), 3) as avg_pf,
    ROUND(AVG(voltage::DOUBLE), 1) as avg_voltage
FROM nats_scan('telemetry',
    json_extract := ['zone', 'device_id', 'kw', 'pf', 'voltage']
)
GROUP BY zone, device_id
ORDER BY zone, device_id;

.print
.print ========================================
.print Test 17: Filter with NULL handling
.print ========================================

SELECT
    device_id,
    optional_field
FROM nats_scan('telemetry',
    json_extract := ['device_id', 'optional_field']
)
WHERE optional_field IS NULL
LIMIT 10;

.print
.print ========================================
.print Test 18: Complex WHERE clause with multiple conditions
.print ========================================

SELECT
    device_id,
    zone,
    kw::DOUBLE as kw,
    pf::DOUBLE as pf,
    voltage::DOUBLE as voltage
FROM nats_scan('telemetry',
    json_extract := ['device_id', 'zone', 'kw', 'pf', 'voltage']
)
WHERE zone IN ('zone-a', 'zone-b')
  AND kw::DOUBLE BETWEEN 40 AND 80
  AND pf::DOUBLE > 0.90
  AND voltage::DOUBLE BETWEEN 475 AND 485
LIMIT 20;

.print
.print ========================================
.print All JSON extraction tests completed successfully!
.print ========================================

