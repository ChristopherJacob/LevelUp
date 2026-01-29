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

License: not specified — please add a `LICENSE` file if desired.
