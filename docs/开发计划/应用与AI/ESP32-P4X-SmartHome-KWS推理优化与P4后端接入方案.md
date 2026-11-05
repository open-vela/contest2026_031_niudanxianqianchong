# ESP32-P4X Smart Home KWS 推理优化与 P4 后端接入方案

> 状态：已实施。ESP-NN P4 内核已按 `ccf_audioevent` 架构接入
> （`third_party/esp-nn/` 80 个源文件 + `third_party/tflm-espnn-adapter/` 三个
> 内核 + `scripts/link_esp_nn.sh` 软链覆盖），mask 配置为
> `CONV2D=0xa8000000`、`DEPTHWISE=0x50000000`，覆盖 58.3% MAC。
> 板端实测 3.68 s → 2.38 s（2.54×），输出与 reference 逐位一致。
> 覆盖上限受 `IsEspNnConvOutputSelected()` 的 `tid < 32` 限制（末层 tid=33 无法选中）；
> 平台吞吐仍与 benchmark 有 5~20 倍差距。本文保留 P4 后端选型与验证方法。
>
> 适用目标：ESP32-P4 Function EV Board、OpenVela/NuttX、`wake_large` int8
> 唤醒词模型。
>
> 本文借鉴 `ccf_audioevent` 的 ESP-NN 验证方法，但不将 ESP32-S3 的 ESP-NN
> 后端直接移植到 ESP32-P4。

## 1. 结论和目标

`ccf_audioevent` 的 ESP-NN 实现是 **ESP32-S3/LX7 专用**优化后端，不是可由
Kconfig 直接迁移到 ESP32-P4 的通用库。当前 OpenVela 中
`CONFIG_TFLITEMICRO_ESP_NN` 依赖 `ARCH_CHIP_ESP32S3`；构建文件还会编译
`*_esp32s3.S` 汇编源码，并定义 `CONFIG_IDF_TARGET_ESP32S3`。P4 是 RISC-V，
因此下列做法均不可接受：

- 放宽 `TFLITEMICRO_ESP_NN` 的 Kconfig 依赖后尝试编译；
- 将 S3 的 output tensor ID 掩码复制到 `wake_large`；
- 使用 S3 的 benchmark 数据推断 P4 的吞吐或功耗；
- 把任何 int8 Conv2D 一律替换为“优化内核”。

本方案目标是把 ccf 的工程纪律迁移到 Smart Home：

```text
reference 基线 → 算子/内存剖析 → 单节点候选后端 → 字节级验证
              → 组合验证 → 正式 P95/资源报告 → KWS 常驻集成
```

P4 首版必须采用 TFLite Micro reference kernel。只有 reference 基线不能满足
20 ms 推理周期预算时，才评估 P4 专用 RISC-V 后端；该后端是新开发工作，名称不应
称为 ESP-NN，除非 Espressif 提供并验证了 P4 版本的 ESP-NN。

## 2. 可复用经验与不可复用资产

| 类别 | ccf_audioevent 已验证经验 | Smart Home/P4 的处理 |
| --- | --- | --- |
| 推理封装 | 固定 resolver、模型/输入/输出契约检查、arena 统计 | 直接复用设计；`kws_smoke` 已实现同类检查。 |
| 性能方法 | warmup 后连续 Invoke，报告 min/P50/mean/P95/max | 直接复用；固定伪特征仅用于性能回归，不用于识别率。 |
| 后端准入 | 类型、NHWC shape、batch、stride、padding、dilation、对齐、节点白名单均通过才委派 | 直接复用为 P4 后端准入规则。 |
| 正确性 | 单节点 reference/优化结果逐字节 VERIFY，不匹配时回退 reference | P4 后端的强制准入条件。 |
| 配置管理 | verify、trace、正式性能三种独立 profile | Smart Home 也必须分离，禁止用 VERIFY 时间报告性能。 |
| S3 汇编内核 | LX7 SIMD、ESP32-S3 CCOUNT、ESP-NN dispatcher | 不可复用到 P4。 |
| 节点编号/掩码 | ccf 8-class 模型的 tensor ID、shape、scratch 大小 | 不可复用；`wake_large` 必须重新枚举。 |
| 性能数据 | S3 240 MHz、特定模型的 17.591 ms Invoke（五卷积+Mean ESP-NN profile） | 仅说明流程有效，不是 P4 目标或预测值。 |

ccf 的历史记录还说明：优化五个卷积和 Mean 后，float log-mel 前端成为主要耗时。
所以即使将来 P4 推理加速成功，也必须分别记录 `frontend_us`、`invoke_us` 和
`total_us`，不能只优化 Invoke 后宣称整个 KWS 已满足实时性。

## 3. 当前 Smart Home 基线

`wake_large` 的部署契约如下：

