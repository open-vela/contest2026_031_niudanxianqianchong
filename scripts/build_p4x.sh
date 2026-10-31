#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Build an ESP32-P4X contest configuration through the Make path, selecting
# the compiler that CONFIG_ESP32P4_ESPRESSIF_TOOLCHAIN asks for.
#
# The openvela entry point (build.sh) sources build/envsetup.sh, which only
# adds prebuilts/gcc/<host>/{riscv,riscv-none}-{eabi,elf}/bin to PATH and
# therefore never finds riscv32-esp-elf.  This wrapper puts the right bin
# directory first and then hands over to ./build.sh unchanged, so the shared
# workspace is never modified.
#
# Usage: scripts/build_p4x.sh [config-name] [build.sh args...]
#   config-name  directory under board/esp32p4/esp32p4-function-ev-board/configs
#                (default: smp_smart_home)

set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
ROOT_DIR=$(CDPATH= cd -- "$PROJECT_DIR/.." && pwd)

CONFIG_NAME=${1:-smp_smart_home}
shift || true

BOARD_NAME=esp32p4-function-ev-board
CONFIG_DIR="$PROJECT_DIR/board/esp32p4/$BOARD_NAME/configs/$CONFIG_NAME"

if [[ ! -f "$CONFIG_DIR/defconfig" ]]; then
  echo "error: missing $CONFIG_DIR/defconfig" >&2
  exit 1
fi

ESP_TOOLCHAIN_BIN="$ROOT_DIR/prebuilts/gcc/linux-x86_64/riscv32-esp-elf/bin"
XPACK_TOOLCHAIN_BIN="$ROOT_DIR/prebuilts/gcc/linux-x86_64/riscv-none-elf/bin"

if grep -q "^CONFIG_ESP32P4_ESPRESSIF_TOOLCHAIN=y" "$CONFIG_DIR/defconfig"; then
  if [[ ! -x "$ESP_TOOLCHAIN_BIN/riscv32-esp-elf-gcc" ]]; then
    echo "error: CONFIG_ESP32P4_ESPRESSIF_TOOLCHAIN=y but no toolchain at" >&2
    echo "       $ESP_TOOLCHAIN_BIN" >&2
    echo "       see the toolchain switch plan under docs/开发计划/基础与规划/" >&2
    exit 1
  fi
  export PATH="$ESP_TOOLCHAIN_BIN:$PATH"
  echo "CC: $(command -v riscv32-esp-elf-gcc) ($(riscv32-esp-elf-gcc -dumpversion))"
else
  export PATH="$XPACK_TOOLCHAIN_BIN:$PATH"
  echo "CC: $(command -v riscv-none-elf-gcc || echo "<none>")"
fi

BOARD_CONFIG="$(basename "$PROJECT_DIR")/board/esp32p4/$BOARD_NAME/configs/$CONFIG_NAME"

cd "$ROOT_DIR"
exec ./build.sh "$BOARD_CONFIG" "$@"
