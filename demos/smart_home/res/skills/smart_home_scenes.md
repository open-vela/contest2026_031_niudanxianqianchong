---
name: smart_home_scenes
description: Scene modes mapped onto real Mi Home devices.
priority: 90
flags: enabled,llm_visible,summary_only
---

# 场景模式（真实米家设备）

调用方式：`run_scene`，scene 取值 sleep / away / home / movie。
场景动作由固件按设备规格的语义锚点自动匹配执行（如夜视=
night-shot、人形追踪=human-tracking、开关=on），跨设备型号稳定，
无需也无需提供 iid。

四个场景的实际动作（在所有在线摄像头类设备上执行）：

- sleep 睡眠模式：夜视=自动、人形追踪=开。
- away 离家模式：摄像头=开、夜视=自动、人形追踪=开（看家）。
- home 回家模式：夜视=关、人形追踪=开。
- movie 观影模式：摄像头=关（隐私）。

回复规则：

- 工具返回 ok=true 时，把 report 内容转述给用户（已对哪些设备
  提交了哪些动作），并说明状态约 5 秒后刷新。
- ok=false 时如实转述 report 里的原因（如网关不可用、没有设备
  支持本场景），不要编造执行结果。
- 用户想自定义组合（如"只开夜视"）时不要用场景，改用
  miot_device_control 逐项控制。
