# Dashboard Setup

This guide adds a LevelUp Lovelace dashboard to Home Assistant.

## Step 1: Copy Images

Copy the included images to your HA `www` folder:

```
images/ha/levelup_header.svg  →  /config/www/levelup/levelup_header.svg
images/ha/van_top.png         →  /config/www/levelup/van_top.png
```

## Step 2: Add Template Sensors (Optional)

Creates per-corner inch values and a "van is level" binary sensor.
Add to `configuration.yaml`:

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

## Step 3: Lovelace Dashboard

Create a new dashboard in Home Assistant and paste this YAML.
Entity IDs may differ depending on discovery — update as needed.

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
            image: /local/levelup/van_top.png
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
