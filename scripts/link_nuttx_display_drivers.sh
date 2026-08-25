#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Create the development-time relative links required by the P4X build.  This
# script deliberately does not edit any NuttX build or Kconfig files.

set -eu

usage()
{
  echo "Usage: $0 [--check] [NUTTX_DIR]" >&2
}

mode=sync

if [ "${1:-}" = "--check" ]; then
  mode=check
  shift
fi

if [ "$#" -gt 1 ]; then
  usage
  exit 2
fi

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
workspace_dir=$(CDPATH= cd -- "$project_dir/.." && pwd)
nuttx_dir=${NUTTX_DIR:-"$workspace_dir/nuttx"}

if [ "$#" -eq 1 ]; then
  nuttx_dir=$1
fi

source_root="$project_dir/drivers/nuttx"

link_one()
{
  source_file=$1
  destination_file=$2
  expected_target=$3

  if [ ! -f "$source_file" ]; then
    echo "error: source file is missing: $source_file" >&2
    return 1
  fi

  if [ -L "$destination_file" ]; then
    if [ "$(readlink "$destination_file")" = "$expected_target" ]; then
      echo "OK: $destination_file -> $expected_target"
      return 0
    fi

    echo "error: $destination_file is linked to $(readlink "$destination_file"), expected $expected_target" >&2
    return 1
  fi

  if [ -e "$destination_file" ]; then
    echo "error: refusing to replace existing file $destination_file" >&2
    return 1
  fi

  if [ "$mode" = "check" ]; then
    echo "MISSING: $destination_file" >&2
    return 1
  fi

  mkdir -p "$(dirname "$destination_file")"
  ln -s "$expected_target" "$destination_file"
  echo "LINKED: $destination_file -> $expected_target"
}

remove_legacy_link()
{
  destination_file=$1
  legacy_target=$2

  if [ ! -L "$destination_file" ]; then
    return 0
  fi

  if [ "$(readlink "$destination_file")" != "$legacy_target" ]; then
    echo "error: refusing to replace unexpected link $destination_file -> $(readlink "$destination_file")" >&2
    return 1
  fi

  if [ "$mode" = "check" ]; then
    echo "LEGACY: $destination_file -> $legacy_target" >&2
    return 1
  fi

  rm "$destination_file"
  echo "REMOVED legacy link: $destination_file"
}

if [ ! -d "$nuttx_dir/drivers" ] || [ ! -d "$nuttx_dir/include/nuttx" ]; then
  echo "error: NuttX worktree not found: $nuttx_dir" >&2
  exit 1
fi

# An earlier development revision created nuttx/external -> ../external for
# FreeType.  In the OpenVela layout that makes the kernel invoke the root
# external Makefile without APPDIR and breaks pass2dep at /Directory.mk.
# Runtime P4X fonts use LVGL TinyTTF instead, so this link must stay absent.
if [ -L "$nuttx_dir/external" ]; then
  if [ "$(readlink "$nuttx_dir/external")" != "../external" ]; then
    echo "error: refusing to remove unexpected link $nuttx_dir/external -> $(readlink "$nuttx_dir/external")" >&2
    exit 1
  fi
  if [ "$mode" = "check" ]; then
    echo "STALE: $nuttx_dir/external -> ../external (remove it)" >&2
    exit 1
  fi

  rm "$nuttx_dir/external"
  echo "REMOVED stale link: $nuttx_dir/external -> ../external"
elif [ -e "$nuttx_dir/external" ]; then
  echo "error: unexpected path exists: $nuttx_dir/external" >&2
  exit 1
fi

if [ ! -f "$source_root/drivers/lcd/ek79007.c" ] || \
   [ ! -f "$source_root/drivers/lcd/ek79007.h" ] || \
   [ ! -f "$source_root/drivers/input/gt911.c" ] || \
   [ ! -f "$source_root/drivers/input/gt911.h" ]; then
  echo "error: display driver source skeleton is incomplete under $source_root" >&2
  exit 1
fi

remove_legacy_link \
  "$nuttx_dir/drivers/video/mipidsi/ek79007.c" \
  "../../../../contest2026_031_niudanxianqianchong/drivers/nuttx/drivers/video/mipidsi/ek79007.c"
remove_legacy_link \
  "$nuttx_dir/include/nuttx/video/ek79007.h" \
  "../../../../contest2026_031_niudanxianqianchong/drivers/nuttx/include/nuttx/video/ek79007.h"

link_one \
  "$source_root/drivers/lcd/ek79007.c" \
  "$nuttx_dir/drivers/lcd/ek79007.c" \
  "../../../contest2026_031_niudanxianqianchong/drivers/nuttx/drivers/lcd/ek79007.c"
link_one \
  "$source_root/drivers/lcd/ek79007.h" \
  "$nuttx_dir/drivers/lcd/ek79007.h" \
  "../../../contest2026_031_niudanxianqianchong/drivers/nuttx/drivers/lcd/ek79007.h"
link_one \
  "$source_root/drivers/input/gt911.c" \
  "$nuttx_dir/drivers/input/gt911.c" \
  "../../../contest2026_031_niudanxianqianchong/drivers/nuttx/drivers/input/gt911.c"
link_one \
  "$source_root/drivers/input/gt911.h" \
  "$nuttx_dir/drivers/input/gt911.h" \
  "../../../contest2026_031_niudanxianqianchong/drivers/nuttx/drivers/input/gt911.h"
