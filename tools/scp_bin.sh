#!/bin/bash
# scp_bin.sh - Copy the built firmware binary to CJ-MBA with the git hash in the filename.
# Usage: bash tools/scp_bin.sh

set -e

REMOTE="cj-mba"
BUILD_DIR="$(cd "$(dirname "$0")/.." && pwd)/build"
HASH=$(git -C "$(dirname "$0")/.." rev-parse --short HEAD)
DEST="$HOME/Downloads/leveler_min_${HASH}.bin"
REMOTE_DEST="~/Downloads/leveler_min_${HASH}.bin"

if [ ! -f "$BUILD_DIR/leveler_min.bin" ]; then
    echo "ERROR: $BUILD_DIR/leveler_min.bin not found. Run idf.py build first." >&2
    exit 1
fi

echo "==> Copying build/leveler_min.bin -> $REMOTE:$REMOTE_DEST"
scp "$BUILD_DIR/leveler_min.bin" "$REMOTE:$REMOTE_DEST"
echo "==> Done: leveler_min_${HASH}.bin is in ~/Downloads on $REMOTE"
