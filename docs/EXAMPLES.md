# NATS JetStream Extension Examples

Recipes for common queries. They use the demo streams that `make generate-data` creates (see the [Guide](GUIDE.md#demo-data)):

- `telemetry`: JSON power readings on subjects such as `telemetry.dc1.power.pm5560.pm5560-001`
- `telemetry_proto`: the same readings encoded with `test/proto/telemetry.proto`, on subjects such as `telemetry_proto.dc1.power.pm5560.pm5560-001`

The server is `nats://localhost:4222` unless a query sets `url`.

## Installation

```sql
INSTALL nats_js FROM community;
LOAD nats_js;
```

## Reading Messages

### All Messages

Each row is one stored message. `payload` is a BLOB:

```sql
SELECT stream, subject, seq, ts_nats, payload
FROM nats_scan('telemetry');
```

### Sequence Range

Both ends are inclusive. Deleted sequences are skipped:

```sql
SELECT seq, ts_nats, subject
FROM nats_scan('telemetry', start_seq := 1000, end_seq := 2000);
```

### Time Range

Both ends are inclusive, at microsecond precision. Times are UTC:

```sql
SELECT seq, ts_nats, subject
FROM nats_scan('telemetry',
    start_time := TIMESTAMP '2026-10-21 09:00:00',
    end_time := TIMESTAMP '2026-10-21 17:00:00'
);
```

### The Last Hour

`now()` is a TIMESTAMPTZ; convert it to UTC before comparing with stream times:

```sql
SELECT count(*)
FROM nats_scan('telemetry', start_time := now() AT TIME ZONE 'UTC' - INTERVAL 1 HOUR);
```

### Payload as Text

`decode` turns a UTF-8 payload into VARCHAR. It fails on payloads that are not valid UTF-8:

```sql
SELECT seq, decode(payload) AS text
FROM nats_scan('telemetry', end_seq := 10);
```

### Headers

`headers` is a map from each header name to a list of its values:

```sql
SELECT seq, headers['Nats-Msg-Id'][1] AS msg_id
FROM nats_scan('telemetry', end_seq := 10);

SELECT count(*)
FROM nats_scan('telemetry')
WHERE headers['Content-Type'][1] = 'application/json';
```

## Filtering by Subject

The `subject` option is a NATS subject filter, applied by the server, so messages on other subjects are not read.

### One Subject

```sql
SELECT seq, ts_nats
FROM nats_scan('telemetry', subject := 'telemetry.dc1.power.pm5560.pm5560-001');
```

### Wildcards

`*` matches one token and `>` matches one or more tokens at the end:

```sql
-- every power reading in dc1
SELECT count(*) FROM nats_scan('telemetry', subject := 'telemetry.dc1.power.>');

-- meter pm5560-001 in any datacenter
SELECT count(*) FROM nats_scan('telemetry', subject := 'telemetry.*.power.pm5560.pm5560-001');
```

A filter without wildcards matches only that exact subject: `'telemetry.dc1.power'` matches nothing in the demo stream.

### In WHERE

`WHERE subject = '...'` and prefixes that end with a dot, such as `LIKE 'telemetry.dc1.%'`, narrow the read like the option does:

```sql
SELECT count(*) FROM nats_scan('telemetry') WHERE subject LIKE 'telemetry.dc1.%';
```

### Substring Matches

For anything a subject filter cannot express, filter in SQL. This reads every message in the range:

```sql
SELECT seq, subject
FROM nats_scan('telemetry')
WHERE subject LIKE '%pm5560-00%';
```

## JSON Messages

### Extract Fields

Each field becomes a VARCHAR column. With `json_extract`, `payload` is VARCHAR as well:

```sql
SELECT device_id, kw, voltage, pf
FROM nats_scan('telemetry', json_extract := ['device_id', 'kw', 'voltage', 'pf']);
```

Strings are unescaped, numbers keep the digits they were written with, `true` and `false` stay as text, JSON `null` and missing fields are NULL, and objects and arrays come back as JSON text.

### Nested Fields

Dot paths follow nested objects, and numeric parts index arrays. The column name replaces dots with underscores. For a payload such as `{"device": {"id": "m1"}, "phases": [{"amps": 12.5}]}`:

```sql
SELECT device_id, phases_0_amps
FROM nats_scan('readings', json_extract := ['device.id', 'phases.0.amps']);
```

A key that itself contains dots, such as `{"device.id": "m1"}`, is matched first.

### Aggregate

Cast the extracted text for arithmetic:

```sql
SELECT device_id,
       round(avg(kw::DOUBLE), 2) AS avg_kw,
       max(kw::DOUBLE) AS peak_kw,
       count(*) AS readings
FROM nats_scan('telemetry',
    subject := 'telemetry.*.power.>',
    json_extract := ['device_id', 'kw']
)
GROUP BY device_id
ORDER BY avg_kw DESC;
```

### Filter

```sql
SELECT device_id, kw, voltage
FROM nats_scan('telemetry', json_extract := ['device_id', 'kw', 'voltage'])
WHERE kw::DOUBLE > 50.0
  AND voltage::DOUBLE BETWEEN 475.0 AND 485.0;
```

### Anything Else with DuckDB's JSON Functions

The payload can go straight to DuckDB's JSON functions:

```sql
SELECT decode(payload)->>'$.device_id' AS device_id,
       decode(payload)->>'$.zone' AS zone
FROM nats_scan('telemetry', end_seq := 100);
```

## Protocol Buffers Messages

`proto_file` is the path to a `.proto` file, `proto_message` the message type in it, and `proto_extract` the fields to read. Columns get DuckDB types from the schema.

### Extract Fields

```sql
SELECT device_id, timestamp, online, firmware_version
FROM nats_scan('telemetry_proto',
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id', 'timestamp', 'online', 'firmware_version']
);
```

### Nested Fields

Dot paths go through nested messages. A field under a message that is not set is NULL:

```sql
SELECT device_id, location_zone, metrics_kw, metrics_voltage
FROM nats_scan('telemetry_proto',
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id', 'location.zone', 'metrics.kw', 'metrics.voltage']
);
```

Extracting a message field itself, such as `'location'`, returns it as a JSON object.

### Repeated Fields

Repeated fields are JSON arrays and map fields JSON objects. Cast to a list to unnest:

```sql
SELECT device_id, unnest(tags::JSON::VARCHAR[]) AS tag
FROM nats_scan('telemetry_proto',
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id', 'tags']
);
```

A path cannot continue past a repeated field: extract the whole field instead of `'items.name'`.

### Aggregate

Numeric fields need no casts:

```sql
SELECT location_zone,
       count(*) AS readings,
       round(avg(metrics_kw), 2) AS avg_kw,
       max(metrics_voltage) AS peak_voltage
FROM nats_scan('telemetry_proto',
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['location.zone', 'metrics.kw', 'metrics.voltage']
)
GROUP BY location_zone
ORDER BY location_zone;
```

### Filter

```sql
SELECT device_id, metrics_kw, metrics_voltage
FROM nats_scan('telemetry_proto',
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id', 'metrics.kw', 'metrics.voltage', 'online']
)
WHERE metrics_kw > 5.0
  AND online;
```

## Combined Patterns

### Time Range and Subject

```sql
SELECT seq, ts_nats, subject, device_id, kw
FROM nats_scan('telemetry',
    subject := 'telemetry.dc1.power.>',
    start_time := TIMESTAMP '2026-10-21 09:00:00',
    end_time := TIMESTAMP '2026-10-21 17:00:00',
    json_extract := ['device_id', 'kw']
)
ORDER BY seq;
```

### Sequence and Time Together

```sql
-- from sequence 1,000,000 up to a time
SELECT count(*)
FROM nats_scan('telemetry', start_seq := 1000000, end_time := TIMESTAMP '2026-10-21 12:00:00');
```

### Another Server

```sql
SELECT seq, device_id, kw
FROM nats_scan('telemetry',
    url := 'nats://nats.example.com:4222',
    json_extract := ['device_id', 'kw']
)
LIMIT 100;
```

### A Server That Wants Credentials

The secret applies to URLs that start with its scope. See the [Guide](GUIDE.md#authentication-and-tls) for tokens, NKeys, credentials files and client certificates:

```sql
CREATE SECRET prod (
    TYPE nats,
    USER 'reporting',
    PASSWORD '...',
    TLS_CA '/etc/nats/ca.pem',
    SCOPE 'tls://nats.example.com:4222'
);

SELECT count(*) FROM nats_scan('telemetry', url := 'tls://nats.example.com:4222');

-- or name the secret. Give it a scope as well, since a secret without one applies to every URL.
CREATE SECRET reporting (TYPE nats, CREDS '/etc/nats/reporting.creds', SCOPE 'tls://reports.example.com:4222');
SELECT count(*) FROM nats_scan('telemetry', url := 'tls://nats.example.com:4222', secret := 'reporting');
```

### Export to Parquet

```sql
COPY (
    SELECT ts_nats, device_id, kw::DOUBLE AS kw, voltage::DOUBLE AS voltage
    FROM nats_scan('telemetry',
        start_time := TIMESTAMP '2026-10-21 00:00:00',
        end_time := TIMESTAMP '2026-10-21 23:59:59',
        json_extract := ['device_id', 'kw', 'voltage']
    )
) TO 'telemetry_2026-10-21.parquet' (FORMAT parquet);
```

### Join with a Table

```sql
SELECT d.device_name, d.site, t.kw, t.ts_nats
FROM nats_scan('telemetry',
    subject := 'telemetry.dc1.power.>',
    json_extract := ['device_id', 'kw']
) t
JOIN devices d ON t.device_id = d.device_id;
```

## Errors

A missing schema file:

```sql
SELECT * FROM nats_scan('telemetry_proto',
    proto_file := 'nonexistent.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id']
);
-- IO Error: Failed to import protobuf schema file: nonexistent.proto
-- nonexistent.proto:-1:0: File not found.
```

A field the schema does not have:

```sql
SELECT * FROM nats_scan('telemetry_proto',
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['kw']
);
-- Binder Error: Field 'kw' not found in message type 'Telemetry' (field path: kw)
```

An unreachable server or a missing stream:

```sql
SELECT * FROM nats_scan('telemetry', url := 'nats://invalid-host:4222');
-- IO Error: Failed to connect to NATS at 'nats://invalid-host:4222': No server available for connection

SELECT * FROM nats_scan('telemetri');
-- IO Error: Stream 'telemetri' not found
```

## Performance

The number of messages read sets the cost. Reads are fastest on nats-server 2.11 or later from streams with `allow_direct`, which return messages in batches (see the [Guide](GUIDE.md#how-reads-work)).

### Narrow the Read

The options and `WHERE` clauses on `seq`, `ts_nats` and `subject` narrow what is read. Other conditions filter after reading:

```sql
-- read only the matching messages
SELECT count(*) FROM nats_scan('telemetry', subject := 'telemetry.dc1.power.>');
SELECT count(*) FROM nats_scan('telemetry') WHERE subject LIKE 'telemetry.dc1.power.%';
SELECT count(*) FROM nats_scan('telemetry') WHERE seq BETWEEN 1000000 AND 1100000;

-- read the whole stream, then filter
SELECT count(*) FROM nats_scan('telemetry') WHERE subject LIKE '%power%';
SELECT count(*) FROM nats_scan('telemetry') WHERE seq > 1000000 OR subject = 'telemetry.dc2.power.pm5560.pm5560-003';
```

`EXPLAIN` shows what the scan reads:

```sql
EXPLAIN SELECT count(*) FROM nats_scan('telemetry') WHERE seq > 1000000 AND subject LIKE 'telemetry.dc1.%';
```

```
╭─ Nats Scan ───────────────┴───────────────────────────╮
│ Stream: telemetry                                     │
│ Subject: telemetry.dc1.>                              │
│ Sequences: from 1000001                               │
│ Projections: seq, subject                             │
│ ~1 row                                                │
╰───────────────────────────────────────────────────────╯
```

### Only the Columns You Need

Payloads are decoded only for the field columns a query uses, so naming columns is cheaper than `SELECT *` with `json_extract` or `proto_extract`.

### LIMIT

A `LIMIT` without `ORDER BY` stops the scan once enough rows are read. With `ORDER BY`, every message in the range is read first:

```sql
-- a quick look: reads at most a few thousand messages
SELECT * FROM nats_scan('telemetry') LIMIT 10;

-- the latest messages: use a time range instead of ORDER BY seq DESC
SELECT * FROM nats_scan('telemetry', start_time := now() AT TIME ZONE 'UTC' - INTERVAL 5 MINUTE);
```
