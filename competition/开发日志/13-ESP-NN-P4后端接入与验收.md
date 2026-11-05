# ESP-NN P4 后端接入与验收（ESP32-P4X · TFLM INT8 算子）

| 元信息 | 内容 |
| --- | --- |
| 适配对象 | ESP-NN INT8 Conv2D / DepthwiseConv2D 的 ESP32-P4 PIE 优化内核，经 TFLite Micro 后端接入 |
| 适配方式 | 上游源码 `third_party/esp-nn/` + 版本化 adapter `third_party/tflm-espnn-adapter/kernels/` + 软链脚本 `scripts/link_esp_nn.sh`；工具链 `CONFIG_ESP32P4_ESPRESSIF_TOOLCHAIN` |
| 当前真机状态 | **部分通过**：PIE 内核已编入固件并生效，`kws_smoke` 50 次推理 p50=2.38 s，与历史基线独立吻合；**逐算子数值比对未执行** |
| 关键代码入口 | `third_party/esp-nn/src/convolution/esp_nn_{conv,depthwise_conv}_esp32p4.c`、`third_party/tflm-espnn-adapter/kernels/{conv,depthwise_conv}.cc`、配置 `configs/smart_home_kws` |
| 验证日期与固件 | 2026-11-04/05，`contest2026` @ `37bb1f0`，`configs/smart_home_kws` |

## 一、适配背景与目标

ESP32-P4 有一套私有 RISC-V 指令扩展 `xespv`（PIE，Packed-Int8 Extension），
提供 `esp.vld.128.ip`、`esp.vmulas.s8.xacc`、`esp.st.qacc.*` 等 128 位向量指令，
专门用于 INT8 卷积的乘累加。

ESP-NN 是乐鑫为这些指令写的算子库。本项目接入 P4 内核对 KWS（关键词唤醒）模型加速：

| 后端 | 实测单次推理 | 说明 |
| --- | --- | --- |
| reference（纯 C） | 3.68 s | TFLM 内置参考实现 |
| esp-nn（PIE） | 2.38 s | 本域适配目标，覆盖 58.3% MAC |

P4 内核覆盖 Conv2D 与 DepthwiseConv2D 两类算子，其余算子仍走 TFLM 参考实现。

## 二、接入结构

### 2.1 三处组件

| 组件 | 位置 | 作用 |
| --- | --- | --- |
| 上游源码 | `third_party/esp-nn/` | 44 个 `.c`，含 P4 PIE 内核 |
| TFLM adapter | `third_party/tflm-espnn-adapter/kernels/{conv,depthwise_conv,mean}.cc` | 版本化 kernel，含两处 P4 scratch 修复 |
| 软链脚本 | `scripts/link_esp_nn.sh` | 把 `apps/mlearning/esp-nn/esp-nn` 与三个 kernel 指向本仓 |

TFLM 的 Makefile 把 ESP-NN 源码目录写死为 `$(APPDIR)/mlearning/esp-nn/esp-nn`，
因此必须靠软链把本仓源码接进去。脚本支持三种模式：

```sh
./scripts/link_esp_nn.sh           # 建立/更新链接（幂等）
./scripts/link_esp_nn.sh --check   # 只检查，不改动
./scripts/link_esp_nn.sh --remove  # 移除链接
```

构建前必须 `--check`，否则会**静默使用主线 kernel**（缺少 P4 scratch 修复）。

### 2.2 两处 P4 scratch 修复

主线 kernel 缺这两处修复，是 P4 上实测发现的：

| 问题 | 现象 | 修复 |
| --- | --- | --- |
| `esp_nn_get_depthwise_conv_scratch_size_esp32p4()` 返回 0，而 TFLM 用 `TF_LITE_ENSURE(scratch_size > 0)` | `AllocateTensors` 必然返回 `-ENOMEM` | 仅在 `scratch_size > 0` 时申请 |
| `esp_nn_set_conv/depthwise_conv_scratch_buf()` 被无条件调用 | reference 节点把全局 scratch 指针覆盖成 NULL，后续 ESP-NN 节点带 NULL 运行 | 仅在 `data.use_esp_nn` 时设置 |

注意：DW 的 scratch 查询返回 0，**0-byte scratch 是合法值**而非分配失败。

### 2.3 算子选择机制

不是所有 Conv 都走 esp-nn，由 output tensor ID 掩码控制：

```text
CONFIG_TFLITEMICRO_ESP_NN_CONV2D_OUTPUT_TENSOR_MASK=0xa8000000
CONFIG_TFLITEMICRO_ESP_NN_DEPTHWISE_CONV2D_OUTPUT_TENSOR_MASK=0x50000000
```

判定逻辑（`kernels/esp_nn/conv.cc:71`）：

```c
static bool IsEspNnConvOutputSelected(int output_tensor_id) {
  if (output_tensor_id == CONFIG_TFLITEMICRO_ESP_NN_CONV2D_OUTPUT_TENSOR) return true;
  if (output_tensor_id < 0 || output_tensor_id >= 32) return false;
  return (MASK & (1u << output_tensor_id)) != 0;
}
```

