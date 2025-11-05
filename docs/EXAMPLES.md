# NATS JetStream Extension Examples

This document provides practical examples for common use cases of the DuckDB NATS JetStream extension. Each example includes complete SQL queries and expected output formats.

## Installation

Install the extension from the DuckDB community repository:

```sql
INSTALL nats_js FROM community;
LOAD nats_js;
```

## Basic Message Retrieval

### Query All Messages

Retrieve all messages from a stream. The payload column returns binary data as BLOB type:

```sql
SELECT stream, subject, seq, ts_nats, payload
FROM nats_scan('telemetry');
```

### Query Specific Sequence Range

Retrieve messages between sequence numbers 1000 and 2000:

```sql
SELECT seq, ts_nats, subject
FROM nats_scan('telemetry', 
    start_seq := 1000, 
    end_seq := 2000
);
```

### Query by Timestamp Range

Retrieve messages within a specific time window:

```sql
SELECT seq, ts_nats, subject
FROM nats_scan('telemetry',
    start_time := '2025-11-01 09:00:00'::TIMESTAMP,
    end_time := '2025-11-01 17:00:00'::TIMESTAMP
);
```

### Filter by Subject

Retrieve messages matching a subject pattern:

```sql
SELECT seq, subject, ts_nats
FROM nats_scan('telemetry', 
    subject := 'telemetry.dc1.power'
);
```

The subject parameter performs substring matching on message subjects.

## JSON Message Processing

### Extract JSON Fields

Extract specific fields from JSON-encoded messages:

```sql
SELECT device_id, kw, voltage, pf
FROM nats_scan('telemetry',
    json_extract := ['device_id', 'kw', 'voltage', 'pf']
);
```

When using json_extract, the payload column returns VARCHAR type containing the original JSON string.

### Extract Nested JSON Fields

Access nested JSON properties using dot notation:

```sql
SELECT device_id, location_zone, metrics_kw
FROM nats_scan('telemetry',
    json_extract := ['device_id', 'location.zone', 'metrics.kw']
);
```

### Aggregate JSON Data

Perform aggregations on extracted JSON fields:

```sql
SELECT 
    device_id,
    AVG(kw::DOUBLE) as avg_power,
    MAX(kw::DOUBLE) as peak_power,
    COUNT(*) as reading_count
FROM nats_scan('telemetry',
    start_time := '2025-11-01 00:00:00'::TIMESTAMP,
    end_time := '2025-11-01 23:59:59'::TIMESTAMP,
    json_extract := ['device_id', 'kw']
)
GROUP BY device_id
ORDER BY avg_power DESC;
```

JSON fields are extracted as VARCHAR and require explicit casting for numeric operations.

### Filter JSON Data

Apply WHERE clause filters to extracted JSON fields:

```sql
SELECT device_id, kw, voltage
FROM nats_scan('telemetry',
    json_extract := ['device_id', 'kw', 'voltage']
)
WHERE kw::DOUBLE > 50.0
  AND voltage::DOUBLE BETWEEN 475.0 AND 485.0;
```

## Protocol Buffers Processing

### Extract Protobuf Fields

Extract fields from protobuf-encoded messages using a schema file:

```sql
SELECT device_id, kw, voltage, online
FROM nats_scan('telemetry',
    proto_file := 'schemas/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id', 'kw', 'voltage', 'online']
);
```

The proto_file parameter specifies the path to the .proto schema file, and proto_message specifies the message type name.

### Extract Nested Protobuf Fields

Access nested message fields using dot notation:

```sql
SELECT device_id, location_zone, metrics_kw, metrics_voltage
FROM nats_scan('telemetry',
    proto_file := 'schemas/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id', 'location.zone', 'metrics.kw', 'metrics.voltage']
);
```

### Aggregate Protobuf Data

Perform aggregations on protobuf fields with native type support:

```sql
SELECT 
    device_id,
    AVG(kw) as avg_power,
    MAX(voltage) as peak_voltage,
    COUNT(*) as reading_count
FROM nats_scan('telemetry',
    start_time := '2025-11-01 00:00:00'::TIMESTAMP,
    end_time := '2025-11-01 23:59:59'::TIMESTAMP,
    proto_file := 'schemas/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id', 'kw', 'voltage']
)
GROUP BY device_id
ORDER BY avg_power DESC;
```

