# DuckDB NATS JetStream Extension: Guide

The extension adds one table function, `nats_scan`, which reads the messages stored in a JetStream stream and returns one row per message. It reads a bounded range: the scan ends at the stream's last message as of the start of the query.

## Installation

```sql
INSTALL nats_js FROM community;
LOAD nats_js;
```

The NATS client (cnats) and Protocol Buffers are linked into the extension, so nothing else needs installing.

`nats_scan` needs a NATS server with JetStream enabled. It connects to `nats://localhost:4222` unless a query sets `url`. Development and tests use nats-server 2.15; reading by subject relies on `next_by_subj`, which needs nats-server 2.9 or later.

## Reading a Stream

```sql
SELECT stream, subject, seq, ts_nats, octet_length(payload) AS bytes
FROM nats_scan('telemetry', end_seq := 3);
```

```
┌───────────┬───────────────────────────────────────┬────────┬────────────────────────────┬───────┐
│  stream   │                subject                │  seq   │          ts_nats           │ bytes │
│  varchar  │                varchar                │ uint64 │         timestamp          │ int64 │
├───────────┼───────────────────────────────────────┼────────┼────────────────────────────┼───────┤
│ telemetry │ telemetry.dc1.power.pm5560.pm5560-001 │      1 │ 2026-10-21 23:10:23.776818 │   183 │
│ telemetry │ telemetry.dc1.power.pm5560.pm5560-002 │      2 │ 2026-10-21 23:10:23.777079 │   183 │
│ telemetry │ telemetry.dc1.power.pm5560.pm5560-003 │      3 │ 2026-10-21 23:10:23.777243 │   183 │
└───────────┴───────────────────────────────────────┴────────┴────────────────────────────┴───────┘
```

The five base columns are:

| Column | Type | Contents |
|--------|------|----------|
| `stream` | VARCHAR | Stream name |
| `subject` | VARCHAR | Message subject |
| `seq` | UBIGINT | Stream sequence number |
| `ts_nats` | TIMESTAMP | Time the server stored the message, in UTC |
| `payload` | BLOB, or VARCHAR with `json_extract` | Message data |

Extracted fields follow as extra columns. Message headers are not returned yet.

### Sequence Ranges

```sql
SELECT seq, ts_nats, payload
FROM nats_scan('telemetry', start_seq := 1000, end_seq := 2000);
```

Both ends are inclusive. `start_seq` defaults to the stream's first message and `end_seq` to its last. Deleted sequences are skipped.

### Time Ranges

```sql
SELECT seq, ts_nats, subject
FROM nats_scan('telemetry',
    start_time := TIMESTAMP '2026-10-21 09:00:00',
    end_time := TIMESTAMP '2026-10-21 17:00:00'
);
```

Both ends are inclusive and compared with `ts_nats` at microsecond precision, in UTC. The start is found by binary search, which takes about 20 requests on a stream of a million messages; the scan stops at the first message after `end_time`. A time range cannot be combined with `start_seq` or `end_seq`.

To compare with the current time, convert it to UTC: `start_time := now() AT TIME ZONE 'UTC' - INTERVAL 1 HOUR`.

### Subject Filters

```sql
SELECT seq, subject
FROM nats_scan('telemetry', subject := 'telemetry.dc1.power.>');
```

`subject` is a NATS subject filter: `*` matches one token and `>` matches the remaining tokens. The server applies it, so messages on other subjects are never read. A filter without wildcards matches one subject exactly. For substring matches, filter in SQL with `WHERE subject LIKE ...`, which reads the whole range.

## JSON Payloads

`json_extract` lists fields to extract. Each becomes a VARCHAR column, and `payload` becomes VARCHAR too:

```sql
SELECT seq, device_id, kw, voltage, pf
FROM nats_scan('telemetry', start_seq := 1, end_seq := 4,
    json_extract := ['device_id', 'kw', 'voltage', 'pf']);
```

```
┌────────┬────────────┬─────────┬─────────┬─────────┐
│  seq   │ device_id  │   kw    │ voltage │   pf    │
│ uint64 │  varchar   │ varchar │ varchar │ varchar │
├────────┼────────────┼─────────┼─────────┼─────────┤
│      1 │ pm5560-001 │ 49.64   │ 480.3   │ 0.926   │
│      2 │ pm5560-002 │ 56.51   │ 482.4   │ 0.896   │
│      3 │ pm5560-003 │ 62.06   │ 484.4   │ 0.872   │
│      4 │ pm5560-004 │ 81.1    │ 479.5   │ 0.892   │
└────────┴────────────┴─────────┴─────────┴─────────┘
```

| JSON value | Column value |
|------------|--------------|
| string | the string, unescaped |
| number | the number as written in the message, e.g. `42`, `1e-7`, `9007199254740993` |
| `true`, `false` | `'true'`, `'false'` |
| `null`, missing field | NULL |
| object, array | JSON text |

