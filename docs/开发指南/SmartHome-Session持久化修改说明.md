# SmartHome Session 持久化修改说明

实现提交：`c7f1916`。目标：将 P4 标准应用的 RAM Session 改为文件存储，重启后恢复模型上下文和聊天页记录。

## 1. 修改文件

应用源码路径相对于 `demos/smart_home/src/`。

| 文件或目录 | 修改内容 |
| --- | --- |
| `packages/cAGENT/providers/storage/{files,jsonl}/` | 引入共享文件接口、JSONL 编解码和完整回合存储。 |
| `packages/cAGENT/ports/{posix,openvela}/storage/` | 接入 NuttX 文件读写、同步及临时文件替换。 |
| `agent/smart_home_session.[ch]` | 新增持久化实例，管理缓冲区、目录、保留策略和历史枚举。 |
| `agent/smart_home_agent_backend.[ch]` | 持久化开启时绑定文件 Session；关闭时保留 RAM 实现；销毁时释放对应资源。 |
| `agent/smart_home_agent.[ch]` | 请求前整理历史；提供加锁枚举接口，记录 Session 错误状态。 |
| `config/smart_home_config.h` | Session ID 改为 `home`，编码后文件名满足 LittleFS 长度限制。 |
| `ui/lvgl/smart_home_lvgl_history.c`、`ui/lvgl/smart_home_lvgl.c` | 聊天页创建后读取历史，复制文本并按时间顺序显示。 |
| `ui/lvgl/smart_home_lvgl_agent.c` | 保存失败时保留已生成回复，并单独显示错误提示。 |
| Kconfig、Makefile、P4 `smart_home/defconfig` | 编入文件存储组件，启用持久化及容量配置。 |

## 2. 保存流程

1. backend 调用 `smart_home_session_open()`，创建目录并初始化 File Store、JSONL 和应用持有的缓冲区。
2. 使用 `agent_set_session_storage()` 将文件后端绑定到 Core。
3. 每次请求前调用 `smart_home_session_prepare()`，清理未完成尾记录、淘汰旧回合，并预留新回合空间。
4. `agent_run()` 通过 `begin/append` 在缓冲区组织用户消息、助手回复、工具调用和结果。
5. `finish_complete_only()` 仅将完整回合交给 JSONL 追加并同步文件；执行失败或中止回合丢弃。

保存路径：`/data/smart_home/sessions/session-686f6d65.jsonl`。一行对应一个完整回合。

## 3. 恢复与清空

- 启动恢复：`smart_home_lvgl_chat_restore()` → `smart_home_agent_app_visit_history()` → JSONL `recent()`。
- `recent()` 按新到旧返回借用视图；UI 在回调内复制内容，读取全部成功后按旧到新显示，最后释放副本。
- 聊天页显示用户／助手文字和历史工具名称；模型上下文保留完整工具结果。恢复不发起模型请求、不重新执行工具。
- “新对话”沿用 `smart_home_agent_app_clear_session()` → `agent_session_clear_all()`，成功删除文件后清空页面。

## 4. 容量与异常处理

| 配置或问题 | 处理方式 |
| --- | --- |
| 文件保留上限 | 默认 20 个完整回合、1 MiB；按完整回合淘汰，实际保留数受两项上限共同约束。 |
| 模型历史 | `CONFIG_SMART_HOME_SESSION_HISTORY_TURNS=3`，与文件保留轮数独立。 |
| 单回合容量 | `CONFIG_SMART_HOME_SESSION_LINE_BYTES=131072`；JSONL 追加消息时预留结束标记空间，超限报错。 |
| 未完成尾记录 | 读取时忽略，下次请求前清理。 |
| 损坏的完整记录 | 读取报错并保留文件，不静默跳过。 |
| 追加短写 | 尝试截断回原长度；同步失败时记录可能已经存在，不自动重试控制。 |
| 目录不可用 | 初始化明确报错，不自动格式化。文件同步已启用，目录 fsync 未启用。 |

## 5. 验证结果

- Host/ASan/UBSan：真实文件、独立进程恢复、工具结果、滚动淘汰、清空、转义容量、半条尾记录、短写及同步失败通过。
- UI：借用文本复制、历史顺序、工具名称展示和读取失败时不显示半份历史通过；控件为 mock。
- P4：55 个目标对象检查、Make 完整固件构建及镜像校验通过；板端重启显示和突然断电恢复待验收。

测试命令及板端操作见[使用与验收说明](SmartHome-Session持久化.md)。
