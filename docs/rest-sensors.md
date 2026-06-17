# REST Sensor Setup (Alternative)

If you prefer polling over MQTT, the device exposes a JSON status endpoint.

Add these sensors to your Home Assistant `configuration.yaml`:

```yaml
rest:
  - resource: http://<device_ip>/status.json
    scan_interval: 10
    sensor:
      - name: LevelUp Roll
        value_template: "{{ value_json.roll_deg }}"
        unit_of_measurement: "°"
      - name: LevelUp Pitch
        value_template: "{{ value_json.pitch_deg }}"
        unit_of_measurement: "°"
      - name: LevelUp RSSI
        value_template: "{{ value_json.rssi }}"
        unit_of_measurement: "dBm"
```

Recommended polling interval: 5–10 seconds. Shorter intervals increase Wi-Fi traffic and power use.
For always-on dashboards, 5 seconds is a good compromise.

## Leveling Guidance Fields

The `/status.json` response includes additional fields for leveling guidance when the IMU is
calibrated and a leveling mode is active.

### Field Reference

| Field | Type | Meaning |
|-------|------|---------|
| `lvl_mode` | string | Active leveling mode: `"blocks"` or `"ramps"` |
| `guidance_available` | bool | `true` when calibration is complete and guidance can be computed |
| `is_level` | bool | `true` when all corners are within the level tolerance |
| `lift_fl` | number | Inches to raise the front-left wheel (blocks mode) |
| `lift_fr` | number | Inches to raise the front-right wheel (blocks mode) |
| `lift_rl` | number | Inches to raise the rear-left wheel (blocks mode) |
| `lift_rr` | number | Inches to raise the rear-right wheel (blocks mode) |
| `worst_corner` | int | Index of the corner furthest from level: 0=FL, 1=FR, 2=RL, 3=RR |
| `ramp_axis_is_roll` | bool | `true` if the primary off-level axis is roll (side-to-side) |
| `ramp_lift_left` | bool | `true` if the left side needs to be raised (ramps mode) |
| `ramp_lift_front` | bool | `true` if the front needs to be raised (ramps mode) |
| `ramp_remaining_in` | number | Inches still needed to reach level on the ramp axis (ramps mode countdown) |

**Blocks mode** uses the four `lift_fl`/`lift_fr`/`lift_rl`/`lift_rr` values to tell you how
many inches of block stack to place under each wheel.

**Ramps mode** uses `ramp_axis_is_roll`, `ramp_lift_left`, `ramp_lift_front`, and
`ramp_remaining_in` to guide you as you drive up or down a leveling ramp — the remaining-inches
value counts down toward zero as the vehicle reaches level.

### Sample Response (with leveling guidance)

```json
{
  "roll_deg": 2.1,
  "pitch_deg": -0.8,
  "roll_in": 0.9,
  "pitch_in": -0.3,
  "rssi": -62,
  "accel_x": 0.01,
  "accel_y": 0.02,
  "accel_z": 9.81,
  "ip": "192.168.1.50",
  "mode": "STA",
  "lvl_mode": "blocks",
  "guidance_available": true,
  "is_level": false,
  "lift_fl": 0.0,
  "lift_fr": 1.5,
  "lift_rl": 0.0,
  "lift_rr": 1.5,
  "worst_corner": 1,
  "ramp_axis_is_roll": true,
  "ramp_lift_left": false,
  "ramp_lift_front": false,
  "ramp_remaining_in": 1.5
}
```

### Example Home Assistant REST Sensors (leveling guidance)

```yaml
rest:
  - resource: http://<device_ip>/status.json
    scan_interval: 5
    sensor:
      - name: LevelUp Lift Front-Left
        value_template: "{{ value_json.lift_fl }}"
        unit_of_measurement: "in"
      - name: LevelUp Lift Front-Right
        value_template: "{{ value_json.lift_fr }}"
        unit_of_measurement: "in"
      - name: LevelUp Lift Rear-Left
        value_template: "{{ value_json.lift_rl }}"
        unit_of_measurement: "in"
      - name: LevelUp Lift Rear-Right
        value_template: "{{ value_json.lift_rr }}"
        unit_of_measurement: "in"
      - name: LevelUp Ramp Remaining
        value_template: "{{ value_json.ramp_remaining_in }}"
        unit_of_measurement: "in"
    binary_sensor:
      - name: LevelUp Is Level
        value_template: "{{ value_json.is_level }}"
```
