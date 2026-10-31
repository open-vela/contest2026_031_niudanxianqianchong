#!/usr/bin/env bash
#
# 一键跑 V9 板端实测的前置步骤：构建 / 生成数据镜像 / 烧录 / 采集日志。
#
# 默认只打印计划，不做任何事；每个动作都要显式开关：
#
#   ./scripts/v9_board_measure.sh --plan                 # 打印将执行的命令
#   ./scripts/v9_board_measure.sh --build --image        # 只构建并生成镜像
#   ./scripts/v9_board_measure.sh --backup               # 先备份板上 16 MiB flash（会复位芯片）
#   ./scripts/v9_board_measure.sh --flash                # 烧录（会覆盖板上固件）
#   ./scripts/v9_board_measure.sh --capture 1800         # 采集 30 分钟串口日志
#   ./scripts/v9_board_measure.sh --all --capture 1800   # 全流程
#
# 注意：构建阶段会临时把 openvela 构建树里的 board / demo / cAGENT 软链接
# 指向本 worktree，退出时无条件还原（trap）。

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OPENVELA_ROOT="${OPENVELA_ROOT:-/home/arongw/openvela}"
CONFIG="${CONFIG:-smart_home_service_lab}"
PORT="${PORT:-/dev/ttyACM0}"
BAUD="${BAUD:-921600}"
LOG="${LOG:-${PROJECT_ROOT}/board.log}"

DO_BUILD=0 DO_IMAGE=0 DO_FLASH=0 DO_BACKUP=0 CAPTURE_SECONDS=0 BACKUP=""

while [ $# -gt 0 ]; do
    case "$1" in
        --plan) ;;
        --build) DO_BUILD=1 ;;
        --image) DO_IMAGE=1 ;;
        --flash) DO_FLASH=1 ;;
        --backup) DO_BACKUP=1 ;;
        --capture) shift; CAPTURE_SECONDS="${1:-0}" ;;
        --all) DO_BUILD=1; DO_IMAGE=1; DO_FLASH=1 ;;
        -h|--help) sed -n '2,20p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 2 ;;
    esac
    shift
done

CONFIG_PATH="${PROJECT_ROOT}/board/esp32p4/esp32p4-function-ev-board/configs/${CONFIG}"
LINK_PAIRS=(
  "vendor/espressif/boards/esp32p4/esp32p4-function-ev-board|${PROJECT_ROOT}/board/esp32p4/esp32p4-function-ev-board"
  "packages/demos/smart_home|${PROJECT_ROOT}/demos/smart_home"
  "packages/cAGENT|${PROJECT_ROOT}/packages/cAGENT"
  "packages/cagent_addons|${PROJECT_ROOT}/packages/cagent_addons"
)

# Snapshot every link before changing any of them, including dangling links.
ORIGINAL_TARGETS=()
ORIGINAL_PRESENT=()
SWITCHED_LINKS=0

save_links() {
    local pair link target path
    for pair in "${LINK_PAIRS[@]}"; do
        IFS='|' read -r link target <<< "$pair"
        path="${OPENVELA_ROOT}/${link}"
        if [ -L "$path" ]; then
            target=$(readlink -- "$path") || return 1
            ORIGINAL_TARGETS+=("$target")
            ORIGINAL_PRESENT+=(1)
        elif [ -e "$path" ]; then
            echo "拒绝替换非软链接路径: $path" >&2
            return 1
        else
            ORIGINAL_TARGETS+=("")
            ORIGINAL_PRESENT+=(0)
        fi
    done
}

restore_links() {
    local i link target result=0
    for ((i=0; i<SWITCHED_LINKS; i++)); do
        IFS='|' read -r link target <<< "${LINK_PAIRS[i]}"
        if [ "${ORIGINAL_PRESENT[i]}" -eq 1 ]; then
            ln -sfn -- "${ORIGINAL_TARGETS[i]}" "${OPENVELA_ROOT}/${link}" || result=1
        elif [ -L "${OPENVELA_ROOT}/${link}" ]; then
            rm -- "${OPENVELA_ROOT}/${link}" || result=1
        fi
    done
    return "$result"
}

plan() {
    echo "openvela 根 : ${OPENVELA_ROOT}"
    echo "worktree    : ${PROJECT_ROOT}"
    echo "配置        : ${CONFIG}"
    echo "串口        : ${PORT} (${BAUD})"
    echo
    echo "构建   : cd ${OPENVELA_ROOT} && ./build.sh ${CONFIG_PATH} -j8"
    echo "镜像   : OPENVELA_ROOT=${OPENVELA_ROOT} ./scripts/make_p4x_littlefs_data_image.sh"
    echo "备份   : esptool.py --chip esp32p4 --port ${PORT} read_flash 0x0 0x1000000 out/board_backup_<ts>.bin（会复位芯片）"
    echo "烧录   : esptool.py --chip esp32p4 --port ${PORT} --baud ${BAUD} write_flash -fs 16MB -fm dio -ff 80m 0x2000 nuttx/nuttx.bin 0x600000 ${OPENVELA_ROOT}/out/p4x_littlefs_data/data_lfs.bin"
    echo "采集   : python3 读 ${PORT} 写入 ${LOG}"
    echo "汇总   : python3 scripts/v9_perf_report.py ${LOG} --out report.md"
}

