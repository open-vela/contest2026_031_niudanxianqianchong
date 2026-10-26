# SmartHome cAGENT V2 移植过程

分支：`feat/cagent-v2-migration`。代码记录截至：`6397f6b`。
范围：P4 标准 `smart_home`；使用 V2 原生接口，不增加 V1 兼容层。
下文应用源码路径均相对于 `demos/smart_home/src/`。

## 1. 替换 Agent 包（8d344ee）

- 文件：`packages/cAGENT/`、包构建入口及示例配置。
- 修改：以原 `packages/cAgent2/` 的本地 V2 快照替换旧包，保留 `packages/cAGENT/` 路径和 `CONFIG_CAGENT` 总开关；来源记录在 `V2_SNAPSHOT_COMMIT`。
- 问题与处理：V1 API、配置和文档不再适用，移除旧实现，随后分层迁移调用方；保留 MIT 和派生 TLS 的 Apache-2.0 归属声明。此提交仅为阶段快照。

## 2. Agent 生命周期与模型（d0025d6）

- 文件：`agent/smart_home_agent.[ch]`、新增 `agent/smart_home_agent_backend.[ch]`，以及 Kconfig/Make 构建入口。
- 接口：使用 `agent_init → 注册组件 → agent_start → agent_run → agent_destroy`；backend 管理 Core、OpenAI Provider、Transport/TLS 和工作区。
- 问题与处理：V2 借用配置字符串及缓冲区，改由应用持续持有；运行、配置切换和销毁共用互斥锁；新模型先验证再替换，失败保留原后端；缺密钥仍可进入设置页。
- 配置：Core 与调用方统一通过 `AGENT_BUILD_CONFIG_HEADER` 加载 NuttX 容量配置，避免结构体布局不一致。

## 3. 工具与权限（c67f8f9，6397f6b 补全边界处理）

- 文件：`tools/smart_home_tools.c`、新增 `tools/smart_home_tool_io.h`、`miloco/smart_home_miloco_tools.c`、`agent/smart_home_tool_metadata.h`。
- 接口：工具回调改用 `agent_tool_context_t` 接收参数、`agent_text_sink_t` 输出；显式设置 `AGENT_TOOL_READ_ONLY` / `AGENT_TOOL_SIDE_EFFECT`，由应用映射 UI 分组和类别。
- 问题与处理：参数视图不保证以 NUL 结尾，改为有界 JSON 解析；拒绝重复键、尾随内容、非法整数、非法可选字段和转义 NUL，校验失败不执行控制。
- 权限与结果：策略拒绝时不调用工具；米家控制回执表示已入队；设备列表超限时返回 `truncated` 和 `total_devices`，避免误报完整结果。

## 4. 设备上下文与 Skill（0985852）

- 文件：`device/smart_home_device*`、`skills/smart_home_skill_loader.[ch]`、`skills/smart_home_skills.c`、`skills/smart_home_scene_catalog.c`。
- 接口：通过 `agent_context_provider_t` 注册 `AGENT_CONTEXT_REFERENCE`；设备服务加锁生成快照，输出受字节上限约束。
- 修改：Skill 改用 V2 的 `name/description/content/priority/required`；应用持有摘要及全文，`read_skill` 按需返回正文。
- 问题与处理：主目录文件格式错误时不再回退；现有米家场景 Skill 不含旧虚拟设备 catalog，缺省时跳过注册，格式错误仍报错。

## 5. RAM 多轮会话（edfa21d）

- 文件：`packages/cAGENT/providers/storage/ram/`、Agent backend 和会话配置。
- 接口：接入 `agent_set_session_storage()`，增加空闲时使用的 `agent_session_ram_trim()` 和应用侧加锁清空入口。
- 修改：应用持有会话缓冲，默认保留三个历史完整回合；模型切换保留历史，重启清空。
- 问题与处理：按完整回合淘汰，避免拆散工具调用与结果；失败或中止回合不进入后续上下文，容量错误明确返回并允许回收后继续运行。

## 6. UI 与事件（24436ed，6397f6b 补全聊天状态）

- 文件：`ui/smart_home_ui.c`、`ui/smart_home_ui_event.h`、`ui/lvgl/smart_home_lvgl*.c`、`smart_home_memory.[ch]`。
- 接口：映射 `TURN_BEGIN/END`、`MODEL_BEGIN/END`、`TOOL_BEGIN/END`；事件中的借用视图复制后入队，LVGL 操作集中在 UI 线程。
- 问题与处理：工具枚举回调再次读取权限会递归加锁，改为消费枚举快照中的 `access_allowed`；提前分配完成消息，避免请求结束后因内存不足卡在忙碌状态。
- 显示与交互：错误统一展示一次；截断回复显示部分正文和提示；缺少结束事件的工具标为“未确认”；“新对话”同步清空模型历史与可见轨迹，忙碌或清空失败时保留原状态。
- 内存：应用接管分配及 PSRAM 辅助函数，移除标准路径对旧 `ov_mem_*` 运行时接口的依赖。

## 7. 标准配置与完整固件（6397f6b）

- 文件：`board/esp32p4/esp32p4-function-ev-board/configs/smart_home/defconfig`，配置、密钥、网络及米家服务相关文件。
- 修改：启用 V2 OpenAI、RAM Session 和 OpenVela 端口；统一 `agent/error.h` 等接口，补齐头文件及声明。
- 容量：16 个工具、48 条投影消息、8 KiB 上下文、64 KiB scratch、64 KiB 模型请求缓冲、96 KiB Core 工作区；RAM 会话载荷 64 KiB。
- 问题与处理：旧生成配置残留，重新配置后构建；P4 对象检查改为与完整固件一致的 `ilp32` ABI；标准联测增加鉴权失败、请求容量不足与恢复场景。

## 验证结果

- Host：`scripts/test_smart_home_{agent,tools,context,ui,standard}.py` 五个脚本均通过 ASan/UBSan；HTTP/TLS、米家网关和部分 UI 控件使用 mock。
- 回归：现有 CMake/CTest 五项业务测试通过；`test_smart_home_agent.py --p4` 的 47 个目标对象交叉编译通过。
- 完整固件：Make `smart_home` 编译链接及镜像校验通过，镜像 1,409,752 字节；构建仍有既有警告。
- 标准 Host 联测：八轮、每轮四个工具调用，请求峰值 35,798 字节，scratch 峰值 40,382 字节；这些数值不代表板端内存峰值。

完整构建命令（在 openvela 根目录执行）：

```bash
source myenv/bin/activate
./build.sh contest2026_031_niudanxianqianchong/board/esp32p4/esp32p4-function-ev-board/configs/smart_home -j4
```

构建产物和验证清单：openvela 根目录下 `out/cagent-v2-smart-home-6397f6b/`。

待完成：烧录、真机与真实云端/米家网关验收；语音及 Node/MCP/App Bridge 等扩展适配。CMake 包目标尚未完成，当前固件构建使用 Make。
