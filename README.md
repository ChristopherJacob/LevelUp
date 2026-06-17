# LevelUp

A van leveling sensor for Home Assistant. Measures roll and pitch with a built-in IMU,
displays real-time angles on an LCD screen, and streams data to Home Assistant over MQTT
with automatic entity discovery.

Designed for camper vans, RVs, and any vehicle where getting level matters.

## What It Does

- Measures roll and pitch in both **degrees** and **inches**
- Built-in display shows real-time level status
- Publishes to **MQTT** with Home Assistant auto-discovery — entities appear automatically
- Web dashboard at `http://<device_ip>/` for remote viewing and calibration
- Calibration wizard for accurate readings on uneven ground
- OTA firmware updates — no USB cable needed after initial flash

## Installation

### 1. Flash the Device

Flash the firmware to an ESP32. If you received a pre-flashed device, skip to step 2.

See [docs/building.md](docs/building.md) for ESP-IDF build instructions.

### 2. Connect to WiFi

On first boot, LevelUp creates a WiFi access point. Connect to it and configure your
van's network credentials through the captive portal or web dashboard.

To switch networks later, use the **Reconfigure Wi-Fi** button on the `/status` page —
it drops the device back into setup AP mode while preserving all other settings.

### 3. Point It at Your MQTT Broker

Set your MQTT broker address in the device settings. Default: `mqtt://homeassistant.local:1883`

For Home Assistant OS, the MQTT broker is typically at `<ha_ip>:1883` (Mosquitto add-on)
or the internal broker.

## Home Assistant Setup

### Auto-Discovery (Recommended)

If your Home Assistant has the **MQTT integration** configured and **discovery enabled**
(both are defaults), LevelUp sensors appear automatically. No YAML needed.

Look for:
- `sensor.levelup_roll` / `sensor.levelup_pitch` — degrees
- `sensor.levelup_roll_in` / `sensor.levelup_pitch_in` — inches
- `sensor.levelup_rssi` — WiFi signal strength

### Manual Setup

If auto-discovery is disabled, add sensors manually:
- [Manual MQTT config](docs/mqtt-manual.md)
- [REST polling (alternative)](docs/rest-sensors.md)

## Dashboard

A pre-built Lovelace dashboard with gauges, corner indicators, and a "LEVEL" badge
is included. See [docs/dashboard.md](docs/dashboard.md) for setup.

## Technical Docs

- [Building from source](docs/building.md)
- [MQTT discovery details](docs/mqtt-discovery.md)
- [Manual MQTT setup](docs/mqtt-manual.md)
- [REST sensor alternative](docs/rest-sensors.md)
- [Dashboard setup](docs/dashboard.md)

## License

CC0 1.0 Universal (public domain). See [LICENSE](LICENSE).
