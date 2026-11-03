# SmartHome Session 持久化

实现文件、接口与流程见[修改说明](SmartHome-Session持久化修改说明.md)。

P4 标准 `smart_home` 启用 `CONFIG_SMART_HOME_SESSION_PERSIST`，保存于：

```text
/data/smart_home/sessions/session-686f6d65.jsonl
```

- 保存完整回合的用户消息、助手回复、工具调用和结果；失败或中止回合不保存。
- 重启后聊天页按时间顺序恢复文字及工具调用名称；恢复不会调用模型或执行设备控制。
- 模型默认使用最近三个完整回合；文件最多保留二十回合、1 MiB，容量不足时按完整回合淘汰旧记录。
- “新对话”同时删除文件记录和当前聊天内容，重启后不会恢复旧对话。
- 单回合编码缓冲为 128 KiB；超限明确报错，不截断为成功记录。
- 未挂载 `/data` 或无法创建目录时初始化报错；不会自动格式化资源分区。

## 故障处理

- 未写完的尾记录不显示，下次请求前清理；损坏的完整记录报错并保留原文件。
- 保存失败时显示错误；已有回复继续显示，不自动重试设备控制。
- 使用文件同步及同目录临时文件替换；目录 fsync 未启用，突然断电的实际恢复能力仍需板端验收。

## 验证

在竞赛仓目录执行：

```bash
source ../myenv/bin/activate
python scripts/test_smart_home_session.py
python scripts/test_smart_home_standard.py --persistent
python scripts/test_smart_home_ui.py
```

Host 已覆盖真实文件、独立进程恢复、完整工具结果、滚动淘汰、清空后重建、半条尾记录、短写和同步失败；HTTP/TLS、网关及 UI 控件使用 mock。

板端验收：完成两轮对话后重启，进入聊天页确认历史仍在；点击“新对话”后再次重启确认为空；核对恢复期间没有再次执行设备控制。烧录与断电测试尚未执行。
