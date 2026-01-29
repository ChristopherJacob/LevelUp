# LevelUp

This repository contains the LevelUp firmware (derived from `leveler_min`) for an ESP32-based device. It includes the ESP-IDF project sources, components, and configuration used to build the firmware.

Quick start

1. Install ESP-IDF and toolchain (see https://docs.espressif.com).
2. From the project root run:

```bash
cd /path/to/LevelUp
. $HOME/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/ttyUSB0 flash
```

Home Assistant integration (Wi-Fi)

The firmware already exposes a JSON status endpoint at `http://<device_ip>/status.json` that
includes `roll_deg`, `pitch_deg`, Wi-Fi RSSI, and connection metadata. Use the steps below
to integrate with Home Assistant.

1. REST sensors (polling)

Add sensors that read the JSON payload directly. Example configuration (YAML):

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

Recommended polling interval: 5–10 seconds. Shorter intervals increase Wi-Fi traffic and
power use. If you plan to display an always-on dashboard, 5 seconds is a good compromise.

2. MQTT topics & payload schema (implemented)

If you want push updates and Home Assistant auto-discovery, MQTT is supported in firmware.
Configure the broker and topics via `menuconfig` (defaults are shown below). Suggested
topic structure:

```
levelup/<device_id>/state
levelup/<device_id>/availability
```

Defaults (configurable via `menuconfig`):

- Broker URI: `mqtt://homeassistant.local`
- Topic prefix: `levelup`
- Discovery prefix: `homeassistant`

Suggested payload for `state` (JSON):

```json
{
  "roll_deg": 1.234,
  "pitch_deg": -0.456,
  "rssi": -62,
  "ip": "192.168.1.50",
  "mode": "STA"
}
```

Suggested payload for `availability`:

```json
"online"
```

Recommended publish rate: 5 Hz for responsive dashboards, or 1 Hz for low-traffic monitoring.
Use `menuconfig` to change `LevelUp MQTT → Publish rate (Hz)`.

3. Home Assistant MQTT discovery (example)

Configure auto-discovery payloads so Home Assistant creates entities automatically:

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

Repeat the discovery payload for `pitch_deg` and `rssi` by changing the `name` and
`value_template`. For the discovery topic, publish to:

```
homeassistant/sensor/levelup_<device_id>/roll/config
```

License

This project is dedicated to the public domain under the CC0 1.0 Universal license. See the `LICENSE` file for the full text.

Repository license: CC0-1.0 (public domain)
