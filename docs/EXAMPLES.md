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

