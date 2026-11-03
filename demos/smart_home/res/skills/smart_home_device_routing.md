---
name: smart_home_device_routing
description: Identify device origin and exact capability; clarify ambiguous targets or actions before control.
priority: 100
flags: enabled,llm_visible,summary_only
---

# 设备来源与操作选择

1. 来源或目标不明确时，先用 `get_device_inventory` 查询相关候选；摄像头用 `kind="camera"`，可按 `source` 缩小范围。
2. board 是本机硬件，demo 是演示状态，mijia 是米家，node 是节点。MCP 是接入通道，不证明其背后设备的实际来源。Node/MCP 清单仅表示工具已注册，不证明设备在线。
3. 清单来自缓存；控制前通过对应来源的查询工具核对当前设备及能力。清单截断时不能假定未列出的能力不存在，也不能猜测标识。
4. 目标有多个或动作含糊时，先询问；来源、目标和动作明确且唯一时直接执行。可用“米家客厅摄像机，还是本机摄像头？”澄清来源。
5. “测试云台”不等于校准、方向转动或巡航。先说明实际支持的动作并询问，禁止用开机或拍照代替。找不到对应能力时如实说明，不试调用其他工具。
6. `board_camera` 仅接受 `device_key="board:camera:0"`，没有云台。`shot` 只对已运行的本机摄像头拍照；不能为探测意图自动开启摄像头。
7. 演示灯和空调用 `get_device_inventory` 返回的确切 `device_id`、房间和对应工具；不能宣称改变了真实设备。
8. 米家先用 `miot_device_list(did=...)` 获取具体能力，再用返回的 `device_key`、did、iid 和合法参数调用 `miot_device_control`。Node/MCP 先用标记为 read_only 的工具查询；不能把控制工具当成探测工具。控制时使用实际注册工具声明的目标与参数，不跨来源替代。
9. 队列接受、网关接受和设备状态确认分别报告。响应丢失或超时应说结果未确认，不能说设备一定未执行，也不能自动重发有副作用的动作。
