#!/usr/bin/env python3
"""Verify we can read and decode a protobuf message from NATS."""

import sys
sys.path.insert(0, 'test/proto')

import telemetry_pb2
from nats.aio.client import Client as NATS
import asyncio


async def verify_message():
    nc = NATS()
    await nc.connect("nats://localhost:4222")
    js = nc.jetstream()
    
    # Get a recent message (should be protobuf)
    # The last 500 messages are protobuf
    msg = await js.get_msg("telemetry", seq=805)
    
    print(f"Message sequence: {msg.seq}")
    print(f"Subject: {msg.subject}")
    print(f"Data length: {len(msg.data)} bytes")
    print(f"Data (first 50 bytes hex): {msg.data[:50].hex()}")
    
    # Try to decode as protobuf
    try:
        telemetry = telemetry_pb2.Telemetry()
        telemetry.ParseFromString(msg.data)
        
        print("\n✓ Successfully decoded protobuf message:")
        print(f"  device_id: {telemetry.device_id}")
        print(f"  timestamp: {telemetry.timestamp}")
        print(f"  online: {telemetry.online}")
        print(f"  firmware_version: {telemetry.firmware_version}")
        print(f"  location.zone: {telemetry.location.zone}")
        print(f"  location.rack: {telemetry.location.rack}")
        print(f"  location.building: {telemetry.location.building}")
        print(f"  metrics.kw: {telemetry.metrics.kw}")
        print(f"  metrics.voltage: {telemetry.metrics.voltage}")
        print(f"  metrics.current: {telemetry.metrics.current}")
        
    except Exception as e:
        print(f"\n✗ Failed to decode: {e}")
    
    await nc.close()


if __name__ == "__main__":
    asyncio.run(verify_message())

