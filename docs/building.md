# Building LevelUp from Source

This guide covers compiling and flashing the LevelUp firmware to an ESP32 device.

## Prerequisites

1. Install ESP-IDF and toolchain (see https://docs.espressif.com)
2. Clone this repository

## Build & Flash

```bash
cd /path/to/LevelUp
. $HOME/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/ttyUSB0 flash
```

## Configuration

Use `idf.py menuconfig` to set:

- **WiFi credentials** — or configure on-device via the captive portal
- **MQTT broker URI** — default: `mqtt://homeassistant.local:1883`
- **MQTT topic prefix** — default: `levelup`
- **MQTT discovery prefix** — default: `homeassistant`
- **Publish rate** — default: 5 Hz (LevelUp MQTT → Publish rate)

## OTA Updates

The firmware supports OTA updates. Flash the initial image via USB, then subsequent
updates can be pushed over WiFi through the web dashboard.
