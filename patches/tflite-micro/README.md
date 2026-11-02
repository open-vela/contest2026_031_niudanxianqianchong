# patches/tflite-micro

本目录保存对外层 `apps/mlearning/tflite-micro/` 的补丁。apps 不在 contest
仓库内，直接在 apps 工作区改会游离于版本控制之外，因此统一以补丁形式固化。

## 0001-add-esp-nn-p4-backend.patch

给 TFLite Micro 的 ESP-NN 后端加上 ESP32-P4 支持。

现状：openvela 的 TFLM 已经内置了完整的 `TFLITEMICRO_ESP_NN*` Kconfig
（21 项，含 trace / 逐节点 VERIFY / tensor mask），以及 S3 的 Make 与 CMake
接线。缺的只是 P4 分支。

改动三个文件：

| 文件 | 改动 |
| --- | --- |
| `Kconfig` | `TFLITEMICRO_ESP_NN` 的 `depends on` 从 `ARCH_CHIP_ESP32S3` 放宽到 `ARCH_CHIP_ESP32S3 \|\| ARCH_CHIP_ESP32P4`，help 补充 P4 需要工具链开关 |
| `Makefile` | ESP-NN 段按 `CONFIG_ARCH_CHIP_ESP32P4` 分支：选 `*_esp32p4.c` / `*_esp32p4.S`，加 `-DCONFIG_IDF_TARGET_ESP32P4=1`；`ESP_NN_DIR` **保持 TFLM 原值**不变 |
| `CMakeLists.txt` | 同样加 P4 分支；P4 用独立的 25 个源文件列表替换 S3 的 Xtensa 列表 |

关键设计：**不改 `ESP_NN_DIR`**。TFLM 默认就是
`$(APPDIR)/mlearning/esp-nn/esp-nn`，由 contest 仓的
`scripts/link_esp_nn.sh` 把这个路径链接到 `third_party/esp-nn/`。
这样补丁只需增加分支，不引入变量覆盖，也就不会踩到 Make 展开顺序的坑。

### 前置

```sh
cd /home/arongw/openvela/contest2026_031_niudanxianqianchong
./scripts/link_esp_nn.sh          # 建立源码与 kernel 链接
./scripts/link_esp_nn.sh --check  # 确认链接正确
```

### 应用与校验

```sh
cd /home/arongw/openvela/apps
git apply --check ../../contest2026_031_niudanxianqianchong/patches/tflite-micro/0001-add-esp-nn-p4-backend.patch
git apply          ../../contest2026_031_niudanxianqianchong/patches/tflite-micro/0001-add-esp-nn-p4-backend.patch
```

### 需要同时生效的 nuttx 补丁

P4 内核用 Espressif PIE（`xespv`），整机必须切到乐鑫工具链：

- `patches/nuttx/0012-espressif-riscv-toolchain.patch`（已有，开关默认 n）
- `patches/nuttx/0015-libcxx-gcc14-builtin-trait.patch`（乐鑫工具链是 GCC 14.2，
  与 libc++ 17 的内建 trait 用法冲突）

配置侧需要：

```
CONFIG_TFLITEMICRO_ESP_NN=y
CONFIG_ESP32P4_ESPRESSIF_TOOLCHAIN=y
CONFIG_ARCH_RV_ISA_VENDOR_EXTENSIONS="xespv"
```

### kernel 覆盖

`scripts/link_esp_nn.sh` 还会把
`apps/mlearning/tflite-micro/kernels/esp_nn/{conv,depthwise_conv,mean}.cc`
替换成指向 `third_party/tflm-espnn-adapter/kernels/` 的链接，并把主线原文件
备份为 `*.orig`。

这样做的原因：P4 需要两处主线没有的修复——

1. `esp_nn_get_depthwise_conv_scratch_size_esp32p4()` 直接 `return 0`，
   而主线用 `TF_LITE_ENSURE(context, scratch_size > 0)` 断言，导致
   `AllocateTensors` 必然失败并返回 `-ENOMEM`；
2. `esp_nn_set_*_scratch_buf()` 原本被无条件调用，reference 节点会把
   kernel 全局 scratch 指针覆盖成 NULL，后续 ESP-NN 节点拿到 NULL。

两处修复都在 adapter 里，**不要**直接改 apps 工作区。

回退：

```sh
./scripts/link_esp_nn.sh --remove
cd /home/arongw/openvela/apps
git checkout -- mlearning/tflite-micro/kernels/esp_nn/
rm -f mlearning/tflite-micro/kernels/esp_nn/*.orig
```
