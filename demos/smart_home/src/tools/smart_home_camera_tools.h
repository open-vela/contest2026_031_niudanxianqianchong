/* SPDX-License-Identifier: Apache-2.0 */

#ifndef SMART_HOME_CAMERA_TOOLS_H
#define SMART_HOME_CAMERA_TOOLS_H

#include <agent.h>

/* 注册 agent 本机 board_camera 工具（status/on/off/shot）。
 * 依赖 CONFIG_SMART_HOME_CAMERA_PREVIEW 提供的摄像头服务。 */
int smart_home_camera_tools_register(agent_t *agent);

/* 无模型的板端自测入口：以包含固定 device_key 的 action 参数 直接调用同一个 handler，
 * 返回文本写到 stdout。用于云端不可用时验证工具完整执行路径
 * （含 shot 的取帧、统计与两槽滚动落盘）。返回 0 表示 handler 返回 AGENT_OK。
 */
int smart_home_camera_tool_selftest(const char *action);

#endif /* SMART_HOME_CAMERA_TOOLS_H */
