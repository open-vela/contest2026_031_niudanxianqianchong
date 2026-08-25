#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Apply the tracked P4X NuttX compatibility patches to an existing disposable
# esp-hal-3rdparty checkout.  The normal Make/CMake paths apply them after a
# clean checkout; this helper is for an already configured build tree.

set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
HAL_DIR=${HAL_DIR:-"$PROJECT_DIR/chips/esp32p4/esp-hal-3rdparty"}
PATCH_DIR="$PROJECT_DIR/chips/esp32p4/common/espressif/patches"
NUTTX_DIR="${NUTTX_DIR:-$PROJECT_DIR/../nuttx}"

if [[ ! -d "$HAL_DIR/.git" ]]; then
  echo "error: P4X ESP HAL worktree not found: $HAL_DIR" >&2
  exit 1
fi

shopt -s nullglob
patches=("$PATCH_DIR"/*.patch)
if [[ ${#patches[@]} -eq 0 ]]; then
  echo "error: no P4X HAL patches found in $PATCH_DIR" >&2
  exit 1
fi

for patch in "${patches[@]}"; do
  if git -C "$HAL_DIR" apply --check "$patch" 2>/dev/null; then
    git -C "$HAL_DIR" apply "$patch"
    echo "APPLIED: $(basename "$patch")"
  elif git -C "$HAL_DIR" apply --reverse --check "$patch" 2>/dev/null; then
    echo "ALREADY APPLIED: $(basename "$patch")"
  else
    echo "error: patch does not match the pinned HAL: $patch" >&2
    exit 1
  fi
done

# A HAL patch can change inline code in platform headers.  These archives may
# contain objects compiled before the patch and must be rebuilt, but no source
# files or unrelated build outputs are removed.
rm -f "$NUTTX_DIR/arch/risc-v/src/libarch.a" \
      "$NUTTX_DIR/arch/risc-v/src/libkarch.a" \
      "$NUTTX_DIR/staging/libarch.a" \
      "$NUTTX_DIR/staging/libkarch.a"
find "$NUTTX_DIR/arch/risc-v/src/chip/esp-hal-3rdparty" \
  -type f \( -name '*.o' -o -name '*.d' \) -delete 2>/dev/null || true
echo "INVALIDATED: P4X architecture archives"
