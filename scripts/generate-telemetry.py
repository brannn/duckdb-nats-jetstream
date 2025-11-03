#!/usr/bin/env python3
"""
Generate synthetic telemetry data for NATS JetStream testing.

This script generates realistic power monitoring and environmental sensor data
that mimics real-world datacenter telemetry patterns.
"""

import asyncio
import json
import random
import sys
from datetime import datetime, timedelta
from typing import Dict, Any

try:
    from nats.aio.client import Client as NATS
    from nats.js import JetStreamContext
except ImportError:
    print("Error: nats-py library not found. Install with: pip install nats-py")
    sys.exit(1)


class TelemetryGenerator:
    """Generates synthetic telemetry data for testing."""
    
    def __init__(self, nats_url: str = "nats://localhost:4222"):
        self.nats_url = nats_url
        self.nc = None
        self.js = None
        
        # Device configurations
        self.power_meters = [
            {"id": "pm5560-001", "zone": "zone-a", "capacity_kw": 100},
            {"id": "pm5560-002", "zone": "zone-a", "capacity_kw": 100},
            {"id": "pm5560-003", "zone": "zone-b", "capacity_kw": 150},
            {"id": "pm5560-004", "zone": "zone-b", "capacity_kw": 150},
            {"id": "pm5560-005", "zone": "zone-c", "capacity_kw": 200},
        ]
        
        self.temp_sensors = [
            {"id": "temp-001", "zone": "zone-a", "location": "inlet"},
            {"id": "temp-002", "zone": "zone-a", "location": "outlet"},
            {"id": "temp-003", "zone": "zone-b", "location": "inlet"},
            {"id": "temp-004", "zone": "zone-b", "location": "outlet"},
            {"id": "temp-005", "zone": "zone-c", "location": "inlet"},
            {"id": "temp-006", "zone": "zone-c", "location": "outlet"},
        ]
    
    async def connect(self):
        """Connect to NATS server."""
        self.nc = NATS()
        await self.nc.connect(self.nats_url)
        self.js = self.nc.jetstream()
        print(f"Connected to NATS at {self.nats_url}")
    
    async def disconnect(self):
        """Disconnect from NATS server."""
        if self.nc:
            await self.nc.close()
            print("Disconnected from NATS")
    
    def generate_power_reading(self, meter: Dict[str, Any], timestamp: datetime) -> Dict[str, Any]:
        """Generate a realistic power meter reading."""
        # Base load varies by time of day
        hour = timestamp.hour
        if 9 <= hour <= 17:  # Business hours
            base_utilization = 0.7
        elif 18 <= hour <= 22:  # Evening
            base_utilization = 0.5
        else:  # Night
            base_utilization = 0.3
        
        # Add some randomness
        utilization = base_utilization + random.uniform(-0.1, 0.1)
        utilization = max(0.1, min(0.95, utilization))
        
        kw = meter["capacity_kw"] * utilization
        
        # Power factor typically between 0.85 and 0.98
        pf = random.uniform(0.85, 0.98)
        
        # Calculate apparent power
        kva = kw / pf
        
        # Voltage should be around 480V for 3-phase (with small variations)
        voltage = 480 + random.uniform(-5, 5)
        
        # Calculate current (simplified for 3-phase)
        current = (kva * 1000) / (voltage * 1.732)
        
        return {
            "device_id": meter["id"],
            "zone": meter["zone"],
            "timestamp": timestamp.isoformat(),
            "kw": round(kw, 2),
            "pf": round(pf, 3),
            "kva": round(kva, 2),
            "voltage": round(voltage, 1),
            "current": round(current, 1),
            "frequency": round(60.0 + random.uniform(-0.1, 0.1), 2),
        }
    
    def generate_temp_reading(self, sensor: Dict[str, Any], timestamp: datetime) -> Dict[str, Any]:
        """Generate a realistic temperature sensor reading."""
        # Base temperature varies by location
        if sensor["location"] == "inlet":
            base_temp = 18.0  # Cooler inlet air
        else:
            base_temp = 24.0  # Warmer outlet air
        
        # Add time-based variation
        hour = timestamp.hour
        if 9 <= hour <= 17:
            temp_offset = 2.0  # Warmer during business hours
        else:
            temp_offset = 0.0
        
        temp_c = base_temp + temp_offset + random.uniform(-1.5, 1.5)
        
        # Humidity typically 40-60%
        humidity = random.uniform(40, 60)
        
        return {
            "device_id": sensor["id"],
            "zone": sensor["zone"],
            "location": sensor["location"],
            "timestamp": timestamp.isoformat(),
            "temp_c": round(temp_c, 1),
            "temp_f": round(temp_c * 9/5 + 32, 1),
            "humidity": round(humidity, 1),
        }
    
    async def publish_power_reading(self, meter: Dict[str, Any], timestamp: datetime):
        """Publish a power meter reading to JetStream."""
        reading = self.generate_power_reading(meter, timestamp)
        subject = f"telemetry.dc1.power.pm5560.{meter['id']}"
        
        payload = json.dumps(reading).encode()
        await self.js.publish(subject, payload)
    
    async def publish_temp_reading(self, sensor: Dict[str, Any], timestamp: datetime):
        """Publish a temperature sensor reading to JetStream."""
        reading = self.generate_temp_reading(sensor, timestamp)
        subject = f"environmental.dc1.sensors.temp.{sensor['id']}"
        
        payload = json.dumps(reading).encode()
        await self.js.publish(subject, payload)
    
    async def generate_historical_data(self, hours: int = 24, interval_seconds: int = 60):
        """Generate historical data for the specified time period."""
        end_time = datetime.utcnow()
        start_time = end_time - timedelta(hours=hours)
        
        current_time = start_time
        message_count = 0
        
        print(f"Generating {hours} hours of historical data...")
        print(f"Interval: {interval_seconds} seconds")
        print(f"Start: {start_time.isoformat()}")
        print(f"End: {end_time.isoformat()}")
        
        while current_time <= end_time:
            # Publish readings for all power meters
            for meter in self.power_meters:
                await self.publish_power_reading(meter, current_time)
                message_count += 1
            
            # Publish readings for all temperature sensors
            for sensor in self.temp_sensors:
                await self.publish_temp_reading(sensor, current_time)
                message_count += 1
            
            current_time += timedelta(seconds=interval_seconds)
            
            # Progress indicator
            if message_count % 1000 == 0:
                print(f"Published {message_count} messages...")
        
        print(f"Complete! Published {message_count} total messages")
    
    async def generate_realtime_data(self, duration_seconds: int = 60, interval_seconds: int = 5):
        """Generate real-time data for testing live scenarios."""
        print(f"Generating real-time data for {duration_seconds} seconds...")
        print(f"Interval: {interval_seconds} seconds")
        
        end_time = datetime.utcnow() + timedelta(seconds=duration_seconds)
        message_count = 0
        
        while datetime.utcnow() < end_time:
            current_time = datetime.utcnow()
            
            # Publish readings for all devices
            for meter in self.power_meters:
                await self.publish_power_reading(meter, current_time)
                message_count += 1
            
            for sensor in self.temp_sensors:
                await self.publish_temp_reading(sensor, current_time)
                message_count += 1
            
            print(f"Published {message_count} messages at {current_time.isoformat()}")
            
            await asyncio.sleep(interval_seconds)
        
        print(f"Complete! Published {message_count} total messages")


async def main():
    """Main entry point."""
    generator = TelemetryGenerator()
    
    try:
        await generator.connect()
        
        # Generate 1 hour of historical data at 1-minute intervals for initial testing
        print("\n=== Generating Historical Data ===")
        await generator.generate_historical_data(hours=1, interval_seconds=60)
        
        print("\n=== Data Generation Complete ===")
        print("\nYou can now query the data using:")
        print("  nats stream info telemetry")
        print("  nats stream info environmental")
        
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)
    finally:
        await generator.disconnect()


if __name__ == "__main__":
    asyncio.run(main())