A message that is not valid JSON gives NULL fields. A payload that is not valid UTF-8 reads as a NULL `payload`, since the column is VARCHAR.

Field names are dot paths: `'location.zone'` reads `{"location": {"zone": ...}}`, and numeric parts index arrays (`'phases.0.amps'`). A key that contains dots is matched literally first. Column names replace dots with underscores (`location_zone`). A field named like another column gets a numeric suffix: extracting `'subject'` gives a `subject_1` column.

Cast the text for arithmetic:

```sql
SELECT device_id, round(avg(kw::DOUBLE), 2) AS avg_kw, max(kw::DOUBLE) AS peak_kw, count(*) AS readings
FROM nats_scan('telemetry', subject := 'telemetry.*.power.>', json_extract := ['device_id', 'kw'])
GROUP BY device_id
ORDER BY avg_kw DESC;
```

```
┌────────────┬────────┬─────────┬──────────┐
│ device_id  │ avg_kw │ peak_kw │ readings │
│  varchar   │ double │ double  │  int64   │
├────────────┼────────┼─────────┼──────────┤
│ pm5560-005 │  90.38 │  119.69 │       61 │
│ pm5560-004 │  69.11 │   89.27 │       61 │
│ pm5560-003 │  67.73 │   86.58 │       61 │
│ pm5560-001 │  45.82 │   59.97 │       61 │
│ pm5560-002 │  45.61 │    59.6 │       61 │
└────────────┴────────┴─────────┴──────────┘
```

DuckDB's own JSON functions work on the payload too, for example `decode(payload)->>'$.zone'`.

## Protocol Buffers Payloads

`proto_file` is the path to a `.proto` file, `proto_message` a message type defined in it, and `proto_extract` the fields to read. The schema is parsed when the query is bound, so a missing file or field fails before any message is read.

```sql
SELECT device_id, timestamp, location_zone, metrics_kw, online, tags
FROM nats_scan('telemetry_proto', end_seq := 4,
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['device_id', 'timestamp', 'location.zone', 'metrics.kw', 'online', 'tags']);
```

```
┌────────────┬───────────────┬───────────────┬────────────┬─────────┬──────────────────────────┐
│ device_id  │   timestamp   │ location_zone │ metrics_kw │ online  │           tags           │
│  varchar   │     int64     │    varchar    │   double   │ boolean │         varchar          │
├────────────┼───────────────┼───────────────┼────────────┼─────────┼──────────────────────────┤
│ pm5560-001 │ 1792620624069 │ dc1           │      5.462 │ true    │ ["power","dc1","active"] │
│ pm5560-002 │ 1792620624069 │ dc1           │      4.658 │ true    │ ["power","dc1","active"] │
│ pm5560-003 │ 1792620624069 │ dc2           │      4.552 │ true    │ ["power","dc2","active"] │
│ pm5560-004 │ 1792620624069 │ dc2           │      4.638 │ true    │ ["power","dc2","active"] │
└────────────┴───────────────┴───────────────┴────────────┴─────────┴──────────────────────────┘
```

Dot paths go through nested messages, and column names replace dots with underscores. A path cannot continue past a repeated field; extract the whole field instead.

Imports resolve against the `.proto` file's directory and protobuf's well-known types, such as `google/protobuf/timestamp.proto`. A well-known type reads as its JSON form (a `Timestamp` is an RFC 3339 string); its fields can be extracted too, as in `'created_at.seconds'`.

### Type Mapping

| Protobuf type | DuckDB type |
|---------------|-------------|
| `string` | VARCHAR |
| `bytes` | BLOB |
| `int32`, `sint32`, `sfixed32` | INTEGER |
| `int64`, `sint64`, `sfixed64` | BIGINT |
| `uint32`, `fixed32` | UINTEGER |
| `uint64`, `fixed64` | UBIGINT |
| `float` | FLOAT |
| `double` | DOUBLE |
| `bool` | BOOLEAN |
| `enum` | VARCHAR, the value's name |
| message | VARCHAR, a JSON object |
| `repeated` field | VARCHAR, a JSON array |
| `map` field | VARCHAR, a JSON object |

In the JSON of repeated and map fields, numbers are JSON numbers, bytes are base64 and enums are names. Messages, whether a field or an array element, follow protobuf's JSON mapping with the field names from the `.proto` file; it writes an int64 that a double cannot hold exactly as a string. Cast an array to a list with `tags::JSON::VARCHAR[]`.

Fields with presence (messages, proto3 `optional` fields and `oneof` members) are NULL when unset. Other proto3 fields read as their default value: `0`, `''` or `false`. A path through an unset message is NULL. A payload that does not parse gives NULL fields.

## How Reads Work