**`tid >= 32` 的算子一律不选中**，这是覆盖上限 58.3% 的原因，不是缺陷。

## 三、工具链

### 3.1 为什么必须用乐鑫工具链

PIE 指令只有乐鑫的 binutils 认识。实测：

```sh
# openvela 自带 xPack riscv-none-elf 13.4.0（binutils 2.43.1 原版）
$ riscv-none-elf-gcc -march=rv32imac_zicsr_zifencei_xespv -mabi=ilp32 -c t.c
Error: x ISA extension `xespv' must be set with the versions

# 补上版本号（ELF 标签里就是 xespv2p2）
$ riscv-none-elf-gcc -march=rv32imac_zicsr_zifencei_xespv2p2 -mabi=ilp32 -c t.c
Error: unrecognized opcode `esp.vld.128.ip q0,a0,16'
Error: unrecognized opcode `esp.zero.xacc'
```

第一条报错有误导性，补版本号后仍然不认识指令。**xPack 无法编码 PIE，无回旋余地。**

### 3.2 工具链来源

`riscv32-esp-elf` 由 `scripts/setup_toolchain.sh` 安装到 `prebuilts/`
（`prebuilts/*` 被 `.gitignore` 排除，只有 README 入库）：

```sh
./scripts/setup_toolchain.sh          # 下载、校验、解压、裁剪
./scripts/setup_toolchain.sh --check  # 只校验现有安装
```

校验输出：

```text
OK:   compiler 14.2.0 (riscv32-esp-elf-gcc (crosstool-NG esp-14.2.0_20260121) 14.2.0)
OK:   multilib rv32imac_zicsr_zifencei/ilp32
OK:   libgcc / libm / libsupc++ / libstdc++ present
OK:   xespv PIE opcodes assemble
installation is usable
```

裁剪后约 430 MB（原始 2.15 GB），只保留 `rv32imac_zicsr_zifencei/ilp32` 一套 multilib。

**注意 multilib 是 soft-float（`ilp32` 而非 `ilp32f`）**，这是该工具链的预期配置，
不是缺陷：P4 固件整体用 `-mabi=ilp32` 构建。

`CONFIG_ESP32P4_ESPRESSIF_TOOLCHAIN` 会切换整个固件的 `CROSSDEV`，与 ESP32-S3
使用 `xtensa-esp32s3-elf` 同理。

## 四、构建验证

### 4.1 配置

唯一启用 esp-nn 的配置是 `smart_home_kws`：

```text
CONFIG_TFLITEMICRO_ESP_NN=y
CONFIG_TFLITEMICRO_ESP_NN_CONV2D_OUTPUT_TENSOR=-1
CONFIG_TFLITEMICRO_ESP_NN_CONV2D_OUTPUT_TENSOR_MASK=0xa8000000
CONFIG_TFLITEMICRO_ESP_NN_DEPTHWISE_CONV2D_OUTPUT_TENSOR_MASK=0x50000000
CONFIG_ESP32P4_ESPRESSIF_TOOLCHAIN=y
CONFIG_ARCH_RV_ISA_VENDOR_EXTENSIONS="xespv"
```

### 4.2 PIE 指令确实进入固件

```sh
$ objdump -d nuttx/nuttx | grep -c "esp\."
58
```

按函数分布：

| 指令数 | 所在函数 |
| --- | --- |
| 22 | `esp_nn_conv_s8_im2col` |
| 11 | `depthwise_conv_s8_ch1_pie` |
| 9 | `conv_1x1_batch16` |
| 3 | `esp_nn_conv_s8_1x1` |
| 2 | `esp_nn_conv_s8_padded` |
| 1 | `esp_nn_set_conv_scratch_buf_esp32p4` |
| 10 | 循环标签（`.loop16_*`） |

这证明 PIE 内核真的被链接进固件，不是"配置打开了但代码没编进去"。

### 4.3 kernel 覆盖生效

```sh
$ readlink -f apps/mlearning/tflite-micro/kernels/esp_nn/conv.cc
.../third_party/tflm-espnn-adapter/kernels/conv.cc
```

目标文件引用 P4 内核符号：

```sh
$ nm kernels/esp_nn/conv.cc.*.o | grep esp_nn_conv_s8_esp32p4
1
```

## 五、板端验证结果

```text
nsh> kws_smoke
[kws_smoke] arena_reserved=393216 address=0x4825c780 backend=esp-nn \
            conv_mask=0xa8000000 dw_mask=0x50000000
[kws_smoke] stage=allocated arena_used=210452
[kws_smoke] stage=params scale=177180 zp=-20
[kws_smoke] stage=features ready
[kws_smoke] warmup 1 took 2380000 us
[kws_smoke] warmup 2 took 2380000 us
[kws_smoke] warmup 3 took 2380000 us
[kws_smoke] warmup 4 took 2380000 us
[kws_smoke] warmup 5 took 2380000 us
[kws_smoke] invoke_us warmup=5 repeat=50 min=2380000 p50=2380000 \
            mean=2382200 p95=2390000 max=2390000
[kws_smoke] output wake=0.2305 hard_neg=0.7617 other_speech=0.0000 \
            background=0.0078 silence=0.0000
```

同一固件复跑一次：

```text
[kws_smoke] invoke_us warmup=5 repeat=50 min=2380000 p50=2380000 \
            mean=2382800 p95=2390000 max=2390000
[kws_smoke] output wake=0.2305 hard_neg=0.7617 other_speech=0.0000 \
            background=0.0078 silence=0.0000
```

判据：

| 项 | 观察 | 说明 |
| --- | --- | --- |
| 后端 | `backend=esp-nn` | 编译期宏回显 |
| 稳定性 | 5 次 warmup 全部 2380000 us | 确定性定点路径，无动态抖动 |
| 分布 | p50=2.38 s，p95=2.39 s，抖动 <1% | 同上 |
| 可重复 | 两次运行 p50 相同，输出概率逐位相同 | 无随机性 |
| 交叉验证 | 2.38 s 与仓库历史记录「3.68 s → 2.38 s」独立吻合 | **主要证据** |
| 数值自洽 | 五分类概率和 = 1.0000（0.2305+0.7617+0+0.0078+0） | 链路未断裂 |

## 六、证据强度说明（重要）

必须如实记录本次验证的**边界**：

### 6.1 已有的证据

- PIE 指令在固件中（58 条，`objdump` 计数）；
- kernel 覆盖生效（软链 + 符号引用）；
- 推理耗时与历史基线独立吻合；
- 输出概率自洽且可重复。

### 6.2 缺失的证据

**没有做逐算子数值比对。**

`backend=esp-nn` 来自编译期宏回显（`kws_infer.cc:32`）：

```c
#if defined(CONFIG_TFLITEMICRO_ESP_NN)
#  define KWS_INFER_BUILD_BACKEND KWS_INFER_BACKEND_ESPNN
#endif
```

它是**恒真**的，只能证明"固件编入了 ESPNN 后端"，不能证明"运行时每个算子都真正
走了 PIE 内核、且结果与参考实现一致"。

另一个佐证缺失：`CONFIG_TFLITEMICRO_ESP_NN_TRACE=y` 本应打印
`[espnn-trace] Conv2D ... selected=1`，但本次串口 `grep -c "espnn-trace"` 为 0，
固件 `strings` 中也找不到该字符串。trace 分支未产生可观测输出，原因未查清
（kernel 的 `.cc` 没有 include `nuttx/config.h`，可能影响宏可见性）。

因此本次把**「与历史 2.38 s 基线吻合」**作为主要证据，把 trace 缺失与
逐算子未比对记为待补项，而**不宣称 esp-nn 的数值正确性已验证**。

## 七、如何补齐（建议的下一步）

TFLM 已内置两个验证开关，可把"间接证据"升级为"逐字节一致"：

```
CONFIG_TFLITEMICRO_ESP_NN_CONV2D_VERIFY
CONFIG_TFLITEMICRO_ESP_NN_DEPTHWISE_CONV2D_VERIFY
```

开启后，kernel 会在板端**同时跑 reference 与 esp-nn 两套实现**、逐字节比对，
不一致时用 reference 结果覆盖，并打印差异。

配合关闭 esp-nn 的对照构建（reference 后端），可以同时拿到：

1. 数值等价性证明（verify 开关）；
2. 真实的加速比（同一模型、同一固件条件下的对照测速）。

这两项是当前最主要的缺口。

## 八、未覆盖项

- 逐算子数值比对（见 §6.2、§7）；
- 与 reference 后端的同镜像对照测速（2.38 s vs 3.68 s 取自历史记录，非本次同条件测量）；
- `tid >= 32` 之外算子的覆盖扩展；
- 多 interpreter / 双核调用（Conv 使用进程级静态 scratch 指针，E2 统一锁未实现前不得并发）；
- TFLM adapter 层以外直接调用 ESP-NN API 的场景。

## 九、复现步骤

```sh
cd /home/arongw/openvela
./contest2026_031_niudanxianqianchong/scripts/link_esp_nn.sh --check
./contest2026_031_niudanxianqianchong/scripts/setup_toolchain.sh --check
./contest2026_031_niudanxianqianchong/scripts/build_p4x.sh smart_home_kws -j2
myenv/bin/esptool --chip esp32p4 --port /dev/ttyACM0 --baud 921600 \
  write-flash -fs 16MB -fm dio -ff 80m 0x2000 nuttx/nuttx.bin
```

板端：

```text
kws_smoke
```

判据：`backend=esp-nn`，`invoke_us` 的 `p50` 在 2.38 s 量级。

## 十、结论

ESP-NN 的 P4 PIE 后端已完成接入并在实板运行：58 条 PIE 指令编入固件，
kernel 覆盖生效，50 次推理 p50=2.38 s 且可重复，与历史基线独立吻合。

但本次**未做逐算子数值比对**，因此结论限定为"接入生效、性能复现"，
不宣称"数值正确性已验证"。补齐计划见 §7。
