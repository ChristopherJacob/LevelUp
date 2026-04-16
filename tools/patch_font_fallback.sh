#!/usr/bin/env sh
set -eu

FONT_FILE="${1:-main/ds_digit_36.c}"

if [ ! -f "$FONT_FILE" ]; then
  echo "Font file not found: $FONT_FILE" >&2
  exit 1
fi

# Ensure the fallback points to LV_FONT_DEFAULT for LVGL >= 8.2 builds.
# This keeps the degree symbol (and other missing glyphs) rendering even
# if the digit font doesn't include them.
python3 - "$FONT_FILE" <<'PY'
import sys
from pathlib import Path

path = Path(sys.argv[1])
text = path.read_text()
needle = ".fallback = NULL,"
replacement = ".fallback = LV_FONT_DEFAULT,"
if needle in text:
    text = text.replace(needle, replacement, 1)
    path.write_text(text)
    print(f"Patched fallback in {path}")
else:
    print(f"No fallback patch needed in {path}")
PY
