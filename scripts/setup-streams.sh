#!/usr/bin/env bash

set -e

NATS_URL="nats://localhost:4222"

echo "Setting up JetStream streams..."

# Create telemetry stream for power monitoring data
nats stream add telemetry \
  --subjects "telemetry.>" \
  --storage file \
  --retention limits \
  --max-msgs=-1 \
  --max-bytes=-1 \
  --max-age=7d \
  --max-msg-size=1048576 \
  --discard old \
  --dupe-window=2m \
  --replicas=1 \
  --server="${NATS_URL}" \
  --defaults

echo "Created stream: telemetry"

# Create environmental stream for temperature/humidity data
nats stream add environmental \
  --subjects "environmental.>" \
  --storage file \
  --retention limits \
  --max-msgs=-1 \
  --max-bytes=-1 \
  --max-age=7d \
  --max-msg-size=1048576 \
  --discard old \
  --dupe-window=2m \
  --replicas=1 \
  --server="${NATS_URL}" \
  --defaults

echo "Created stream: environmental"

# Create telemetry_proto stream for protobuf test data
nats stream add telemetry_proto \
  --subjects "telemetry_proto.>" \
  --storage file \
  --retention limits \
  --max-msgs=-1 \
  --max-bytes=-1 \
  --max-age=7d \
  --max-msg-size=1048576 \
  --discard old \
  --dupe-window=2m \
  --replicas=1 \
  --server="${NATS_URL}" \
  --defaults

echo "Created stream: telemetry_proto"

# Create events stream for audit/system events
nats stream add events \
  --subjects "events.>" \
  --storage file \
  --retention limits \
  --max-msgs=-1 \
  --max-bytes=-1 \
  --max-age=30d \
  --max-msg-size=1048576 \
  --discard old \
  --dupe-window=2m \
  --replicas=1 \
  --server="${NATS_URL}" \
  --defaults

echo "Created stream: events"

# Create test consumers
echo "Creating test consumers..."

nats consumer add telemetry etl-power \
  --filter "telemetry.dc1.power.>" \
  --ack explicit \
  --pull \
  --deliver all \
  --max-deliver=-1 \
  --max-pending=1000 \
  --replay instant \
  --server="${NATS_URL}" \
  --defaults

echo "Created consumer: etl-power"

nats consumer add telemetry analytics-consumer \
  --filter "telemetry.>" \
  --ack explicit \
  --pull \
  --deliver all \
  --max-deliver=-1 \
  --max-pending=1000 \
  --replay instant \
  --server="${NATS_URL}" \
  --defaults

echo "Created consumer: analytics-consumer"

echo ""
echo "Stream setup complete!"
echo ""
echo "Streams:"
nats stream list --server="${NATS_URL}"
echo ""
echo "Consumers:"
nats consumer list telemetry --server="${NATS_URL}"