Protobuf numeric fields are extracted with native DuckDB types and do not require casting.

### Filter Protobuf Data

Apply filters to protobuf fields:

```sql
SELECT device_id, kw, voltage, online
FROM nats_scan('telemetry',
    proto_file := 'schemas/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id', 'kw', 'voltage', 'online']
)
WHERE kw > 50.0
  AND voltage BETWEEN 475.0 AND 485.0
  AND online = true;
```

## Combined Query Patterns

### Time Range with Subject Filter

Combine timestamp range and subject filtering:

```sql
SELECT seq, ts_nats, subject, device_id, kw
FROM nats_scan('telemetry',
    start_time := '2025-11-01 09:00:00'::TIMESTAMP,
    end_time := '2025-11-01 17:00:00'::TIMESTAMP,
    subject := 'telemetry.dc1.power',
    json_extract := ['device_id', 'kw']
)
ORDER BY seq;
```

### Custom NATS Server URL

Connect to a NATS server at a custom URL:

```sql
SELECT seq, device_id, kw
FROM nats_scan('telemetry',
    url := 'nats://nats.example.com:4222',
    json_extract := ['device_id', 'kw']
)
LIMIT 100;
```

### Export to Parquet

Export query results to Parquet format:

```sql
COPY (
    SELECT device_id, kw, voltage, ts_nats
    FROM nats_scan('telemetry',
        start_time := '2025-11-01 00:00:00'::TIMESTAMP,
        end_time := '2025-11-01 23:59:59'::TIMESTAMP,
        json_extract := ['device_id', 'kw', 'voltage']
    )
) TO 'telemetry_2025-11-01.parquet' (FORMAT PARQUET);
```

### Join with DuckDB Tables

Join stream data with existing DuckDB tables:

```sql
SELECT 
    d.device_name,
    d.location,
    t.kw,
    t.voltage,
    t.ts_nats
FROM nats_scan('telemetry',
    start_time := '2025-11-01 09:00:00'::TIMESTAMP,
    end_time := '2025-11-01 17:00:00'::TIMESTAMP,
    json_extract := ['device_id', 'kw', 'voltage']
) t
JOIN devices d ON t.device_id = d.device_id
WHERE d.location = 'dc1';
```

## Working with Payload Data

### Inspect Payload Type

Check the payload column type and size:

```sql
SELECT 
    seq, 
    typeof(payload) as payload_type,
    octet_length(payload) as payload_size
FROM nats_scan('telemetry')
LIMIT 5;
```

### Convert BLOB Payload to VARCHAR

Convert binary payload to text when the data is known to be UTF-8:

```sql
SELECT 
    seq,
    CAST(payload AS VARCHAR) as payload_text
FROM nats_scan('telemetry')
WHERE octet_length(payload) < 1000
LIMIT 10;
```

This conversion is only safe when the payload contains valid UTF-8 text data.

## Error Handling

### Handle Missing Proto Files

The extension returns an error if the specified proto file does not exist:

```sql
SELECT * FROM nats_scan('telemetry',
    proto_file := 'nonexistent.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id']
);
-- Error: Failed to open proto file: nonexistent.proto
```

### Handle Invalid Field Names

The extension returns an error if a requested field does not exist in the protobuf schema:

```sql
SELECT * FROM nats_scan('telemetry',
    proto_file := 'schemas/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['nonexistent_field']
);
-- Error: Field 'nonexistent_field' not found in message 'Telemetry'
```

### Handle Connection Failures

The extension returns an error if it cannot connect to the NATS server:

```sql
SELECT * FROM nats_scan('telemetry',
    url := 'nats://invalid-host:4222'
);
-- Error: Failed to connect to NATS server
```

## Data Pipeline Patterns

### Batch Processing with External Acknowledgment

The extension uses the NATS Direct Get API for read-only access to streams. For data pipelines that require message acknowledgment after processing, use an external consumer workflow:

