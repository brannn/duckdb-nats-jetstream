#!/usr/bin/env python3
"""
Generate protobuf telemetry messages and publish to NATS JetStream.
This creates test data for the protobuf extension functionality.
"""

import sys
import time
import random
from datetime import datetime, timedelta

# Add test/proto to path for imports
sys.path.insert(0, 'test/proto')

import telemetry_pb2
from nats.aio.client import Client as NATS
from nats.js import JetStreamContext
import asyncio


async def generate_and_publish():
    """Generate protobuf messages and publish to NATS JetStream."""
    
    # Connect to NATS
    nc = NATS()
    await nc.connect("nats://localhost:4222")
    js = nc.jetstream()
    
    print("Connected to NATS JetStream")
    
    # Device configurations
    devices = [
        {"id": "pm5560-001", "zone": "dc1", "rack": "A1", "building": "North"},
        {"id": "pm5560-002", "zone": "dc1", "rack": "A2", "building": "North"},
        {"id": "pm5560-003", "zone": "dc2", "rack": "B1", "building": "South"},
        {"id": "pm5560-004", "zone": "dc2", "rack": "B2", "building": "South"},
        {"id": "pm5560-005", "zone": "dc3", "rack": "C1", "building": "East"},
    ]
    
    firmware_versions = ["v2.1.0", "v2.1.1", "v2.2.0"]
    
    # Generate messages
    base_time = datetime.now() - timedelta(hours=1)
    message_count = 0
    
    for i in range(100):
        for device in devices:
            # Create telemetry message
            msg = telemetry_pb2.Telemetry()
            
            # Set basic fields
            msg.device_id = device["id"]
            msg.timestamp = int((base_time + timedelta(seconds=i*10)).timestamp() * 1000)
            msg.online = random.choice([True, True, True, False])  # 75% online
            msg.firmware_version = random.choice(firmware_versions)
            
            # Set location (nested message)
            msg.location.zone = device["zone"]
            msg.location.rack = device["rack"]
            msg.location.building = device["building"]
            
            # Set metrics (nested message) with realistic power values
            base_kw = 5.0 + random.uniform(-0.5, 0.5)
            msg.metrics.kw = round(base_kw, 3)
            msg.metrics.pf = round(random.uniform(0.85, 0.95), 3)
            msg.metrics.kva = round(base_kw / msg.metrics.pf, 3)
            msg.metrics.voltage = round(480.0 + random.uniform(-5, 5), 2)
            msg.metrics.current = round(msg.metrics.kva * 1000 / (msg.metrics.voltage * 1.732), 2)
            msg.metrics.frequency = round(60.0 + random.uniform(-0.1, 0.1), 2)
            
            # Serialize to binary
            binary_data = msg.SerializeToString()
            
            # Publish to NATS
            subject = f"telemetry.{device['zone']}.power.pm5560.{device['id']}"
            await js.publish(subject, binary_data)
            
            message_count += 1
            
            if message_count % 50 == 0:
                print(f"Published {message_count} messages...")
    
    print(f"\n✓ Published {message_count} protobuf messages to JetStream")
    print(f"  Stream: telemetry")
    print(f"  Subjects: telemetry.*.power.pm5560.*")
    print(f"  Format: Protocol Buffers (binary)")
    print(f"  Schema: test/proto/telemetry.proto")
    
    # Print example message for verification
    print("\nExample message structure:")
    example = telemetry_pb2.Telemetry()
    example.device_id = "pm5560-001"
    example.timestamp = int(datetime.now().timestamp() * 1000)
    example.online = True
    example.firmware_version = "v2.1.0"
    example.location.zone = "dc1"
    example.location.rack = "A1"
    example.location.building = "North"
    example.metrics.kw = 5.234
    example.metrics.pf = 0.92
    example.metrics.kva = 5.689
    example.metrics.voltage = 480.5
    example.metrics.current = 7.89
    example.metrics.frequency = 60.02
    
    print(example)
    
    await nc.close()


if __name__ == "__main__":
    asyncio.run(generate_and_publish())

