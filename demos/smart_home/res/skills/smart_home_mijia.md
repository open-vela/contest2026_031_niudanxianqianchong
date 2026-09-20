---
name: smart_home_mijia
description: Mi Home (Miloco gateway) real device control policy.
priority: 95
flags: enabled,llm_visible,summary_only
---

# 米家真实设备控制规则

设备发现：

- 网关上的真实米家设备必须先用 `miot_device_list` 获取，每台设备
  返回 did、名称、房间、在线状态与 controls 可控项列表。
- `online` 为 false 的设备（如手环）不可控制，直接告知用户设备离线。

操作纪律：

- iid 只能来自 `miot_device_list` 返回的 controls，禁止凭空猜测或
  使用记忆中的 iid；设备重新上线后 controls 可能变化，需重新查询。
- BOOL 类控件（如开关、人形侦测）：`operation="set"`，value 为 0/1。
- ENUM 类控件（如夜视、录制模式）：`operation="set"`，value 必须是
  controls 中 options 列出的合法档位值。
- ACTION 类控件（如云台）：`operation="action"`，无需 value。
- 控制提交后设备状态约 5 秒轮询后刷新，回复用户用"已提交"措辞
  （如"已为你开启人形侦测，状态稍后刷新"），不要声称已确认生效。

常见任务映射（以 controls 实际返回为准，禁止硬编码 iid）：

- 开关机/电源：找 type 为开关的 BOOL 控件（常见 prop.2.1）。
- 夜视/图像/侦测类：ENUM 控件按 options 档位设置。
- 云台/转动：ACTION 控件直接调用。
- 用户语义模糊（如"ren xin jian ce"）时，先在 controls 里找描述
  匹配的控件再执行，找不到就列出可用控件让用户选。

失败处理：

- 工具返回 error 时如实告知，绝不谎报成功。
- 同房间多台同类设备时，先列出设备让用户确认目标。
