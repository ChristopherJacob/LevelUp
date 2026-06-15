# Manual MQTT Sensor Setup

If MQTT auto-discovery is disabled in Home Assistant, define sensors manually.

Add to `configuration.yaml`:

```yaml
mqtt:
  sensor:
    - name: "LevelUp Roll"
      state_topic: "levelup/<device_id>/state"
      value_template: "{{ value_json.roll_deg }}"
      unit_of_measurement: "°"
    - name: "LevelUp Pitch"
      state_topic: "levelup/<device_id>/state"
      value_template: "{{ value_json.pitch_deg }}"
      unit_of_measurement: "°"
    - name: "LevelUp Roll (in)"
      state_topic: "levelup/<device_id>/state"
      value_template: "{{ value_json.roll_in }}"
      unit_of_measurement: "in"
    - name: "LevelUp Pitch (in)"
      state_topic: "levelup/<device_id>/state"
      value_template: "{{ value_json.pitch_in }}"
      unit_of_measurement: "in"
    - name: "LevelUp Accel X"
      state_topic: "levelup/<device_id>/state"
      value_template: "{{ value_json.accel_x }}"
    - name: "LevelUp Accel Y"
      state_topic: "levelup/<device_id>/state"
      value_template: "{{ value_json.accel_y }}"
    - name: "LevelUp Accel Z"
      state_topic: "levelup/<device_id>/state"
      value_template: "{{ value_json.accel_z }}"
```

Replace `<device_id>` with your device's ID (visible in the web dashboard or serial console).
