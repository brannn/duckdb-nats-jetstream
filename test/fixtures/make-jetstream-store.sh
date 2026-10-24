#!/usr/bin/env bash
# Regenerates test/fixtures/jetstream, the JetStream store that `make test_integration` serves.
# Needs nats-server (or $NATS_SERVER), the nats CLI and protoc. All data is synthetic.
set -euo pipefail

cd "$(dirname "$0")/../.."
PORT=${NATS_JS_FIXTURE_PORT:-42223}
URL=nats://127.0.0.1:$PORT
WORK=$(mktemp -d)

"${NATS_SERVER:-nats-server}" -js -a 127.0.0.1 -p "$PORT" -sd "$WORK/store" > "$WORK/server.log" 2>&1 &
SERVER=$!
trap 'kill $SERVER 2>/dev/null || true; rm -rf "$WORK"' EXIT
until grep -q "Server is ready" "$WORK/server.log"; do sleep 0.1; done

n() { nats -s "$URL" "$@"; }
pub() { n pub --no-templates "$@" > /dev/null 2>&1; }
# Publishes a protobuf message given in text format: pub_proto <subject> <file.proto> <type> <text>
pub_proto() {
    echo "$4" | protoc --encode="$3" -I test/proto "test/proto/$2" > "$WORK/msg.bin"
    n pub --force-stdin "$1" < "$WORK/msg.bin" > /dev/null 2>&1
}

# events: JSON and non-JSON payloads on subjects that share prefixes
n stream add events --subjects 'events.>' --storage file --allow-direct --defaults > /dev/null
pub events.site1.power '{"device_id":"pdu-1","kw":42,"big":9007199254740993,"tiny":1e-7,"location":{"zone":"z1","rack":"r7"},"tags":["a","b"],"note":"he said \"hi\"","name":"naïve","status":null,"ok":true}'
pub events.site10.power '{"device_id":"pdu-2","kw":1.5,"location":{"zone":"z2"}}'
pub events.site1.cooling '{"device_id":"crac-1","kw":-3,"subject":"from payload"}'
pub events.site1.power 'not json'
pub events.site2.power '[{"id":1},{"id":2}]'
printf '\xff\xfe\x00\x01' | n pub --force-stdin events.site2.power > /dev/null 2>&1
pub events.site2.power ''
pub events.site1.power '{"a.b":"literal","a":{"b":"nested","c":"only nested"}}'

# gaps: ten messages 200ms apart, with 4-6 deleted
n stream add gaps --subjects 'gaps.>' --storage file --allow-direct --defaults > /dev/null
for i in $(seq 1 10); do
    pub gaps.x "{\"i\":$i}"
    sleep 0.2
done
for seq in 4 5 6; do n stream rmm gaps "$seq" -f > /dev/null; done

# api_only: without allow_direct, read through the JetStream API; 2 is deleted
n stream add api_only --subjects 'api.>' --storage file --no-allow-direct --defaults > /dev/null
pub api.a '{"n":1}'
pub api.b '{"n":2}'
pub api.a '{"n":3}'
pub api.b '{"n":4}'
n stream rmm api_only 2 -f > /dev/null

# empty: no messages
n stream add empty --subjects 'empty.>' --storage file --allow-direct --defaults > /dev/null

# proto: protobuf payloads for test/proto/*.proto, and one that does not parse
n stream add proto --subjects 'proto.>' --storage file --allow-direct --defaults > /dev/null
pub_proto proto.telemetry telemetry.proto telemetry.Telemetry \
    'device_id: "pdu-1" timestamp: 1767225600000 location { zone: "z1" rack: "r7" building: "b1" }
     metrics { kw: 4.25 pf: 0.98 voltage: 230.5 } online: true firmware_version: "1.2.3" tags: "critical" tags: "row-a"'
pub_proto proto.telemetry telemetry.proto telemetry.Telemetry 'device_id: "pdu-2" metrics { kw: 0.5 }'
pub_proto proto.types types_test.proto test.TypesTest \
    'str_field: "héllo" bytes_field: "\x00\x01\xff" int32_field: -2147483648 int64_field: -9223372036854775808
     uint32_field: 4294967295 uint64_field: 18446744073709551615 sint32_field: -5 sint64_field: -6 fixed32_field: 7
     fixed64_field: 8 sfixed32_field: -9 sfixed64_field: -10 float_field: 0.1 double_field: 2.5e-300 bool_field: true
     status_field: ACTIVE'
pub_proto proto.reading reading.proto fixture.Reading \
    'id: "r1" values: [0.1, -2.5, 1e300] parts { name: "a" count: 9007199254740993 } parts { name: "b\"q" }
     counters { key: "x" value: 1 } level: 0 raw: "\x00\xff" main { name: "m" count: 3 } blobs: ["\x01\x02", ""]
     kinds: [KIND_A, KIND_B] names: ["a\"b", "c\\", "ü"] ratios: [0.1, 1.5] bigs: [9223372036854775807, -1]'
pub_proto proto.reading reading.proto fixture.Reading 'id: "r2"'
printf '\xff\xff\xff' | n pub --force-stdin proto.junk > /dev/null 2>&1
pub_proto proto.event event.proto fixture.Event 'at { seconds: 1767225600 nanos: 500000000 } part { name: "p" count: 2 } kind: KIND_B'

add_streams_0_4() {
    # hdrs and hdrs_api: messages with and without headers, read in batches and through the JetStream API
    n stream add hdrs --subjects 'hdrs.>' --storage file --allow-direct --defaults > /dev/null
    n stream add hdrs_api --subjects 'hdrs_api.>' --storage file --no-allow-direct --defaults > /dev/null
    for s in hdrs hdrs_api; do
        pub "$s.a" '{"n":1}' -H 'Nats-Msg-Id:m1' -H 'X-Trace:t1' -H 'X-Trace:t2'
        pub "$s.a" '{"n":2}'
        pub "$s.b" '{"n":3}' -H 'Content-Type:application/json'
    done

    # many: more messages than two batches hold
    n stream add many --subjects 'many.>' --storage file --allow-direct --defaults > /dev/null
    n pub many.x '{"i":{{Count}}}' --count 4100 > /dev/null 2>&1

    # replies: messages with headers the server also puts on its replies: a Status, and from replies_src, which
    # republishes into it, Nats-Stream, Nats-Subject, Nats-Sequence and the rest
    n stream add replies --subjects 'replies.>' --storage file --allow-direct --defaults > /dev/null
    pub replies.own '{"n":1}'
    pub replies.own '' -H 'Status:404'
    pub replies.own '{"n":3}' -H 'Status:204' -H 'Description:No Messages'
    pub replies.own '{"n":4}' -H 'Status:shipped'
    n stream add replies_src --subjects 'replies_src.>' --storage file --allow-direct --defaults \
        --republish-source 'replies_src.>' --republish-destination 'replies.>' > /dev/null
    pub replies_src.a '{"n":5}'
    pub replies_src.b '{"n":6}'
    # an empty message with Status 503, which the client library takes for "no responders" in a request's reply
    pub replies.own '' -H 'Status:503'
}
add_streams_0_4

kill "$SERVER"
wait "$SERVER" 2>/dev/null || true
rm -rf test/fixtures/jetstream
cp -R "$WORK/store/jetstream" test/fixtures/jetstream
# index.db is a cache the server rebuilds from the message blocks
find test/fixtures/jetstream -name index.db -delete
echo "wrote test/fixtures/jetstream"
