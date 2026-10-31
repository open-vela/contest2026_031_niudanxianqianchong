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
#
# The script works from the main workspace and from a git worktree: the
# openvela root is located by walking up until a directory containing both
# nuttx/ and prebuilts/ is found, because a worktree's parent directory is
# worktrees/ rather than the repository root.

set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

find_openvela_root()
{
  local dir=$1

  while [[ "$dir" != "/" ]]; do
    if [[ -d "$dir/nuttx" && -d "$dir/prebuilts" && -f "$dir/build.sh" ]]; then
      echo "$dir"
      return 0
    fi
    dir=$(dirname "$dir")
  done

  return 1
}

if ! ROOT_DIR=$(find_openvela_root "$PROJECT_DIR"); then
  echo "error: cannot locate the openvela root above $PROJECT_DIR" >&2
  echo "       expected a directory containing nuttx/, prebuilts/ and build.sh" >&2
  exit 1
fi

CONFIG_NAME=${1:-smp_smart_home}
shift || true

BOARD_NAME=esp32p4-function-ev-board
CONFIG_DIR="$PROJECT_DIR/board/esp32p4/$BOARD_NAME/configs/$CONFIG_NAME"

if [[ ! -f "$CONFIG_DIR/defconfig" ]]; then
  echo "error: missing $CONFIG_DIR/defconfig" >&2
  exit 1
fi

XPACK_TOOLCHAIN_BIN="$ROOT_DIR/prebuilts/gcc/linux-x86_64/riscv-none-elf/bin"

# The Espressif toolchain is staged either in the contest repository's own
# prebuilts/ (scripts/setup_toolchain.sh destination) or in the shared
# openvela prebuilts/.  Prefer the repository copy so a branch checkout is
# self-contained.
ESP_TOOLCHAIN_BIN=""
for candidate in \
    "$PROJECT_DIR/prebuilts/gcc/linux-x86_64/riscv32-esp-elf/bin" \
    "$ROOT_DIR/prebuilts/gcc/linux-x86_64/riscv32-esp-elf/bin"; do
  if [[ -x "$candidate/riscv32-esp-elf-gcc" ]]; then
    ESP_TOOLCHAIN_BIN=$candidate
    break
  fi
done

if grep -q "^CONFIG_ESP32P4_ESPRESSIF_TOOLCHAIN=y" "$CONFIG_DIR/defconfig"; then
  if [[ -z "$ESP_TOOLCHAIN_BIN" ]]; then
    echo "error: CONFIG_ESP32P4_ESPRESSIF_TOOLCHAIN=y but no toolchain found in" >&2
    echo "       $PROJECT_DIR/prebuilts/gcc/linux-x86_64/riscv32-esp-elf/bin" >&2
    echo "       $ROOT_DIR/prebuilts/gcc/linux-x86_64/riscv32-esp-elf/bin" >&2
    echo "       run scripts/setup_toolchain.sh first" >&2
    exit 1
  fi
  export PATH="$ESP_TOOLCHAIN_BIN:$PATH"
  echo "CC: $(command -v riscv32-esp-elf-gcc) ($(riscv32-esp-elf-gcc -dumpversion))"
else
  export PATH="$XPACK_TOOLCHAIN_BIN:$PATH"
  echo "CC: $(command -v riscv-none-elf-gcc || echo "<none>")"
fi

# The image packaging step (nuttx/tools/espressif/check_esptool.py) checks the
# esptool *Python package*, not the esptool launcher on PATH.  Without the
# venv the link succeeds but nuttx.bin is never produced, so activate it here
# like build_cmake_p4x.sh does.
if [[ -f "$ROOT_DIR/myenv/bin/activate" ]]; then
  # shellcheck disable=SC1091
  source "$ROOT_DIR/myenv/bin/activate"
fi

# build.sh resolves the board configuration relative to ROOT_DIR, so pass the
# path relative to it rather than to the project directory (they differ when
# this script runs from a worktree).
BOARD_CONFIG=$(realpath --relative-to="$ROOT_DIR" "$CONFIG_DIR")

cd "$ROOT_DIR"
exec ./build.sh "$BOARD_CONFIG" "$@"
