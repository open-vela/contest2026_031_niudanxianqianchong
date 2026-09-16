# 智能家居与本地音频项目：问题复盘与 AI 辅助开发资产

> 适用范围：`openvela_smarthome/demos/smart_home`、
> `ccf_audioevent/app/audio_event` 和 P4X SmartHome 的端侧音频复用方案。
>
> 目的：沉淀可重复执行的排障方法、Skill、Workflow 和 AI 判断边界。本文只记录
> 已有代码、文档或日志可支撑的结论；“下一步”不等同于已经完成。

## 1. 总览：从一次问题到可复用资产

两个项目的共性不是“都使用了 AI”，而是都面对资源受限系统的工程问题：硬件接口、
实时线程、内存属性、网络不确定性、模型数值一致性和安全执行边界。每个问题按统一方式
收敛，才能把个人经验变成团队可用资产：

```text
现象与原始证据
    → 划分层次、提出可证伪假设
    → 最小对照实验
    → 修复已证实根因
    → 回归/性能/长稳验证
    → 固化为日志口径、测试 profile、Skill 或 Workflow
```

这里的“AI 辅助”是加快资料检索、假设生成、代码审查和测试设计；代码、原理图、ELF、
编译输出、原始串口日志和实板结果才是结论依据。

## 2. 智能家居项目的问题与解决方案

| 问题 | 证据与根因判断 | 已采取方案 | 复用价值 / 后续验证 |
| --- | --- | --- | --- |
| LLM 请求可能阻塞界面 | 云模型、TLS 和远程工具的时延不可控；若在 LVGL owner 线程运行，会冻结触摸和动画 | Console 同步执行；LVGL 将 Agent 放进独立 worker，事件复制后异步投递回 UI 线程 | 建立“UI owner 不做阻塞网络 I/O”的并发规则；检查 worker 栈、取消和 `join` 生命周期 |
| Tool registry 与 Node/MCP 动态目录并发 | Agent 运行时会遍历工具表，远程发现/断线会注册、注销工具；并发修改可导致悬空指针或旧连接误删新工具 | 用 `agent_mutex` 串行保护 Agent 运行与 mutation；以 `route_id + connection_gen` 区分连接代次 | 可迁移到门锁、网关的动态能力注册；断线、重连、迟到事件和 pending call 必须纳入回归 |
| Bridge worker 存在栈溢出风险 | 4 KiB worker 调用链曾包含约 6 KiB HTTP 请求、4 KiB API 响应及 WebSocket 快照/帧等局部缓冲 | 大缓冲改为连接级短时堆对象；分配失败返回 `503 resource_unavailable`，不以加大栈掩盖问题 | 形成“审查最大栈帧 + 大对象显式所有权”的 C/RTOS 检查项；继续用 stack coloration/高水位验证 |
| TLS EOF/超时归因不清 | DNS 和 TCP 已成功，失败位于 TLS 握手；worker 栈大小与失败并无稳定因果，聚合 `mallinfo()` 也不能代表 DMA/Wi-Fi 专用资源 | Bridge 开关、仅监听、不同 worker 行为/栈的 A/B；在请求、响应、TLS setup/handshake 前后记录堆水位 | 已修复确定的栈风险；TLS 根因仍需局域网稳定 TLS 服务和重复压力测试，不把相关性当因果 |
| 资源受限下模型/网络/UI 争用内存 | TLS、HTTP、WebSocket 快照和 Agent worker 都可能产生大块内存；Wi-Fi/DMA 又不适合随意迁到 PSRAM | 短实时路径和敏感资源留 SRAM；大 Agent 栈、请求/响应等放 PSRAM；关键路径打印区域与堆水位 | 固化 SRAM/PSRAM 放置原则；每个配置用 `ps`、heap、map 与现场日志确认，不引用单一“总内存”数字 |
| 模型输出不可信或调用风险工具 | LLM 可产生非法参数、幻觉或被 prompt 注入，UI 隐藏不是安全控制 | JSON schema、tool guard、policy callback、远程 token/allowlist、模型配置校验组成多层闸门 | 对门锁解锁、撤防等高风险能力增加原生二次确认、审计和幂等键；模型只负责提出调用 |
| Skill 全文塞入上下文导致容量失控 | 嵌入式 Agent context 有固定容量，全部 Markdown 全量注入会挤占 session 与 tool schema | Skill loader 加载受控目录；`SUMMARY_ONLY` 时仅注入名称/摘要，模型经只读 `read_skill` 按需获取全文 | 形成“摘要进 context、全文按需读、固定 ID 不接受任意路径”的知识装载方式 |
| 模拟器与真机行为不同 | Goldfish 使用 Ethernet/ADB/大分辨率，BOX-3 使用 Wi-Fi/LittleFS/LCD/触摸和紧内存 | 统一资源根 `/data/res/`，但保留各平台网络、资源镜像、分辨率和验证步骤 | 测试结论需带平台标签；模拟器验证协议和功能，真机验证资源、驱动、TLS 与交互 |

