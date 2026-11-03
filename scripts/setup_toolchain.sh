#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Stage the Espressif riscv32-esp-elf toolchain under prebuilts/ for ESP32-P4
# builds that enable CONFIG_ESP32P4_ESPRESSIF_TOOLCHAIN.
#
# The result is ~430 MB after trimming, is listed in .gitignore and must never
# be committed.  See prebuilts/README.md for the expected layout and the list
# of removed components.
#
# Usage:
#   scripts/setup_toolchain.sh            # install (download + verify + trim)
#   scripts/setup_toolchain.sh --check    # verify an existing installation
#   scripts/setup_toolchain.sh --source DIR
#                                         # copy from an existing installation
#                                         # instead of downloading
#
# Environment:
#   TOOLCHAIN_VERSION   release tag (default esp-14.2.0_20260121)
#   TOOLCHAIN_MIRROR    base URL for the archive
#                       (default https://github.com/espressif/crosstool-NG/releases/download)

set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
DEST="$PROJECT_DIR/prebuilts/gcc/linux-x86_64/riscv32-esp-elf"

VERSION="${TOOLCHAIN_VERSION:-esp-14.2.0_20260121}"
HOST_TAG="x86_64-linux-gnu"
ARCHIVE="riscv32-esp-elf-14.2.0_20260121-${HOST_TAG}.tar.xz"
MIRROR="${TOOLCHAIN_MIRROR:-https://github.com/espressif/crosstool-NG/releases/download}"
SHA256_FILE="crosstool-NG-esp-14.2.0_20260121-checksum.sha256"

# Only this multilib is kept; the platform always builds with
# -march=rv32imac_zicsr_zifencei -mabi=ilp32.
KEEP_MULTILIB="rv32imac_zicsr_zifencei"



usage() {
  sed -n '4,26p' "$0" | sed 's/^# \{0,1\}//'
}

verify() {
  local gcc="$DEST/bin/riscv32-esp-elf-gcc"

  if [[ ! -x "$gcc" ]]; then
    echo "FAIL: $gcc not found" >&2
    return 1
  fi

  local ver
  ver=$("$gcc" -dumpfullversion 2>/dev/null || "$gcc" -dumpversion)
  echo "OK:   compiler $ver ($("$gcc" --version | head -1))"

  local multi
  multi=$("$gcc" -march="$KEEP_MULTILIB" -mabi=ilp32 -print-multi-directory)
  if [[ "$multi" != "$KEEP_MULTILIB/ilp32" && "$multi" != "." ]]; then
    echo "FAIL: unexpected multilib '$multi'" >&2
    return 1
  fi
  echo "OK:   multilib $multi"

  local lib
  for lib in libgcc.a libm.a libsupc++.a libstdc++.a; do
    if ! "$gcc" -march="$KEEP_MULTILIB" -mabi=ilp32 \
         --print-file-name="$lib" | xargs -r test -f; then
      echo "FAIL: missing $lib" >&2
      return 1
    fi
  done
  echo "OK:   libgcc / libm / libsupc++ / libstdc++ present"

  # The whole point of this toolchain: it must assemble PIE opcodes.
  local tmp
  tmp=$(mktemp -d)
  cat > "$tmp/pie.S" <<'EOF'
    .text
    .globl probe
probe:
    esp.vld.128.ip     q0, x10, 16
    esp.vmulas.s8.xacc q0, q1
    ret
EOF
  if ! "$gcc" -march="${KEEP_MULTILIB}_xespv" -mabi=ilp32 \
         -c "$tmp/pie.S" -o "$tmp/pie.o" 2>"$tmp/err"; then
    echo "FAIL: cannot assemble PIE opcodes: $(head -1 "$tmp/err")" >&2
    rm -rf "$tmp"
    return 1
  fi
  rm -rf "$tmp"
  echo "OK:   xespv PIE opcodes assemble"

  echo
  echo "size: $(du -sh "$DEST" | cut -f1)"
  echo "installation is usable"
}