`nats_scan` creates no consumers. It connects when the query starts, reads the stream info, and then requests one message at a time: the first message at or after a sequence whose subject matches the filter (`>` without one). Deleted sequences are skipped on the server, and the next request starts after the returned message.

Streams with `allow_direct` are read with direct gets, which any replica can answer. Other streams are read through the JetStream API (`$JS.API.STREAM.MSG.GET`), which the stream leader answers.

The scan stops at the stream's last sequence as recorded at the start, so it finishes on a stream that is still receiving messages. Each request waits up to 5 seconds. The scan runs on one thread and returns rows in chunks of 2,048, and a query can be cancelled between messages.

Reading one message per request makes the server's round trip the main cost: roughly 30,000 messages a second on loopback, and about 1,000 a second over a 1 ms network round trip. Narrow scans with `subject`, sequence or time options rather than `WHERE`, which filters after reading.

## Security

With `SET enable_external_access = false`, `nats_scan` refuses to run, since it opens network connections and reads `.proto` files.

## API Reference

```
nats_scan(stream, [subject, url, start_seq, end_seq, start_time, end_time,
                   json_extract, proto_file, proto_message, proto_extract])
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `stream` | VARCHAR | required | Stream name (positional) |
| `url` | VARCHAR | `nats://localhost:4222` | NATS server URL |
| `subject` | VARCHAR | all subjects | NATS subject filter, with `*` and `>` wildcards |
| `start_seq` | UBIGINT | first message | First sequence, inclusive |
| `end_seq` | UBIGINT | last message | Last sequence, inclusive |
| `start_time` | TIMESTAMP | - | Earliest message time in UTC, inclusive |
| `end_time` | TIMESTAMP | - | Latest message time in UTC, inclusive |
| `json_extract` | VARCHAR[] | - | JSON fields to extract, as dot paths |
| `proto_file` | VARCHAR | - | Path to the `.proto` file |
| `proto_message` | VARCHAR | - | Message type in `proto_file` |
| `proto_extract` | VARCHAR[] | - | Protobuf fields to extract, as dot paths |

Rules:

- Sequence options (`start_seq`, `end_seq`) cannot be combined with time options (`start_time`, `end_time`).
- `json_extract` and `proto_extract` cannot be combined.
- `proto_extract` requires `proto_file` and `proto_message`.
- Options cannot be NULL, and field lists cannot contain NULL or empty names.

## Roadmap

Planned for 0.4.0:

- Reads through an ephemeral ordered consumer, which streams messages in batches, with direct gets as the fallback
- Filter pushdown, so `WHERE seq`, `WHERE ts_nats` and `WHERE subject = ...` narrow the read like the options do, and projection pushdown, which skips payload decoding when it is not needed
- A headers column
- Authentication and TLS through DuckDB secrets: credentials files, NKeys, tokens, user and password
- Progress and row estimates from the stream's counters
- Tests against a real server in CI

Later ideas: listing streams and their info, reading KV buckets, native LIST and STRUCT columns for protobuf, well-known types such as `Timestamp` as DuckDB types, imports from other directories, and loading schemas from S3 or HTTP.

## Development

### Building

The build follows DuckDB's extension template and gets cnats and protobuf from vcpkg:

```bash
git clone --recurse-submodules https://github.com/brannn/duckdb-nats-jetstream
cd duckdb-nats-jetstream
git clone https://github.com/microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh
export VCPKG_TOOLCHAIN_PATH=$PWD/vcpkg/scripts/buildsystems/vcpkg.cmake
GEN=ninja make release
```

The extension is written to `build/release/extension/nats_js/nats_js.duckdb_extension`, next to a DuckDB shell built from the same source in `build/release/duckdb`. Start that shell, or any DuckDB 2.0 shell, with `-unsigned` and load the file:

```sql
LOAD 'build/release/extension/nats_js/nats_js.duckdb_extension';
```

### Tests

```bash
make test               # tests that need no server
make test_integration   # tests against a real nats-server
```

`make test_integration` copies the JetStream store in `test/fixtures/jetstream` to `build/`, serves it with `nats-server` on port 42222 and runs `test/sql/integration`. It needs `nats-server` on the `PATH`, or `NATS_SERVER=/path/to/nats-server`. The store holds synthetic messages with fixed sequences and timestamps; `test/fixtures/make-jetstream-store.sh` regenerates it, using nats-server, the nats CLI and protoc.

### Demo Data

```bash
pip install -r requirements.txt
make generate-data
```

This starts nats-server with Docker Compose, creates the demo streams, and publishes an hour of synthetic JSON readings to `telemetry` and `environmental` and 500 protobuf readings to `telemetry_proto`. `python3 scripts/generate-demo-screenshots.py` renders the README images from queries on that data.

## License

MIT License. See [LICENSE](../LICENSE).