### 2.1 智能家居排障 Workflow：网络/TLS/内存

```text
1. 记录阶段日志：network → DNS → TCP → TLS → HTTP → Agent event
2. 将 UI/Bridge/模型请求拆成可单独开关的变量
3. 记录 stack、普通 heap、PSRAM pool 与最大连续块
4. 做单变量 A/B，不因一次成功/失败下结论
5. 修复能被证明的风险（如大栈帧）
6. 用稳定 LAN TLS 与压力组合复测，区分 WAN/CDN 和本机问题
```

原则：`mallinfo()` 只能证明聚合堆的部分事实；它不能单独证明 Wi-Fi、DMA、缓存或
TLS 接收路径一定健康。

### 2.2 智能家居排障 Workflow：工具接入与安全

```text
定义业务语义和状态所有者
    → 冻结 Tool 名称、JSON Schema、flags、错误码
    → 实现纯 C execute callback 与 JSON result
    → schema/guard/policy/设备层逐层校验
    → 覆盖非法参数、拒绝、超时、离线、重试和重复请求
    → 通过事件链、状态 revision、原始日志复盘
```

这条流程避免让 UI、Prompt 或远程协议直接成为设备控制事实来源。真实设备接入时，应
替换 device/gateway adapter，而不是让新硬件逻辑散落进 Agent 或页面层。

## 3. 本地音频项目的问题与解决方案

| 问题 | 证据与根因判断 | 已采取方案 | 复用价值 / 后续验证 |
| --- | --- | --- | --- |
| 采集格式与模型不匹配 | 模型约束为 16 kHz、mono、PCM16、1 秒窗口；采样率、slot、位宽或移位错误会让特征分布失真，即使设备可打开 | NuttX Audio API 配置采集格式；INMP441 32-bit 双 slot 可选 slot、右移并饱和为 mono int16；保留 PCM stats | 每次新板卡先保存并回放 PCM，核对 min/max/RMS/零值和格式，再谈模型准确率 |
| 采集阻塞、短读或设备退出 | 音频 DMA 是异步路径；设备可能返回短数据、`AUDIO_MSG_STOP/COMPLETE` 或初始化失败 | 多个 `ap_buffer_s` 入队、消息队列取回 `DEQUEUE` 后重入队；处理 `EINTR`、短读、错误与释放 | 抽象为“采集线程只保障连续数据”的模式；推理、显示、网络不可阻塞采集路径 |
| 旧前端与模型输入不一致 | 旧 13 维 MFCC 逐帧路线不能输入当前 `49×40×3` log-mel+delta 模型 | 以模型契约为部署唯一事实：30 ms 窗、20 ms 步长、512 FFT、40 Mel、49 帧及一/二阶差分 | 固化 Python/C 同一 WAV 逐值对拍；替换模型必须同时审查特征、量化、类别与 resolver |
| TFLM arena 或算子配置错误 | 模型加载成功不代表输入输出形状、量化、算子或 arena 合法 | 初始化时校验 schema、INT8 类型、元素数；使用精确 `MicroMutableOpResolver`，记录 arena 已用量 | 不使用 `AllOpsResolver`；arena 大小、对齐、内部 SRAM/PSRAM 放置以目标板实测决定 |
| ESP-NN 全量替换导致崩溃 | 初版只判断 INT8 就把 Conv/DW 全委派，真机出现 `EXCCAUSE=0023`、空地址访问；不同节点回退后崩溃位置转移 | 薄 wrapper + `out_tensor` 逐节点白名单；不满足形状/对齐条件回退 TFLM reference | “可加速”必须逐节点证明，不把第三方优化库视为任意算子的通用替代 |
| 合格节点仍不稳定 | 某 1×1 Conv 的 filter 地址 `mod 8 = 4`，不满足 ESP-NN SIMD 对齐条件 | 在 `Prepare()` 仅为该节点复制 384 B 权重到 8-byte aligned persistent buffer | 形成“检查 shape 也检查地址/布局”的优化准入表；不修改上游 ESP-NN 源码 |
| VERIFY 计时看似没有加速 | VERIFY 同时执行 reference、ESP-NN、逐字节比对和 trace，校验成本抵消优化 | 明确三阶段 `reference → verify → cycles`；性能只用关闭 TRACE/VERIFY 的正式 profile 统计 | 所有性能报告说明计时边界；禁止将 VERIFY 或端到端耗时混作纯 `Invoke()` 数据 |
| 量化 Mean 有 1 LSB 偏差 | 按数学公式得到的 multiplier 不一定等价于 TFLM reference 的定点舍入路径 | 复用 TFLM 等价的 multiplier/shift 推导和双舍入路径；逐字节比对 `match` | “数值接近”不等于部署正确；量化算子优化必须有 golden vector 或 byte-level 验证 |
| 优化后瓶颈转移 | ESP-NN 降低卷积推理后，float log-mel 前端约 60–70 ms 成为主要单项 | 用 profile 拆分 feature/infer/total；后续优先分析 FFT、Mel、`logf` 和复制 | 先测量再优化；每次近似、定点化或 SIMD 改造都要同时验收特征误差、P95 与识别指标 |
| cough 误报、knock 漏检 | 部分非目标声音有很高 cough 分数；共享冷却会压制另一类真实事件；连续两窗对短敲击不友好 | 记录 TP/FP/FN、事件级 Precision/Recall/F1 和原始 profile；把 hard negative、独立候选/冷却作为优化方向 | 不用“调高阈值”掩盖数据与策略问题；模型、门控、阈值、数据集改变时新建 profile |
| 音质问题容易误归为模型 | 削波、静音插入、爆音、底噪和周期性失真会改变输入分布 | 使用 PCM 检查：峰值/削波率、零值比例、相邻跳变、RMS/SNR、周期性 | 音频诊断在模型调参前执行；采样率、声道、样本宽度必须与真实文件一致 |

