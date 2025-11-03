#!/usr/bin/env python3
"""Check NATS JetStream stream info."""

from nats.aio.client import Client as NATS
import asyncio


async def check_stream():
    nc = NATS()
    await nc.connect("nats://localhost:4222")
    js = nc.jetstream()
    
    stream_info = await js.stream_info("telemetry")
    
    print(f"Stream: {stream_info.config.name}")
    print(f"Messages: {stream_info.state.messages}")
    print(f"First Sequence: {stream_info.state.first_seq}")
    print(f"Last Sequence: {stream_info.state.last_seq}")
    print(f"Subjects: {stream_info.config.subjects}")
    
    await nc.close()


if __name__ == "__main__":
    asyncio.run(check_stream())

