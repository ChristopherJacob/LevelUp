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
