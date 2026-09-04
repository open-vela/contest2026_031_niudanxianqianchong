#!/usr/bin/env bash
#
# Build a LittleFS data image for the ESP32-P4X Function EV Board.
#
# The image is mounted by the board at /data and contains Smart Home runtime
# resources.  Secrets are deliberately excluded unless WITH_SECRETS=1 is set.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OPENVELA_ROOT="$(cd "${PROJECT_ROOT}/.." && pwd)"
SMART_HOME_DIR="${PROJECT_ROOT}/demos/smart_home"

OUT_DIR="${OUT_DIR:-${OPENVELA_ROOT}/out/p4x_littlefs_data}"
STAGING_DIR="${STAGING_DIR:-${OUT_DIR}/staging}"
OUT_IMAGE="${OUT_IMAGE:-${OUT_DIR}/data_lfs.bin}"
MKLITTLEFS="${MKLITTLEFS:-${OPENVELA_ROOT}/vendor/artinchip/tools/scripts/mklittlefs}"

# Keep these values synchronized with the P4X LittleFS storage configuration:
# the SPI Flash MTD reports a 64-byte program block and LittleFS uses the
# configured PROGRAM_SIZE_FACTOR (4), therefore prog/page size is 256 bytes.
DATA_SIZE="${DATA_SIZE:-0x100000}"
BLOCK_SIZE="${BLOCK_SIZE:-4096}"
PAGE_SIZE="${PAGE_SIZE:-256}"
FLASH_OFFSET="${FLASH_OFFSET:-0x800000}"

CONFIG_DIR="${CONFIG_DIR:-${SMART_HOME_DIR}/res/config}"
SECRETS_FILE="${SECRETS_FILE:-${CONFIG_DIR}/secrets.json}"
WITH_SECRETS="${WITH_SECRETS:-0}"
# Keep the P4X runtime resource layout aligned with the Smart Home UI paths:
# /data/res/fonts/MiSans-Normal.ttf and /data/res/icons/*.png.
# Use the pre-generated subset font so the LittleFS image remains small.
# Embedded LVGL icon fonts are C sources and are compiled into the firmware;
# they intentionally do not belong in this data image.
WITH_ICONS="${WITH_ICONS:-1}"
WITH_FONTS="${WITH_FONTS:-1}"
FONT_SOURCE="${FONT_SOURCE:-${SMART_HOME_DIR}/res/fonts/MiSans-Normal-subset.ttf}"

if [[ ! -x "${MKLITTLEFS}" ]]; then
  echo "mklittlefs not found or not executable: ${MKLITTLEFS}" >&2
  echo "Set MKLITTLEFS=/path/to/mklittlefs and retry." >&2
  exit 1
fi

if [[ ! -d "${SMART_HOME_DIR}/res/skills" ]]; then
  echo "skills directory not found: ${SMART_HOME_DIR}/res/skills" >&2
  exit 1
fi

if [[ ! -d "${CONFIG_DIR}" ]]; then
  echo "config directory not found: ${CONFIG_DIR}" >&2
  exit 1
fi

# OUT_DIR is derived from a caller-controlled output path.  Restrict removal
# to its dedicated staging child so resource packaging cannot erase sources.
rm -rf "${STAGING_DIR}"
mkdir -p "${STAGING_DIR}/res/skills" "${STAGING_DIR}/smart_home"

skill_count=0
while IFS= read -r -d '' skill_file; do
  cp "${skill_file}" "${STAGING_DIR}/res/skills/"
  skill_count=$((skill_count + 1))
done < <(find "${SMART_HOME_DIR}/res/skills" -maxdepth 1 -type f -name '*.md' -print0)

if [[ "${skill_count}" -eq 0 ]]; then
  echo "no skill Markdown files found in ${SMART_HOME_DIR}/res/skills" >&2
  exit 1
fi

config_count=0
while IFS= read -r -d '' config_file; do
  if [[ "${config_file}" == "${SECRETS_FILE}" ]]; then
    continue
  fi

  cp "${config_file}" "${STAGING_DIR}/smart_home/$(basename "${config_file}")"
  config_count=$((config_count + 1))
done < <(find "${CONFIG_DIR}" -maxdepth 1 -type f -name '*.json' -print0)

if [[ "${config_count}" -eq 0 ]]; then
  echo "no non-secret JSON config found in ${CONFIG_DIR}" >&2
  exit 1
fi

if [[ "${WITH_ICONS}" == "1" ]]; then
  if [[ ! -d "${SMART_HOME_DIR}/res/icons" ]]; then
    echo "icons directory not found: ${SMART_HOME_DIR}/res/icons" >&2
    exit 1
  fi

  mkdir -p "${STAGING_DIR}/res/icons"
  find "${SMART_HOME_DIR}/res/icons" -maxdepth 1 -type f -name '*.png' \
    -exec cp {} "${STAGING_DIR}/res/icons/" \;
fi

if [[ "${WITH_FONTS}" == "1" ]]; then
  if [[ ! -f "${FONT_SOURCE}" ]]; then
    echo "MiSans subset font not found: ${FONT_SOURCE}" >&2
    echo "Set FONT_SOURCE=/path/to/MiSans-Normal-subset.ttf and retry." >&2
    exit 1
  fi

  mkdir -p "${STAGING_DIR}/res/fonts"
  cp "${FONT_SOURCE}" "${STAGING_DIR}/res/fonts/MiSans-Normal.ttf"
fi

if [[ "${WITH_SECRETS}" == "1" ]]; then
  if [[ ! -f "${SECRETS_FILE}" ]]; then
    echo "secrets file not found: ${SECRETS_FILE}" >&2
    exit 1
  fi

  cp "${SECRETS_FILE}" "${STAGING_DIR}/smart_home/secrets.json"
fi

mkdir -p "$(dirname "${OUT_IMAGE}")"

"${MKLITTLEFS}" \
  -c "${STAGING_DIR}" \
  -b "${BLOCK_SIZE}" \
  -p "${PAGE_SIZE}" \
  -s "${DATA_SIZE}" \
  "${OUT_IMAGE}"

echo "LittleFS image: ${OUT_IMAGE}"
echo "Staged files:"
find "${STAGING_DIR}" -type f -printf '  /%P\n' | sort
echo "Flash with:"
printf '  esptool --chip esp32p4 --port /dev/ttyACM0 --baud 921600 \\\n'
printf '  write-flash -fs 16MB -fm dio -ff 80m %s %s\n' \
  "${FLASH_OFFSET}" "${OUT_IMAGE}"