| 项目 | 值 |
| --- | --- |
| 模型 | int8 `ds_cnn_s3`，37,408 B |
| 输入 | `int8[124 × 40 × 3]`，14,880 B |
| 输出 | int8 五分类：`wake`、`hard_neg`、`other_speech`、`background`、`silence` |
| 音频窗口 | 16 kHz、PCM16 mono、2.5 秒（40,000 samples / 约 80 KB） |
| 推理步长目标 | 20 ms |
| 当前后端 | TFLite Micro reference kernel |

`demos/smart_home/src/kws_smoke/` 的独立命令不打开 `/dev/audio/pcm_in0`，也不链接
LVGL、Agent 或网络。它必须先证明以下事实：

1. FlatBuffer schema、9 个 required operators、input/output tensor contract 均正确；
2. `AllocateTensors()` 成功，串口输出 reserve/used arena；
3. 固定 int8 特征输入下连续 Invoke 无失败，得到时延分位数和稳定输出分数；
4. P4 reference 的资源和时延基线可复现。

建议首轮串口命令：

```sh
nsh> kws_smoke --warmup 20 --repeat 100
```

日志必须保留固件 commit、模型 SHA-256
`e3adfe563ae5febe3d1fd89cc13a972f2da5f634fd6aed78661d0e1c26004ec9`、arena 参数、
warmup/repeat 和完整输出。系统时钟粒度不足以稳定测出亚毫秒差异时，应把该限制写入
报告；不得伪造 S3 CCOUNT 数据。

## 4. 建议的代码边界

```text
demos/smart_home/src/
├── kws_smoke/                         # 当前：独立模型/性能验收
│   ├── kws_smoke_main.cc
│   └── models/model.cc/.h
└── voice/                              # 后续：常驻 KWS 服务
    ├── smart_home_voice_capture.c      # ES8311、PCM ring 的唯一所有者
    ├── smart_home_voice_frontend.c     # 训练一致的 PCM → int8 feature
    ├── smart_home_voice_kws.cc         # backend 无关的 interpreter API
    ├── smart_home_voice_kws_ref.cc     # P4 首版 reference backend
    ├── smart_home_voice_kws_p4.cc      # 仅在后续 P4 后端验收后新增
    └── smart_home_voice_post.c          # 阈值、连续命中、冷却、事件发布
```

应用层只依赖稳定的 `smart_home_voice_kws_*` 接口，不应包含 `esp_nn.h` 或任何
芯片专用卷积 API。这样 ES8311、UI、后处理和 Agent 逻辑均不随推理后端改变。

推荐的后端选择是应用域专有的 Kconfig choice：

```text
CONFIG_SMART_HOME_KWS_BACKEND_REFERENCE=y       # 默认、唯一首版选项
# CONFIG_SMART_HOME_KWS_BACKEND_P4_OPT is not set
```

`P4_OPT` 在其内核、验证和性能数据都准备完成前不得出现为可选生产路径。不要修改全局
`CONFIG_TFLITEMICRO_ESP_NN` 使其“支持 P4”；该符号保留给 ESP32-S3 的既有实现。

## 5. 分阶段接入与准入条件

### P0：reference smoke 基线

工作：编译、烧录、运行 `kws_smoke`；记录加载、arena 和 100 次 Invoke 的结果。

通过条件：无 schema/算子/arena 错误，100 次 Invoke 无异常，记录完整。

决策：若 `invoke` P95 加上预计前端成本明显低于 20 ms，可跳过 P4 后端，直接进入
前端对拍和 ES8311 接入。

### P1：端到端预算与算子剖析

工作：在 reference 后端中分别测量 feature、Invoke、post-processing；必要时在 TFLM
debug profile 获取每个算子的类型、tensor ID、NHWC shape、量化参数、输入/输出/filter
地址和对齐信息。

通过条件：形成针对 **当前 `wake_large` 模型** 的算子表和内存表。不得使用 ccf
8-class 模型的 `out_t=23/24/25/26/27` 或掩码。

决策：若热点在 log-mel/FFT/Mel/filter copy，则优先优化训练一致的前端；若热点确为
int8 Conv2D/DepthwiseConv2D，才进入 P2。

### P2：P4 后端可行性评审

工作：选择可在 ESP32-P4/RISC-V 上构建、许可证明确的内核来源，或实现小范围的
P4 C/RISC-V 优化内核。必须确认其支持：int8 input/filter、int32 bias、per-channel
quantization、模型实际的 stride/padding/dilation 和所需 rounding 语义。

通过条件：内核 API、工具链、许可证、量化等价策略、scratch/对齐约束均有书面记录。

禁止事项：移植 S3 Xtensa 汇编；把 ANSI C 路径误称为硬件加速；为了性能启用非
bit-exact rounding 而不增加金标准误差验收。

### P3：薄 wrapper 与单节点 VERIFY

工作：在 P4 后端 wrapper 的 `Prepare()` 中实施以下硬性检查：