```bash
# 1. Create a durable consumer with explicit acknowledgment
nats consumer add telemetry etl-processor \
  --filter "telemetry.>" \
  --ack explicit \
  --pull \
  --deliver all \
  --max-deliver=-1 \
  --max-pending=1000

# 2. Get the consumer's current sequence position
CONSUMER_SEQ=$(nats consumer info telemetry etl-processor -j | jq -r '.delivered.stream_seq')

# 3. Process a batch with DuckDB
duckdb << EOF
LOAD nats_js;
COPY (
    SELECT device_id, kw, voltage, ts_nats
    FROM nats_scan('telemetry',
        start_seq := ${CONSUMER_SEQ},
        end_seq := ${CONSUMER_SEQ} + 999,
        json_extract := ['device_id', 'kw', 'voltage']
    )
) TO 'batch_output.parquet' (FORMAT PARQUET);
EOF

# 4. Acknowledge the batch after successful processing
nats consumer next telemetry etl-processor --count 1000 --ack
```

This pattern separates read operations (DuckDB) from acknowledgment (NATS consumer), allowing batch processing with guaranteed delivery semantics.

### Incremental Processing with State Tracking

Track processing state externally to enable incremental batch processing:

```sql
-- Create a state table to track last processed sequence
CREATE TABLE processing_state (
    stream_name VARCHAR,
    last_seq UBIGINT,
    last_updated TIMESTAMP
);

-- Initialize state
INSERT INTO processing_state VALUES ('telemetry', 0, CURRENT_TIMESTAMP);

-- Process next batch
WITH current_state AS (
    SELECT last_seq FROM processing_state WHERE stream_name = 'telemetry'
),
batch AS (
    SELECT seq, device_id, kw, voltage
    FROM nats_scan('telemetry',
        start_seq := (SELECT last_seq + 1 FROM current_state),
        end_seq := (SELECT last_seq + 1000 FROM current_state),
        json_extract := ['device_id', 'kw', 'voltage']
    )
)
INSERT INTO telemetry_processed SELECT * FROM batch;

-- Update state after successful processing
UPDATE processing_state
SET last_seq = (SELECT MAX(seq) FROM telemetry_processed),
    last_updated = CURRENT_TIMESTAMP
WHERE stream_name = 'telemetry';
```

### Scheduled Batch ETL

Combine DuckDB queries with scheduled jobs for periodic batch processing:

```bash
#!/bin/bash
# etl_hourly.sh - Process last hour of telemetry data

HOUR_AGO=$(date -u -d '1 hour ago' '+%Y-%m-%d %H:00:00')
NOW=$(date -u '+%Y-%m-%d %H:00:00')

duckdb analytics.db << EOF
LOAD nats_js;

-- Extract and transform data
CREATE TEMP TABLE hourly_batch AS
SELECT
    device_id,
    DATE_TRUNC('minute', ts_nats) as minute,
    AVG(kw::DOUBLE) as avg_kw,
    MAX(kw::DOUBLE) as max_kw,
    MIN(kw::DOUBLE) as min_kw,
    COUNT(*) as reading_count
FROM nats_scan('telemetry',
    start_time := '${HOUR_AGO}'::TIMESTAMP,
    end_time := '${NOW}'::TIMESTAMP,
    json_extract := ['device_id', 'kw']
)
GROUP BY device_id, minute;

-- Load into warehouse table
INSERT INTO telemetry_hourly_agg SELECT * FROM hourly_batch;
EOF
```

Schedule with cron for automated processing:

```cron
0 * * * * /path/to/etl_hourly.sh
```

## Performance Considerations

### Limit Result Sets

Use LIMIT to restrict the number of messages retrieved:

```sql
SELECT seq, device_id, kw
FROM nats_scan('telemetry',
    json_extract := ['device_id', 'kw']
)
ORDER BY seq DESC
LIMIT 1000;
```

### Use Sequence Ranges for Large Streams

For large streams, query specific sequence ranges rather than the entire stream:

```sql
SELECT COUNT(*) as message_count
FROM nats_scan('telemetry',
    start_seq := 1000000,
    end_seq := 1100000
);
```

### Combine Filters Efficiently

Apply subject filters and time ranges together to reduce the number of messages scanned:

```sql
SELECT device_id, AVG(kw::DOUBLE) as avg_kw
FROM nats_scan('telemetry',
    start_time := '2025-11-01 09:00:00'::TIMESTAMP,
    end_time := '2025-11-01 10:00:00'::TIMESTAMP,
    subject := 'telemetry.dc1',
    json_extract := ['device_id', 'kw']
)
GROUP BY device_id;
```

