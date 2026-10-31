#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Build an ESP32-P4X contest configuration with the NuttX CMake path.
#
# The Make path creates arch/<arch>/src/chip, include/arch/chip and the dummy
# Kconfig copies during configure.  CMake evaluates Kconfig before those rules
# exist, so this wrapper prepares the same inputs and then drives cmake
# directly (build.sh skips configuration when its binary directory exists).
#
# Usage: scripts/build_cmake_p4x.sh [config-name] [jobs]
#   config-name  directory under board/.../configs (default: smp_smart_home)
#   jobs         parallel build jobs (default: 4)

set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
ROOT_DIR=$(CDPATH= cd -- "$PROJECT_DIR/.." && pwd)

CONFIG_NAME=${1:-smp_smart_home}
JOBS=${2:-4}

BOARD_NAME=esp32p4-function-ev-board
CONFIG_DIR="$PROJECT_DIR/board/esp32p4/$BOARD_NAME/configs/$CONFIG_NAME"
CHIP_DIR="$PROJECT_DIR/chips/esp32p4"
BOARD_DIR="$PROJECT_DIR/board/esp32p4/$BOARD_NAME"
BOARD_COMMON_DIR="$PROJECT_DIR/board/esp32p4/common"
NUTTX_DIR="$ROOT_DIR/nuttx"
BINARY_DIR="$ROOT_DIR/cmake_out/${BOARD_NAME}_${CONFIG_NAME}"

if [[ ! -f "$CONFIG_DIR/defconfig" ]]; then
  echo "error: missing $CONFIG_DIR/defconfig" >&2
  exit 1
fi

PATCH_DIR="$PROJECT_DIR/patches/nuttx"
for patch in "$PATCH_DIR"/*.patch; do
  if git -C "$NUTTX_DIR" apply --reverse --check "$patch" >/dev/null 2>&1; then
    continue
  fi
  if git -C "$NUTTX_DIR" apply --check "$patch" >/dev/null 2>&1; then
    git -C "$NUTTX_DIR" apply "$patch"
    echo "APPLIED: $(basename "$patch")"
  fi
done

# Kconfig inputs normally prepared by the Make configure/context rules.
mkdir -p "$NUTTX_DIR/arch/risc-v/src" "$NUTTX_DIR/include/arch" \
         "$NUTTX_DIR/arch/dummy" "$NUTTX_DIR/boards/dummy"
ln -sfn "$CHIP_DIR" "$NUTTX_DIR/arch/risc-v/src/chip"
ln -sfn "$CHIP_DIR/include" "$NUTTX_DIR/include/arch/chip"
cp -f "$CHIP_DIR/Kconfig" "$NUTTX_DIR/arch/dummy/Kconfig"
cp -f "$BOARD_DIR/Kconfig" "$NUTTX_DIR/boards/dummy/Kconfig"

# The custom board Kconfig sources $BINDIR/arch/risc-v/src/board/Kconfig.
rm -rf "$BINARY_DIR"
mkdir -p "$BINARY_DIR/arch/risc-v/src"
ln -sfn "$BOARD_COMMON_DIR" "$BINARY_DIR/arch/risc-v/src/board"

# Toolchain selection.  CONFIG_ESP32P4_ESPRESSIF_TOOLCHAIN decides which
# compiler the NuttX toolchain files pick; put the matching bin directory
# first on PATH so every invocation in this script sees the same one.
XPACK_TOOLCHAIN_BIN="$ROOT_DIR/prebuilts/gcc/linux-x86_64/riscv-none-elf/bin"

# Prefer the contest repository's own prebuilts/ (scripts/setup_toolchain.sh
# destination), then fall back to the shared openvela copy.
ESP_TOOLCHAIN_BIN=""
for candidate in \
    "$PROJECT_DIR/prebuilts/gcc/linux-x86_64/riscv32-esp-elf/bin" \
    "$ROOT_DIR/prebuilts/gcc/linux-x86_64/riscv32-esp-elf/bin"; do
  if [[ -x "$candidate/riscv32-esp-elf-gcc" ]]; then
    ESP_TOOLCHAIN_BIN=$candidate
    break
  fi
done

if [[ -n "$ESP_TOOLCHAIN_BIN" ]] &&
   grep -q '^CONFIG_ESP32P4_ESPRESSIF_TOOLCHAIN=y' \
        "$CONFIG_DIR/defconfig" 2>/dev/null; then
  export PATH="$ESP_TOOLCHAIN_BIN:$PATH"
else
  export PATH="$XPACK_TOOLCHAIN_BIN:$PATH"
fi

# shellcheck disable=SC1091
source "$ROOT_DIR/myenv/bin/activate"

# Report the compiler that will actually be used; a mismatch here means the
# PATH setup above and the NuttX toolchain selection disagree.
if command -v riscv32-esp-elf-gcc >/dev/null 2>&1; then
  echo "CC: $(command -v riscv32-esp-elf-gcc) ($(riscv32-esp-elf-gcc -dumpversion))"
else
  echo "CC: $(command -v riscv-none-elf-gcc || echo '<none>')"
fi

BOARD_CONFIG="../$(basename "$PROJECT_DIR")/board/esp32p4/$BOARD_NAME/configs/$CONFIG_NAME"

cmake -B "$BINARY_DIR" -S "$NUTTX_DIR" \
  -DBOARD_CONFIG="$BOARD_CONFIG" \
  -DCUSTOM_MODULE_PATH="$ROOT_DIR/build/cmake" \
  -DEXTRA_FLAGS="-Wno-cpp -Wno-deprecated-declarations" \
  -GNinja

cmake --build "$BINARY_DIR" -j"$JOBS"

echo "CMake image: $BINARY_DIR/nuttx.bin"
