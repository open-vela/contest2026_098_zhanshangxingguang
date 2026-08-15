#!/bin/bash
# build_and_pack.sh — build firmware and pack with repack.py
#
# Usage:
#   ./board/contest_board/tools/build_and_pack.sh
#
# Prerequisites:
#   - Run from vendor/openvela root
#   - Python 3.x available

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BOARD_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OPENVELA_DIR="$(cd "$BOARD_DIR/../../.." && pwd)"
CONFIG_PATH="vendor/openvela/boards/contest2026_098_board/configs/nsh"
REPACK_PY="$OPENVELA_DIR/vendor/beken/boards/bk7258/bk7258-devkit/tools/repack.py"

echo "=== PSRAM build_and_pack ==="
echo "  board dir   : $BOARD_DIR"
echo "  config      : $CONFIG_PATH"

# Step 1: Build

echo ""
echo "--- Building ---"
cd "$OPENVELA_DIR"
./build.sh "$CONFIG_PATH" --cmake -j"$(nproc)"

# Step 2: Find nuttx.bin

NUTTX_BIN=""
for d in cmake_out/*contest*/nuttx.bin cmake_out/*board*/nuttx.bin; do
  if [ -f "$d" ]; then
    NUTTX_BIN="$d"
    break
  fi
done

if [ -z "$NUTTX_BIN" ]; then
  echo "ERROR: nuttx.bin not found in cmake_out/"
  exit 1
fi

echo ""
echo "--- Pack ---"
echo "  nuttx.bin : $NUTTX_BIN"
ls -l --time-style=long-iso "$NUTTX_BIN"
python3 "$REPACK_PY" --nuttx-bin "$NUTTX_BIN"

echo ""
echo "=== Done ==="