### 3.1 本地音频 Workflow：从模型到实板的验证阶梯

```text
模型 FlatBuffer / metadata
    → 黄金 INT8 特征 Invoke
    → 单段 WAV：Python 与 C 前端逐值对拍
    → 文件流：滑窗、阈值、去抖、冷却
    → NuttX 原生 Sim / ALSA 连续采集
    → 真实 I2S/PDM/codec：PCM 质量、长稳、端到端指标
```

每一级只增加一个变量。前一层未通过，不能直接把问题归因给板级硬件或模型质量。

### 3.2 本地音频 Workflow：ESP-NN 的正确性与性能

```text
冻结模型 SHA、板卡、频率、输入 pattern 和 arena 配置
    → reference：得到正确输出 hash 与算子热点
    → 单节点白名单：TRACE/VERIFY 逐字节匹配
    → 不满足约束即 reference fallback
    → 关闭 TRACE/VERIFY：warmup + 多次 Invoke 统计 P50/mean/P95/cycles
    → 回到真实 PCM：记录 feature/infer/total、事件 F1 与实时系数
```

已记录的一组 8 类模型纯 `Invoke()` 对照为：同一 ESP32-S3、240 MHz、固定 INT8
pattern、100 次测量下，reference mean `2954.965 ms`，ESP-NN mean `47.282 ms`，
输出 hash 一致。这个结果只描述模型阶段，不能替代真实音频端到端性能。

### 3.3 本地音频 Workflow：PCM 质量诊断

```text
确认采样率 / 声道 / 位宽
    → 保存原始 PCM
    → 削波、零值、跳变、RMS/SNR、周期检测
    → 映射到增益、slot/shift、DMA、buffer、时钟或重采样代码
    → 修复后再次保存 PCM，与模型输入统计对比
```

| 信号特征 | 优先检查 |
| --- | --- |
| `±32767` 占比高 | 模拟/数字增益、移位、饱和保护、混音溢出 |
| 局部零值比例高 | 短读、未初始化、buffer 长度、重采样输出大小 |
| 相邻样本突变大 | DMA/buffer 切换、并发覆盖、时钟同步 |
| RMS 高或 SNR 低 | 增益、ADC/codec 设置、接地/供电、输入格式 |
| 固定周期失真 | 50/60 Hz 干扰、定时器/中断、DMA 刷新周期 |

## 4. 已搭建或已沉淀的可复用 Skill

### 4.1 运行时 Skill：SmartHome 领域策略

