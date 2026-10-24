# DuckDB NATS JetStream Extension: Guide

The extension adds one table function, `nats_scan`, which reads the messages stored in a JetStream stream and returns one row per message. It reads a bounded range: the scan ends at the stream's last message as of the start of the query.

## Installation

```sql
INSTALL nats_js FROM community;
LOAD nats_js;
```

The NATS client (cnats) and Protocol Buffers are linked into the extension, so nothing else needs installing.

`nats_scan` needs a NATS server with JetStream enabled. It connects to `nats://localhost:4222` unless a query sets `url`. Reading by subject relies on `next_by_subj`, which needs nats-server 2.9 or later. Reads are much faster from nats-server 2.11 or later, which returns messages in batches; see [How Reads Work](#how-reads-work). The tests run against nats-server 2.10 and 2.15.

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

The six base columns are:

| Column | Type | Contents |
|--------|------|----------|
| `stream` | VARCHAR | Stream name |
| `subject` | VARCHAR | Message subject |
| `seq` | UBIGINT | Stream sequence number |
| `ts_nats` | TIMESTAMP | Time the server stored the message, in UTC |
| `headers` | MAP(VARCHAR, VARCHAR[]) | Message headers |
| `payload` | BLOB, or VARCHAR with `json_extract` | Message data |

Extracted fields follow as extra columns. Only the columns a query uses are read out of the messages, so a payload is decoded only when one of its fields is selected.

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

Both ends are inclusive and compared with `ts_nats` at microsecond precision, in UTC. On nats-server 2.11 or later, a stream with `allow_direct` finds the start in one request. Otherwise the start is found by binary search, which takes about 20 requests on a stream of a million messages. The scan stops at the first message after `end_time`. Both assume that message times increase with sequence, as they do unless the server's clock moved back, for example when a stream's leader moves to a server whose clock is behind.

Sequence and time options combine: `start_seq := 1000, end_time := ...` reads from sequence 1000 up to that time.

To compare with the current time, convert it to UTC: `start_time := now() AT TIME ZONE 'UTC' - INTERVAL 1 HOUR`.

### Subject Filters

```sql
SELECT seq, subject
FROM nats_scan('telemetry', subject := 'telemetry.dc1.power.>');
```

`subject` is a NATS subject filter: `*` matches one token and `>` matches the remaining tokens. The server applies it, so messages on other subjects are never read. A filter without wildcards matches one subject exactly.

### WHERE Clauses

`WHERE` clauses on `seq`, `ts_nats` and `subject` narrow the read like the options do, so these two queries read the same messages:

```sql
SELECT seq, subject FROM nats_scan('telemetry', start_seq := 1000, end_seq := 2000);
SELECT seq, subject FROM nats_scan('telemetry') WHERE seq BETWEEN 1000 AND 2000;
```

The scan narrows its read for these conditions:

- comparisons (`=`, `<`, `<=`, `>`, `>=`) and `BETWEEN` of `seq` or `ts_nats` with a constant;
- `subject = '...'`;
- `subject LIKE 'a.b.%'` and `starts_with(subject, 'a.b.')`, when the prefix ends with a dot; these become the subject filter `a.b.>`.

The subject or prefix must be a plain one, without `*` or `>` tokens or empty tokens. Conditions joined with `AND` narrow the read together, and they also combine with the options. A `WHERE` clause on `subject` does not replace a `subject` option. Conditions on `ts_nats` read like `start_time` and `end_time`, so they make the same assumption about message times.

DuckDB applies every other condition after reading, such as `OR`, `LIKE '%power%'` or a prefix that ends mid-token. `EXPLAIN` shows the subject, sequences and times the scan reads.

## Headers

`headers` maps each header name to its values, since NATS allows a name to repeat:

```sql
SELECT seq, headers, headers['Nats-Msg-Id'][1] AS msg_id
FROM nats_scan('orders', end_seq := 2);
```

```
┌────────┬──────────────────────────────────────────┬─────────┐
│  seq   │                 headers                  │ msg_id  │
│ uint64 │         map(varchar, varchar[])          │ varchar │
├────────┼──────────────────────────────────────────┼─────────┤
│      1 │ {Nats-Msg-Id=[o-1001], X-Trace=[t1, t2]} │ o-1001  │
│      2 │ {}                                       │ NULL    │
└────────┴──────────────────────────────────────────┴─────────┘
```

A message without headers has an empty map. The headers the server adds to its replies, such as `Nats-Stream` and `Nats-Sequence`, are left out; the base columns hold the same information. A header value that is not valid UTF-8 reads as NULL, and a header name that is not valid UTF-8 is skipped.

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

`nats_scan` creates no consumers. It connects when the query starts and reads the stream info. Then it asks for the messages at or after a sequence whose subject matches the filter (`>` without one). Deleted sequences are skipped on the server, and each request starts after the last message returned.

How many messages a request returns depends on the server and the stream:

| Stream | nats-server 2.11 or later | Older servers |
|--------|---------------------------|---------------|
| `allow_direct` | Direct get batches of up to 2,048 messages or 8 MB | Direct gets, one message per request |
| no `allow_direct` | JetStream API (`$JS.API.STREAM.MSG.GET`), one message per request | The same |

Any replica can answer a direct get, so on a replicated stream a read can miss the latest messages when the replica that answers is behind. Only the stream leader answers the JetStream API.

The server's round trip is the main cost. On loopback, batches read about 700,000 small messages a second, and single messages about 30,000. Over a 1 ms network round trip, single messages drop to about 1,000 a second. For large reads, give the stream `allow_direct` and use nats-server 2.11 or later.

The scan stops at the stream's last sequence as recorded at the start, so it finishes on a stream that is still receiving messages. Each request waits up to 5 seconds. The scan runs on one thread and returns rows in chunks of 2,048. DuckDB's progress bar follows the scan through its range of sequences, and a query can be cancelled between messages.

## Authentication and TLS

Credentials and TLS files are kept in DuckDB secrets of type `nats`:

```sql
CREATE SECRET prod (
    TYPE nats,
    USER 'reporting',
    PASSWORD '...',
    SCOPE 'tls://nats.example.com:4222'
);

SELECT count(*) FROM nats_scan('telemetry', url := 'tls://nats.example.com:4222');
```

A query uses the secret named by its `secret` option. Without that option, it uses the secret with the longest `SCOPE` that `url` starts with, where the scope ends at the end of a scheme, host or port: `tls://nats.example.com:4222` is for `tls://nats.example.com:4222`, and not for `tls://nats.example.com:42220`. A URL without a scheme counts as `nats://`. A secret without a `SCOPE` applies to every URL that no other secret's scope matches. A query that finds no secret connects without credentials. DuckDB's `which_secret()` matches any start of a URL, so it can name a different secret than the one `nats_scan` uses.

A secret holds at most one way to sign in:

| Options | Sign-in |
|---------|---------|
| `USER`, `PASSWORD` | User and password, both given |
| `TOKEN` | Token |
| `CREDS` | Path to a credentials file, holding a user JWT and NKey seed, as for decentralized auth |
| `NKEY` | Path to a file holding a user NKey seed |

For TLS, use a `tls://` URL, or give one of these options:

| Option | Contents |
|--------|----------|
| `TLS_CA` | Path to the certificates that sign the server's certificate, when they are not in the system's store |
| `TLS_CERT`, `TLS_KEY` | Paths to a client certificate and its key, for servers that verify clients. The key cannot be encrypted. |

`duckdb_secrets()` redacts passwords and tokens. The files are read through DuckDB's file system when a query starts, so `~` is the home directory and `disabled_filesystems` applies. Add `PERSISTENT` to `CREATE SECRET` to keep the secret across sessions; DuckDB stores persistent secrets unencrypted in `~/.duckdb/stored_secrets`.

## Security

With `SET enable_external_access = false`, `nats_scan` refuses to run, since it opens network connections and reads `.proto` files. That includes statements prepared before the setting changed.

## API Reference

```
nats_scan(stream, [subject, url, secret, start_seq, end_seq, start_time, end_time,
                   json_extract, proto_file, proto_message, proto_extract])
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `stream` | VARCHAR | required | Stream name (positional) |
| `url` | VARCHAR | `nats://localhost:4222` | NATS server URL |
| `secret` | VARCHAR | the secret that matches `url` | Name of a `nats` secret |
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

- `json_extract` and `proto_extract` cannot be combined.
- `proto_extract` requires `proto_file` and `proto_message`.
- Options cannot be NULL, and field lists cannot contain NULL or empty names.

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

`make test_integration` runs `test/sql/integration` through `test/fixtures/serve.sh`. That script serves two copies of the JetStream store in `test/fixtures/jetstream` with `nats-server`: one without auth on port 42222, and one on port 42223 that wants TLS and a user (`test/fixtures/auth`). It needs `nats-server` on the `PATH`, or `NATS_SERVER=/path/to/nats-server`. The store holds synthetic messages with fixed sequences and timestamps; `test/fixtures/make-jetstream-store.sh` regenerates it, using nats-server, the nats CLI and protoc.

On Linux x86-64 and macOS, CI runs every test against nats-server 2.10 and 2.15, which `serve.sh` downloads and checks against pinned SHA-256 sums.

### Demo Data

```bash
pip install -r requirements.txt
make generate-data
```

This starts nats-server with Docker Compose, creates the demo streams, and publishes an hour of synthetic JSON readings to `telemetry` and `environmental` and 500 protobuf readings to `telemetry_proto`. `python3 scripts/generate-demo-screenshots.py` renders the README images from queries on that data.

## License

MIT License. See [LICENSE](../LICENSE).