- int8 input/filter/output、int32 bias 和 per-channel quantization；
- 4-D NHWC、batch=1、非 group convolution；
- kernel、stride、padding、dilation、depth multiplier；
- input/filter/output/scratch 对齐；
- 当前模型的 output tensor ID 白名单。

每轮只开启一个节点。VERIFY 先计算 reference 输出并保存在独立 scratch，再运行优化
内核、逐字节比较；任何差异、异常或重复 Invoke 不稳定都恢复 reference 输出并撤销该节点。

通过条件：零输入、固定非零 feature、至少三组训练金标准 feature 三类输入均通过逐字节
匹配；连续 100 次无错误。

### P4：组合性能与内存回归

工作：只组合 P3 已验证节点，关闭 TRACE/VERIFY，运行不少于 100 次；分别记录 P50/P95、
arena used、静态 BSS、heap/PSRAM、输出 hash。每新增一个节点重复一次。

通过条件：性能改善覆盖额外 arena/scratch 的资源代价；输出 hash 与 reference 一致；没有
回归到 ES8311 采集、LVGL 或摄像头并发任务。

### P5：常驻 KWS 集成

工作：接入 ES8311 16 kHz PCM16、2.5 秒 ring、训练一致前端、唤醒词后处理和 UI 事件。

通过条件：实板端到端 P95（采集等待以外的 frontend + invoke + post）满足调度预算；
真实环境误唤醒和漏唤醒数据单独报告。

## 6. 内存与调度纪律

P4 优化不能只看模型 37 KB；常驻路径至少同时涉及模型、约 80 KB PCM ring、14,880 B
量化输入、TFLM arena、FFT/Mel workspace 和可能的 VERIFY/reference scratch。建议把
下列数据列入每次报告：

| 项目 | 必填记录 |
| --- | --- |
| TFLM | arena reserved/used、persistent/scratch 增量、静态 BSS |
| 音频 | PCM ring 大小、DMA buffer 数量与溢出/丢帧数 |
| 前端 | 工作区大小、平均/P95/最大耗时、feature 对拍误差 |
| 推理 | 每后端每节点/整次 Invoke 的平均/P95/最大耗时 |
| 系统 | 内部 SRAM、PSRAM、堆最小余量、任务 stack 高水位 |

VERIFY 的 reference output scratch 可显著增大 arena，且 TRACE/串口打印会污染计时；它们
只能用于 bring-up。正式性能 profile 必须关闭二者。TFLM interpreter 与 arena 由 KWS
任务独占；LVGL 线程不执行推理，音频 DMA 缓冲不直接交给 UI 或 Agent。

## 7. 最终验收矩阵

| 维度 | Reference | 单节点 VERIFY | 组合性能 | 常驻系统 |
| --- | --- | --- | --- | --- |
| 模型解析/算子 | 必须 | 必须 | 必须 | 必须 |
| 输出正确性 | 固定输出 hash | 逐字节 reference match | reference hash 一致 | 金标准 + 真实唤醒测试 |
| 时延 | Invoke P50/P95 | 不作为性能结论 | Invoke P50/P95 | frontend/invoke/total P50/P95 |
| 内存 | arena used | arena + verify scratch | arena/BSS/heap | 全系统 SRAM/PSRAM/stack |
| 稳定性 | 100 次 Invoke | 100 次/节点 | 100 次/组合 | 长时音频、UI、摄像头共存 |

## 8. 风险与停止条件

| 风险 | 处理 |
| --- | --- |
| Reference 已满足预算 | 停止后端优化，优先完成 ES8311、前端金标准和真实 KWS。 |
| 优化后端输出不匹配 | 立即回退该节点；不允许以类别相同或误差“很小”代替字节级验证。 |
| Arena/scratch 超出内部 SRAM 预算 | 先测量并重做内存布局；不能直接把实时关键 scratch 迁移 PSRAM。 |
| 前端耗时高于 Invoke | 优先对拍后优化 FFT/Mel/logf/复制；不要继续优化卷积。 |
| 无可用 P4 内核来源 | 保持 reference，改变推理步长或评估模型结构/训练侧方案；不伪造 ESP-NN 支持。 |
| 更换模型 | 清空后端白名单、tensor ID、性能结论，重新执行 P0--P4。 |

## 9. 参考资产

- `ccf_audioevent/docs/优化文档/ESP-NN移植到openvela实施指南.md`：S3 wrapper、逐节点
  VERIFY、对齐和 arena 管理经验。
- `ccf_audioevent/docs/使用与调试/tflm_benchmark算子剖析.md`：reference/正式 profile
  的性能口径。
- `apps/mlearning/tflite-micro/Kconfig` 与 `CMakeLists.txt`：当前 ESP-NN 的 S3 限定和
  源码选择边界。
- `docs/开发计划/应用与AI/ESP32-P4X-SmartHome-wake_large唤醒词模型接入方案.md`：
  `wake_large` 的模型、音频和常驻 KWS 集成方案。
