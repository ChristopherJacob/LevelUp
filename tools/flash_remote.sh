#!/bin/bash
# flash_remote.sh - Build artifacts are on bananabrain; flash via SSH to CJ-MBA
# where the device is physically connected.

set -e

REMOTE="cj-mba"
REMOTE_TMP="/tmp/levelup_flash"
PORT="/dev/tty.usbmodem11201"
BUILD_DIR="$(cd "$(dirname "$0")/.." && pwd)/build"

echo "==> Copying binaries to $REMOTE..."
ssh "$REMOTE" "mkdir -p $REMOTE_TMP"
scp "$BUILD_DIR/bootloader/bootloader.bin" \
    "$BUILD_DIR/partition_table/partition-table.bin" \
    "$BUILD_DIR/ota_data_initial.bin" \
    "$BUILD_DIR/leveler_min.bin" \
    "$REMOTE:$REMOTE_TMP/"

echo "==> Flashing via $REMOTE on $PORT..."
ssh "$REMOTE" "python3 -m esptool \
    --chip esp32s3 \
    -p $PORT \
    -b 460800 \
    --before default_reset \
    --after hard_reset \
    write_flash \
    --flash_mode dio \
    --flash_freq 80m \
    --flash_size 16MB \
    0x0      $REMOTE_TMP/bootloader.bin \
    0x8000   $REMOTE_TMP/partition-table.bin \
    0x10000  $REMOTE_TMP/ota_data_initial.bin \
    0x20000  $REMOTE_TMP/leveler_min.bin"

echo "==> Flash complete."