trim() {
  local root=$1

  echo "== trimming =="

  rm -rf "$root/picolibc"
  echo "  removed picolibc"

  local d b
  for d in "$root"/lib/gcc/riscv32-esp-elf/*/; do
    b=$(basename "$d")
    case "$b" in
      "$KEEP_MULTILIB"|include|include-fixed|install-tools) ;;
      *) rm -rf "$d"; echo "  removed lib/gcc/$b" ;;
    esac
  done

  for d in "$root"/riscv32-esp-elf/lib/*/; do
    b=$(basename "$d")
    case "$b" in
      "$KEEP_MULTILIB"|ldscripts) ;;
      *) rm -rf "$d"; echo "  removed sysroot lib/$b" ;;
    esac
  done

  # NuttX does not build with LTO; these three are ~100 MB.
  rm -f "$root/bin/riscv32-esp-elf-lto-dump"
  rm -f "$root/libexec/gcc/riscv32-esp-elf/"*/lto1
  rm -f "$root/libexec/gcc/riscv32-esp-elf/"*/lto-wrapper
  echo "  removed LTO components"
}

install_from_source() {
  local src=$1

  if [[ ! -x "$src/bin/riscv32-esp-elf-gcc" ]]; then
    echo "error: $src does not look like a riscv32-esp-elf installation" >&2
    exit 1
  fi

  echo "== copying from $src =="
  rm -rf "$DEST"
  mkdir -p "$(dirname "$DEST")"
  cp -a "$src" "$DEST"
  trim "$DEST"
}

install_from_archive() {
  local work url
  work=$(mktemp -d)
  url="$MIRROR/$VERSION/${ARCHIVE}"

  echo "== downloading $url =="
  curl -fL --retry 3 -o "$work/$ARCHIVE" "$url"
  curl -fL --retry 3 -o "$work/checksums" "$MIRROR/$VERSION/${SHA256_FILE}" || true

  if [[ -s "$work/checksums" ]]; then
    echo "== verifying sha256 =="
    local want got
    want=$(grep -F "$ARCHIVE" "$work/checksums" | awk '{print $1}' | head -1)
    got=$(sha256sum "$work/$ARCHIVE" | awk '{print $1}')
    if [[ -z "$want" ]]; then
      echo "warning: $ARCHIVE not listed in checksum file; skipping" >&2
    elif [[ "$want" != "$got" ]]; then
      echo "FAIL: sha256 mismatch" >&2
      echo "  expected $want" >&2
      echo "  got      $got" >&2
      exit 1
    else
      echo "OK:   sha256 $got"
    fi
  else
    echo "warning: no checksum file available; continuing unverified" >&2
  fi

  echo "== extracting =="
  tar -xf "$work/$ARCHIVE" -C "$work"
  local root
  root=$(find "$work" -maxdepth 2 -type d -name "riscv32-esp-elf" | head -1)
  if [[ -z "$root" ]]; then
    echo "error: unexpected archive layout" >&2
    exit 1
  fi

  rm -rf "$DEST"
  mkdir -p "$(dirname "$DEST")"
  cp -a "$root" "$DEST"
  trim "$DEST"
  rm -rf "$work"
}

case "${1:-}" in
  "")            mode=install ;;
  --check)       mode=check ;;
  --source)      mode=source; shift; SRC="${1:-}"; [[ -n "$SRC" ]] || { usage; exit 2; } ;;
  -h|--help)     usage; exit 0 ;;
  *)             usage; exit 2 ;;
esac

if [[ "$mode" == check ]]; then
  verify
  exit 0
fi

if [[ "$mode" == install && -x "$DEST/bin/riscv32-esp-elf-gcc" ]]; then
  echo "already installed at $DEST (use --check to validate)"
  exit 0
fi

if [[ "$mode" == source ]]; then
  install_from_source "$SRC"
else
  install_from_archive
fi

echo
verify
