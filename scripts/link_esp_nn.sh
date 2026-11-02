#!/usr/bin/env bash
############################################################################
# scripts/link_esp_nn.sh
#
# SPDX-License-Identifier: Apache-2.0
#
# 建立 openvela TFLM 期望的 ESP-NN 源码链接与 kernel 覆盖链接。
#
# TFLite Micro 的 Makefile/CMakeLists 把 ESP-NN 源码目录写死为
#   $(APPDIR)/mlearning/esp-nn/esp-nn
# 本仓的源码在 third_party/esp-nn/，因此需要一层链接把它接进去。
#
# 同时把 apps/mlearning/tflite-micro/kernels/esp_nn/ 下的三个 kernel 覆盖为
# 本仓 third_party/tflm-espnn-adapter/kernels/ 的版本——P4 需要的两处 scratch
# 修复只存在于我们的 adapter 中，主线内核没有。
#
# 用法：
#   ./scripts/link_esp_nn.sh           建立/更新链接（幂等）
#   ./scripts/link_esp_nn.sh --check   只检查，不改动；不正确时返回非零
#   ./scripts/link_esp_nn.sh --remove  移除本脚本建立的链接
#
# 环境变量：
#   OPENVELA_ROOT  外层 openvela 根目录（默认按本仓位置推断）
############################################################################

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OPENVELA_ROOT="${OPENVELA_ROOT:-$(cd "$REPO_ROOT/.." && pwd)}"

ESP_NN_SRC="$REPO_ROOT/third_party/esp-nn"
ADAPTER_SRC="$REPO_ROOT/third_party/tflm-espnn-adapter/kernels"

APPS_TFLM="$OPENVELA_ROOT/apps/mlearning/tflite-micro"
ESP_NN_LINK_PARENT="$OPENVELA_ROOT/apps/mlearning/esp-nn"
ESP_NN_LINK="$ESP_NN_LINK_PARENT/esp-nn"
KERNEL_LINK_DIR="$APPS_TFLM/kernels/esp_nn"

KERNELS="conv.cc depthwise_conv.cc mean.cc"

MODE="apply"
case "${1:-}" in
  --check)  MODE="check" ;;
  --remove) MODE="remove" ;;
  "")       MODE="apply" ;;
  -h|--help)
    sed -n '10,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 0
    ;;
  *)
    echo "error: unknown option '$1' (use --check / --remove / no option)" >&2
    exit 2
    ;;
esac

fail=0
errors=()

err() { errors+=("$1"); fail=1; }

############################################################################
# 前置检查
############################################################################

if [ ! -d "$OPENVELA_ROOT/apps/mlearning/tflite-micro" ]; then
  echo "error: not an openvela root: $OPENVELA_ROOT" >&2
  echo "       set OPENVELA_ROOT to the openvela checkout" >&2
  exit 2
fi

for f in LICENSE include/esp_nn.h src/common/common_functions.h; do
  if [ ! -e "$ESP_NN_SRC/$f" ]; then
    echo "error: ESP-NN source incomplete: missing $ESP_NN_SRC/$f" >&2
    exit 2
  fi
done

for f in $KERNELS; do
  if [ ! -f "$ADAPTER_SRC/$f" ]; then
    echo "error: adapter kernel missing: $ADAPTER_SRC/$f" >&2
    exit 2
  fi
done

############################################################################
# 校验函数
############################################################################

check_esp_nn_link() {
  local expected
  expected="$(realpath "$ESP_NN_SRC")"

  if [ ! -L "$ESP_NN_LINK" ]; then
    if [ -e "$ESP_NN_LINK" ]; then
      err "esp-nn 路径存在但不是链接: $ESP_NN_LINK"
    else
      err "esp-nn 链接缺失: $ESP_NN_LINK"
    fi
    return
  fi

  local actual
  actual="$(readlink -f "$ESP_NN_LINK" 2>/dev/null || true)"
  if [ "$actual" != "$expected" ]; then
    err "esp-nn 链接指向错误: $ESP_NN_LINK -> ${actual:-<broken>}（应为 $expected）"
  fi
}

