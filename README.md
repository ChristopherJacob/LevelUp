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

4. Home Assistant dashboard (MQTT)

This dashboard replicates the main on-device experience using the MQTT entities produced by the firmware.
It assumes MQTT discovery is enabled (Home Assistant will create the sensors automatically).

Step A: copy the images into Home Assistant

Copy the included SVGs to your HA `www` folder so they are served at `/local/levelup/`:

```
images/ha/levelup_header.svg  ->  /config/www/levelup/levelup_header.svg
images/ha/van_top.svg         ->  /config/www/levelup/van_top.svg
```

Step B: (optional) create a "van is level" binary sensor

Add a template sensor so the dashboard can show a simple "LEVEL" badge.
Adjust the tolerance as desired.

```yaml
template:
  - binary_sensor:
      - name: "LevelUp Van Level"
        state: >
          {{ (states('sensor.levelup_roll')|float(0))|abs <= 1.5
             and (states('sensor.levelup_pitch')|float(0))|abs <= 1.5 }}
  - sensor:
      - name: "LevelUp Corner FL (in)"
        unit_of_measurement: "in"
        state: >
          {% set r = states('sensor.levelup_roll_in')|float(0) %}
          {% set p = states('sensor.levelup_pitch_in')|float(0) %}
          {{ (-p/2 - r/2) | round(1) }}
      - name: "LevelUp Corner FR (in)"
        unit_of_measurement: "in"
        state: >
          {% set r = states('sensor.levelup_roll_in')|float(0) %}
          {% set p = states('sensor.levelup_pitch_in')|float(0) %}
          {{ (-p/2 + r/2) | round(1) }}
      - name: "LevelUp Corner RL (in)"
        unit_of_measurement: "in"
        state: >
          {% set r = states('sensor.levelup_roll_in')|float(0) %}
          {% set p = states('sensor.levelup_pitch_in')|float(0) %}
          {{ (p/2 - r/2) | round(1) }}
      - name: "LevelUp Corner RR (in)"
        unit_of_measurement: "in"
        state: >
          {% set r = states('sensor.levelup_roll_in')|float(0) %}
          {% set p = states('sensor.levelup_pitch_in')|float(0) %}
          {{ (p/2 + r/2) | round(1) }}
```

Step C: Lovelace dashboard YAML (copy into a new dashboard)

Note: entity IDs may differ depending on discovery; update them if needed.

```yaml
views:
  - title: LevelUp
    path: levelup
    icon: mdi:car-brake-fluid-level
    type: sections
    max_columns: 2
    sections:
      - type: grid
        cards:
          - type: picture-elements
            image: /local/levelup/van_top.svg
            elements:
              - type: state-label
                entity: sensor.levelup_corner_rl_in
                style:
                  top: 78%
                  left: 20%
                  font-size: 18px
              - type: state-label
                entity: sensor.levelup_corner_fr_in
                style:
                  top: 22%
                  left: 80%
                  font-size: 18px
              - type: state-label
                entity: sensor.levelup_corner_fl_in
                style:
                  top: 22%
                  left: 20%
                  font-size: 18px
              - type: state-label
                entity: sensor.levelup_corner_rr_in
                style:
                  top: 78%
                  left: 80%
                  font-size: 18px
              - type: conditional
                conditions:
                  - entity: binary_sensor.levelup_van_level
                    state: "on"
                elements:
                  - type: state-label
                    entity: binary_sensor.levelup_van_level
                    prefix: "LEVEL"
                    style:
                      top: 50%
                      left: 50%
                      font-size: 24px
                      font-weight: 700
                      color: "#1f6feb"
      - type: grid
        cards:
          - type: picture
            image: /local/levelup/levelup_header.svg
            tap_action:
              action: none
            hold_action:
              action: none
          - type: gauge
            entity: sensor.levelup_pitch
            name: Pitch
            unit: "°"
            min: -30
            max: 30
            segments:
              - from: -60
                color: "#d62b37"
              - from: -3
                color: "#fabe30"
              - from: -1
                color: "cadetblue"
              - from: 3
                color: "#fabe30"
              - from: 10
                color: "#d62b37"
          - type: gauge
            entity: sensor.levelup_roll
            name: Roll
            unit: "°"
            min: -30
            max: 30
            segments:
              - from: -60
                color: "#d62b37"
              - from: -6
                color: "#fabe30"
              - from: -1
                color: "cadetblue"
              - from: 1
                color: "#fabe30"
              - from: 6
                color: "#d62b37"
          - type: entities
            title: Diagnostics
            entities:
              - entity: sensor.levelup_roll_in
                name: Roll (in)
              - entity: sensor.levelup_pitch_in
                name: Pitch (in)
              - entity: sensor.levelup_rssi
                name: Wi-Fi RSSI
```

If you prefer to use manual MQTT sensors instead of discovery, define them like:

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

License

This project is dedicated to the public domain under the CC0 1.0 Universal license. See the `LICENSE` file for the full text.

Repository license: CC0-1.0 (public domain)