SmartHome 的 Skill 是受控 Markdown 资源，不是动态执行脚本。目前资源目录包含设备控制、
安全、场景、定时器和天气等策略。启动时 loader 从 `/data/res/skills/*.md` 解析并注册，
Agent context 只注入摘要或全文；当配置为 `SUMMARY_ONLY` 时，模型只能通过只读
`read_skill(name)` 工具获取指定 Skill 全文。

| 资产 | 解决的问题 | 安全/资源边界 |
| --- | --- | --- |
| `smart_home_device_control` | 设备目录、灯光/空调等控制行为 | 只指导模型；真实控制仍必须走 Tool/schema/policy |
| `smart_home_safety` | 温度、亮度和操作安全边界 | Prompt 不能替代 policy；高风险动作还需原生确认 |
| `smart_home_scenes` | 睡眠、电影、离家、回家等场景映射 | 场景实际执行由 device service/adapter 完成 |
| `smart_home_timer` | demo timer 与提醒策略 | 当前 timer 为内存表，重启丢失且不触发真实设备 |
| `smart_home_weather` | 天气数据的解释与家居建议 | 当前 demo 天气/外部数据都应标注来源与失败状态 |
| `read_skill` Tool | 全文按需读取，降低 context 常驻占用 | 仅接受受控 Skill 名称，不提供任意文件读取能力 |

### 4.2 开发期 Skill：PCM 音频质量分析

仓库已有 [PCM Audio Quality Analysis Skill](../../../../.claude/skills/pcm-audio/SKILL.md)。它将
RAW PCM 的削波、静音插入、爆音、底噪和周期性失真映射到增益、buffer、DMA、时钟、
采样率转换等检查方向。这个 Skill 的价值是把“听起来不对”变成可量化的假设，但不会替代
原始 PCM、硬件原理图或驱动代码审查。

建议固化的使用约定：先记录 PCM 的格式，再运行分析；报告中带输入文件 hash、采样率、
声道、位宽与检测阈值；最终由采集日志和修复后的复测确认。

### 4.3 现有开发 Skill 的适用边界

下列 Skill 已在仓库中可用，可作为项目开发流程的工具箱；除 PCM Skill 外，本文不将它们
表述为本项目在所有阶段都已自动执行。

| Skill | 适合接入的节点 |
| --- | --- |
| `openvela-build` | NuttX/openvela 构建、defconfig、首个真实错误定位 |
| `kconfig-tweak` | 精确查询和修改 Kconfig，避免手工改 `.config` 漏依赖 |
| `nuttx-driver-development` | 新增/更新 I2S、codec、传感器、LCD 等驱动的分层与验证清单 |
| `memdump` | 长稳后 heap memdump、泄漏与内存差异分析 |
| `codesize` | TFLM/ESP-NN/Agent 功能引入后的 ELF、map、归档大小比较 |
| `contest-log-collector` | 将构建、串口、测试证据整理为可复查的竞赛日志包 |

## 5. 可复用的 AI 辅助开发 Workflow

### 5.1 需求到可验证任务的拆解

```text
需求/现象
  → AI 生成候选模块、接口、风险清单和测试矩阵
  → 人工对照原理图、芯片手册、既有代码、Kconfig 依赖
  → 冻结最小改动范围与验收条件
  → 实现 + 编译 + 单元/仿真 + 实板验证
  → 把命令、版本、日志、反例和结论归档
```

AI 可以加速“遗漏项发现”，例如提醒 I2S 同时涉及 pin、clock、DMA、codec、audio node、
buffer 和应用格式；但不能凭空确认目标板 GPIO、codec I2C 地址、寄存器语义或驱动可用性。

### 5.2 日志驱动的根因收敛

```text
原始日志（不覆盖）
  → AI 按阶段/错误码/时间线归类
  → 给出多个可证伪假设，而非单一结论
  → 设计最小 A/B 或 golden test
  → ELF、代码路径、heap/stack、实板输出验证
  → 仅修复已证实根因；其余假设保留为待排项
```

适用例子：TLS 的 DNS/TCP/TLS 分段日志、ESP-NN 的 `EXCCAUSE` 与 `addr2line`、
音频 `[audio_stats]` / `[profile]` 输出、Agent 的 `MODEL_REQ/RESP/TOOL_CALL` 事件链。

### 5.3 AI 代码生成与审查的最小门禁

对于 AI 生成的 C/C++、Kconfig、JSON schema 或测试脚本，至少检查：