check_kernel_link() {
  local name="$1"
  local link="$KERNEL_LINK_DIR/$name"
  local expected
  expected="$(realpath "$ADAPTER_SRC/$name")"

  if [ ! -L "$link" ]; then
    if [ -e "$link" ]; then
      err "kernel 不是链接（仍是主线文件）: $link"
    else
      err "kernel 链接缺失: $link"
    fi
    return
  fi

  local actual
  actual="$(readlink -f "$link" 2>/dev/null || true)"
  if [ "$actual" != "$expected" ]; then
    err "kernel 链接指向错误: $link -> ${actual:-<broken>}（应为 $expected）"
  fi
}

check_all() {
  check_esp_nn_link
  local k
  for k in $KERNELS; do
    check_kernel_link "$k"
  done
}

############################################################################
# 动作
############################################################################

if [ "$MODE" = "check" ]; then
  check_all
  if [ "$fail" -ne 0 ]; then
    echo "[esp-nn-link] 检查未通过："
    printf '  - %s\n' "${errors[@]}"
    echo "[esp-nn-link] 运行 ./scripts/link_esp_nn.sh 修复"
    exit 1
  fi
  echo "[esp-nn-link] OK: 源码链接与 $KERNELS 的 kernel 覆盖均正确"
  exit 0
fi

if [ "$MODE" = "remove" ]; then
  removed=0
  if [ -L "$ESP_NN_LINK" ]; then
    rm -f "$ESP_NN_LINK"; echo "[esp-nn-link] 已移除 $ESP_NN_LINK"; removed=1
  fi
  for k in $KERNELS; do
    if [ -L "$KERNEL_LINK_DIR/$k" ]; then
      rm -f "$KERNEL_LINK_DIR/$k"
      echo "[esp-nn-link] 已移除 $KERNEL_LINK_DIR/$k"
      removed=1
    fi
  done
  if [ "$removed" -eq 0 ]; then
    echo "[esp-nn-link] 没有本脚本建立的链接"
  else
    echo "[esp-nn-link] 注意：kernel 原文件已删除，需 git checkout 恢复后才能编译 reference 路径"
  fi
  exit 0
fi

# apply
mkdir -p "$ESP_NN_LINK_PARENT"

if [ -L "$ESP_NN_LINK" ]; then
  current="$(readlink -f "$ESP_NN_LINK" 2>/dev/null || true)"
  if [ "$current" != "$(realpath "$ESP_NN_SRC")" ]; then
    rm -f "$ESP_NN_LINK"
  fi
elif [ -e "$ESP_NN_LINK" ]; then
  echo "error: 拒绝覆盖非链接路径: $ESP_NN_LINK" >&2
  exit 1
fi

if [ ! -L "$ESP_NN_LINK" ]; then
  target="$(realpath --relative-to="$ESP_NN_LINK_PARENT" "$ESP_NN_SRC")"
  ln -s "$target" "$ESP_NN_LINK"
  echo "[esp-nn-link] 建立 $ESP_NN_LINK -> $target"
fi

# kernel 覆盖：先把主线原文件备份到 .orig，再建链接
for k in $KERNELS; do
  link="$KERNEL_LINK_DIR/$k"

  if [ -L "$link" ]; then
    current="$(readlink -f "$link" 2>/dev/null || true)"
    if [ "$current" = "$(realpath "$ADAPTER_SRC/$k")" ]; then
      continue
    fi
    rm -f "$link"
  elif [ -f "$link" ]; then
    # 保留一份原件，方便回退与 diff
    if [ ! -f "$link.orig" ]; then
      cp "$link" "$link.orig"
      echo "[esp-nn-link] 备份主线 kernel: $link.orig"
    fi
    rm -f "$link"
  fi

  target="$(realpath --relative-to="$KERNEL_LINK_DIR" "$ADAPTER_SRC/$k")"
  ln -s "$target" "$link"
  echo "[esp-nn-link] 建立 $link -> $target"
done

echo
check_all
if [ "$fail" -ne 0 ]; then
  echo "[esp-nn-link] 建立后校验失败："
  printf '  - %s\n' "${errors[@]}"
  exit 1
fi

echo
echo "[esp-nn-link] 完成。启用 CONFIG_TFLITEMICRO_ESP_NN 前请先 distclean 重建："
echo "  ./scripts/build_p4x.sh smart_home_kws -j8 distclean"
echo "  ./scripts/build_p4x.sh smart_home_kws -j8"
