# MQTT Discovery Details

LevelUp publishes Home Assistant MQTT auto-discovery payloads automatically.

## Published Topics

| Topic | Purpose |
|-------|---------|
| `levelup/<device_id>/state` | Roll, pitch, RSSI, accel data (JSON) |
| `levelup/<device_id>/availability` | `online` / `offline` |

## State Payload

```json
{
  "roll_deg": 1.234,
  "pitch_deg": -0.456,
  "roll_in": 0.5,
  "pitch_in": -0.2,
  "rssi": -62,
  "accel_x": 0.01,
  "accel_y": 0.02,
  "accel_z": 9.81,
  "ip": "192.168.1.50",
  "mode": "STA"
}
```

## Discovery Payloads

The device publishes to `homeassistant/sensor/levelup_<device_id>/<metric>/config`
for each metric. Example (roll):

```json
{
  "name": "LevelUp Roll",
  "state_topic": "levelup/<device_id>/state",
  "value_template": "{{ value_json.roll_deg }}",
  "unit_of_measurement": "°",
  "availability_topic": "levelup/<device_id>/availability",
  "payload_available": "online",
  "payload_not_available": "offline",
  "device": {
    "identifiers": ["levelup_<device_id>"],
    "name": "LevelUp",
    "manufacturer": "LevelUp",
    "model": "ESP32"
  }
}
```

## Configuration

Defaults (configurable via `menuconfig`):

- **Broker URI:** `mqtt://homeassistant.local:1883`
- **Topic prefix:** `levelup`
- **Discovery prefix:** `homeassistant`
- **Publish rate:** 5 Hz

## Requirements

- Home Assistant with MQTT integration configured
- MQTT discovery enabled (default in HA)
- Device on same network as MQTT broker
