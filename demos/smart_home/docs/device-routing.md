# 设备来源与澄清规则

来源：`board` 本机硬件、`demo` 演示状态、`mijia` 米家、`node` 节点。`mcp` 是接入通道，不证明设备真实来源或在线状态。

`get_device_inventory` 只读缓存，可按 `source`、`kind` 筛选。返回 `truncated=true` 时应缩小范围，不能猜测遗漏设备或能力。Node/MCP 返回本轮开始时已注册的工具，具体设备与参数仍需按对应工具查询。

米家能力包含控件类型、枚举选项、数值范围和有效缓存值。单台设备能力超过输出预算时，按 `next_control_offset` 配合同一 did 继续查询。

控制目标：

- `board_camera` 必须指定 `device_key="board:camera:0"`，仅支持 status/on/off/shot；没有云台，shot 不会开机。
- `miot_device_control` 必须使用清单返回的 `device_key="milo:<did>"`、did、iid 和合法值；提交前核对缓存设备在线及能力，执行时再次校验。
- `set_light`、`set_ac` 必须指定发现结果中的演示 `device_id` 和房间，不再选择同房间第一台设备。

系统提示词常驻核心规则，`smart_home_device_routing` 提供详细流程。模糊目标或动作先查询、再询问；明确且唯一的请求无需反复确认。校准、方向转动、巡航不可互相替代。

## 部署

重新构建 SmartHome 固件，并同步 `res/skills/` 到设备 `/data/res/skills/` 后重启应用。新增路由 skill，更新 device_control、mijia、safety 三份规则。仅更新 nuttx.bin 不会更新 LittleFS 中的 skill 文件。更新资源时保留设备配置与 secrets。

## 板端验收

开始新对话，观察 `[smart_home_tool]` 中的来源、实际工具名和结果码：

| 输入 | 预期 |
|---|---|
| 测试摄像头云台 | 查询摄像头候选；澄清校准或其他动作，不启动本机采集 |
| 打开摄像头（有多个候选） | 询问具体设备及来源 |
| 校准客厅米家摄像机云台 | 查询米家确切能力，仅提交对应校准动作 |
| 本机摄像头支持云台吗 | 只读查询，说明不支持，无 CSI stream on |
| 本机摄像头关闭时拍照 | 提示未运行，不自动开启 |
| 同房间两台演示灯，打开灯 | 询问具体设备；选定后按 ID 控制 |

主机测试覆盖参数、来源校验与注册流程；模型澄清效果、真实设备控制及界面响应需板端验证。UI 长阻塞和 PPA 性能不属于本次修改。
