# MQTT Discovery Details

LevelUp publishes Home Assistant MQTT auto-discovery payloads automatically.

## Published Topics

| Topic | Purpose |
|-------|---------|
| `levelup/<device_id>/state` | Roll, pitch, RSSI, accel, and leveling guidance data (JSON) |
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
  "mode": "STA",
  "lift_fl": 0.0,
  "lift_fr": 1.5,
  "lift_rl": 0.0,
  "lift_rr": 1.5,
  "is_level": false,
  "ramp_axis": "roll",
  "ramp_target": 0.0,
  "ramp_remaining": 1.5
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

## Auto-Discovered Entities

### Sensors (unit: `in`)

These four sensors are published via MQTT discovery and appear automatically in Home Assistant:

| Entity Name | State Key | Description |
|-------------|-----------|-------------|
| `LevelUp Lift Front-Left` | `lift_fl` | Inches to raise the front-left wheel (blocks mode) |
| `LevelUp Lift Front-Right` | `lift_fr` | Inches to raise the front-right wheel (blocks mode) |
| `LevelUp Lift Rear-Left` | `lift_rl` | Inches to raise the rear-left wheel (blocks mode) |
| `LevelUp Lift Rear-Right` | `lift_rr` | Inches to raise the rear-right wheel (blocks mode) |

### Binary Sensors

| Entity Name | State Key | Description |
|-------------|-----------|-------------|
| `LevelUp Is Level` | `is_level` | `true` when all corners are within the level tolerance |

### Buttons

| Entity Name | Icon | Description |
|-------------|------|-------------|
| `LevelUp Set Zero` | `mdi:crosshairs-gps` | Sets the current orientation as the level reference (same as on-device hold-to-zero) |

Pressing the **LevelUp Set Zero** button publishes the payload `zero` to the device's inbound
command topic:

```
<topic_prefix>/<device_id>/cmd
```

This topic is the extensible inbound command path — currently only the `zero` payload is handled.

## Advanced / Template-Only Fields

The following fields are present in the MQTT state JSON but do **not** have an auto-discovery
entity in v1. They are available for use in manually configured Home Assistant
[template sensors](https://www.home-assistant.io/integrations/template/):

| State Key | Description |
|-----------|-------------|
| `ramp_axis` | Dominant ramp axis: `"roll"` or `"pitch"` |
| `ramp_target` | Target tilt angle for the ramp axis (degrees) |
| `ramp_remaining` | Remaining inches to travel on the ramp to reach level |

Example template sensor for `ramp_axis`:

```yaml
template:
  - sensor:
      - name: LevelUp Ramp Axis
        state: "{{ states.sensor.levelup_state.attributes.ramp_axis | default('unknown') }}"
```

Or using an MQTT template sensor directly:

```yaml
mqtt:
  sensor:
    - name: LevelUp Ramp Axis
      state_topic: "levelup/<device_id>/state"
      value_template: "{{ value_json.ramp_axis }}"
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
