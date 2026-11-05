# 用户长期偏好记忆

`user_memory` 保存明确自述的长期偏好，例如“我喜欢卧室空调 26 度”“我习惯睡前关客厅灯”“以后回答简短一些”。它不保存临时愿望、即时设备指令、假设、引用、第三方言论、工具输出或凭据。

工具支持三个动作：

- `remember`：保存偏好。`evidence` 必须逐字引用本轮用户原句，`value` 必须取自该句；相同 preference key 使用新值覆盖旧值。
- `list`：分页查看偏好，最多返回 16 条。
- `forget`：仅在本轮用户明确要求忘记时，按已有 key 删除。

偏好保存在 `/data/smart_home/user_memory.json`，使用临时文件、`fsync` 和原子重命名写入。文件损坏时保留原文件并停止写入，避免覆盖待恢复数据。每条记录包含稳定的 ASCII key、偏好值和用户原句证据。

每轮对话读取最近的有界摘要，作为“仅供引用的用户偏好”提供给模型。偏好不是系统指令，不是设备控制授权，也不能覆盖本轮明确指令。新对话清空聊天历史，但不会清空偏好。

首版使用单一面板 profile，不区分家庭成员。配置项：

```text
CONFIG_SMART_HOME_USER_MEMORY=y
CONFIG_SMART_HOME_USER_MEMORY_MAX_ENTRIES=16
CONFIG_SMART_HOME_USER_MEMORY_CONTEXT_BYTES=768
CONFIG_AGENT_MAX_TOOLS=24
```

主机回归入口：

```bash
python3 scripts/test_smart_home_user_memory.py
python3 scripts/test_smart_home_agent.py
python3 scripts/test_smart_home_context.py
```

这些检查验证真实文件重启恢复、去重/覆盖、写入失败、损坏文件、Core 工具调用和 Context 注入；不代表板端 LittleFS 或模型判断已经验收。