if [ "${DO_BUILD}" -eq 0 ] && [ "${DO_IMAGE}" -eq 0 ] && [ "${DO_FLASH}" -eq 0 ] && [ "${DO_BACKUP}" -eq 0 ] && [ "${CAPTURE_SECONDS}" -eq 0 ]; then
    plan
    exit 0
fi

if [ "${DO_BUILD}" -eq 1 ]; then
    save_links || exit 1
    trap restore_links EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
    for pair in "${LINK_PAIRS[@]}"; do
        IFS='|' read -r link target <<< "$pair"
        SWITCHED_LINKS=$((SWITCHED_LINKS + 1))
        ln -sfn -- "$target" "${OPENVELA_ROOT}/${link}" || exit 1
    done
    echo "== 构建 ${CONFIG} =="
    ( cd "${OPENVELA_ROOT}" && source myenv/bin/activate && \
      ./build.sh "${CONFIG_PATH}" -j8 )
    rc=$?
    restore_links || exit 1
    trap - EXIT INT TERM
    if [ "$rc" -ne 0 ]; then
        echo "构建失败：退出码 ${rc}，拒绝使用可能残留的旧固件" >&2
        exit "$rc"
    fi
    if [ ! -f "${OPENVELA_ROOT}/nuttx/nuttx.bin" ]; then
        echo "构建失败：没有 nuttx.bin（build 退出码 ${rc}）" >&2
        exit 1
    fi
    ls -l "${OPENVELA_ROOT}/nuttx/nuttx.bin"
fi

if [ "${DO_IMAGE}" -eq 1 ]; then
    echo "== 生成数据镜像 =="
    ( cd "${PROJECT_ROOT}" && OPENVELA_ROOT="${OPENVELA_ROOT}" ./scripts/make_p4x_littlefs_data_image.sh ) || exit $?
fi

if [ "${DO_BACKUP}" -eq 1 ]; then
    # 先把板上现有 16 MiB flash 整体读出，便于回滚。注意 esptool 会把芯片
    # 复位进下载模式，当前运行的固件会被打断。
    BACKUP="${OPENVELA_ROOT}/out/board_backup_$(date +%Y%m%d-%H%M%S).bin"
    mkdir -p "$(dirname "${BACKUP}")"
    echo "== 备份板上 flash 到 ${BACKUP}（会复位芯片）=="
    ( cd "${OPENVELA_ROOT}" && source myenv/bin/activate && \
      esptool.py --chip esp32p4 --port "${PORT}" --baud "${BAUD}" \
        read_flash 0x0 0x1000000 "${BACKUP}" ) || { echo "备份失败，中止" >&2; exit 1; }
    ls -l "${BACKUP}"
    echo "回滚: esptool.py --chip esp32p4 --port ${PORT} --baud ${BAUD} write_flash -fs 16MB -fm dio -ff 80m 0x0 ${BACKUP}"
fi

if [ "${DO_FLASH}" -eq 1 ]; then
    echo "== 烧录（覆盖板上固件）=="
    ( cd "${OPENVELA_ROOT}" && source myenv/bin/activate && \
      esptool.py --chip esp32p4 --port "${PORT}" --baud "${BAUD}" \
        write_flash -fs 16MB -fm dio -ff 80m \
        0x2000 nuttx/nuttx.bin \
        0x600000 out/p4x_littlefs_data/data_lfs.bin )
fi

if [ "${CAPTURE_SECONDS}" -gt 0 ]; then
    echo "== 采集 ${CAPTURE_SECONDS}s 到 ${LOG} =="
    ( cd "${OPENVELA_ROOT}" && source myenv/bin/activate && \
      python3 - "${PORT}" "${LOG}" "${CAPTURE_SECONDS}" <<'PY'
import serial, sys, time
port, path, seconds = sys.argv[1], sys.argv[2], float(sys.argv[3])
link = serial.Serial(port, 115200, timeout=0.2)
end = time.time() + seconds
with open(path, 'ab', buffering=0) as out:
    while time.time() < end:
        data = link.read(4096)
        if data:
            sys.stdout.write(data.decode('utf-8', 'replace'))
            out.write(data)
link.close()
PY
    )
    echo "日志: ${LOG}"
    echo "汇总: python3 scripts/v9_perf_report.py ${LOG} --out report.md"
fi