1. API、寄存器、Kconfig 名称和文件路径是否存在，且版本匹配；
2. 错误码、失败路径、所有权、对齐、边界长度和格式字符串是否完整；
3. ISR 是否仅做短、不可阻塞工作；线程是否有栈、退出、锁和资源回收设计；
4. 采集/网络/模型是否存在无界等待、无界分配或大栈帧；
5. Tool 是否有 schema、flags、policy、超时、幂等和结构化错误；
6. 是否提供能失败的反例测试，而不只验证成功路径；
7. 变更是否经过构建、目标板或受控模拟环境验证。

### 5.4 AI 在两个项目中应当做、不能替代做的判断

| 问题 | AI 可辅助的判断 | 必须由工程证据决定 |
| --- | --- | --- |
| ESP-NN 崩溃 | 根据 call stack、算子 shape 提出对齐/资格/空指针假设 | `addr2line`、节点白名单、TRACE/VERIFY、输出逐字节一致性 |
| 性能优化 | 根据 profile 找出占比最高的 feature/Conv/Mean，设计 benchmark | 固定板卡/频率/输入后的 P50/P95/cycles；不能编造加速比 |
| PCM 异常 | 根据削波、零值、跳变、周期提出 DMA/增益/buffer 假设 | 正确格式的原始 PCM、驱动配置、示波/回放和复测 |
| TLS 失败 | 按 DNS/TCP/TLS/HTTP 分层，生成 A/B 矩阵 | 多次可复现实验、堆/栈/网络日志；不能把相关性当根因 |
| Tool/Skill 设计 | 拆分 schema、风险动作、状态与测试用例 | policy、权限、设备回报、真实业务安全规则 |
| 板级音频接入 | 列出 I2C/I2S/codec/PCM 验收清单 | 原理图、BSP、I2C 响应、时钟波形、录放音实测 |

## 6. 建议新增的轻量资产（尚未宣称已经实现）

1. **`audio-capture-contract` Workflow**：固定 PCM 格式、模型 SHA、前端参数、输入样本、
   Python/C 对拍结果和实板录音文件 hash，避免模型/前端悄然漂移。
2. **`embedded-log-triage` Skill**：读取不可修改的原始串口日志，按启动、驱动、网络、
   内存、模型、Agent 事件生成时间线、假设和所需对照实验；它只输出建议，不执行修复。
3. **`agent-tool-safety-review` Checklist**：审查每个新 Tool 的 schema、flags、policy、
   side effect、timeout、幂等、审计与离线语义。
4. **`board-audio-bringup` Workflow**：原理图确认 → I2C codec probe → I2S 时钟/引脚 →
   NuttX device node → 固定格式录放音 → PCM 质量 → 应用/KWS 接入。它可避免把
   `pcm_in0` 的存在误判为板载音频完成。

新增这些资产前，应以真实任务试运行，并根据误判、漏项和实际耗时迭代；不要只写一份
“万能提示词”就当作工程能力。

## 7. 证据索引

- SmartHome 架构与调用关系：[code-architecture.md](../../../../openvela_smarthome/demos/smart_home/docs/code-architecture.md)
- SmartHome 内存/TLS 对照排障：[bridge-memory-tls-investigation.md](../../../../openvela_smarthome/demos/smart_home/docs/bridge-memory-tls-investigation.md)
- SmartHome 真机原始日志：[真机测试.log](../../../../openvela_smarthome/logs/测试日志/真机测试.log)
- SmartHome Skill/Tool 代码：[smart_home_skills.c](../../../../openvela_smarthome/demos/smart_home/src/skills/smart_home_skills.c)、[smart_home_tools.c](../../../../openvela_smarthome/demos/smart_home/src/tools/smart_home_tools.c)
- audio_event 架构与验证阶梯：[架构文档.md](../../../../ccf_audioevent/app/audio_event/docs/架构文档.md)
- ESP-NN 问题—根因—解决记录：[ESP-NN移植问题与解决方案.md](../../../../ccf_audioevent/docs/优化文档/ESP-NN移植问题与解决方案.md)
- 8 类 reference/ESP-NN 性能口径：[renference与esp-nn对比.md](../../../../ccf_audioevent/docs/复赛目标/renference与esp-nn对比.md)
- 音频端侧指标与误报分析：[audio_event模型真机基准测试汇总.md](../../../../ccf_audioevent/docs/项目基线/audio_event模型真机基准测试汇总.md)
- PCM 分析 Skill：[SKILL.md](../../../../.claude/skills/pcm-audio/SKILL.md)
- P4X KWS 和板级音频边界：[ESP32-P4X-端侧KWS方案.md](ESP32-P4X-端侧KWS方案.md)、[esp32p4-ev-board-adaptation.md](../../硬件适配/esp32p4-ev-board-adaptation.md)
