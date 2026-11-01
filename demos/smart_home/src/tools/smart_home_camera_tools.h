/* SPDX-License-Identifier: Apache-2.0 */

#ifndef SMART_HOME_CAMERA_TOOLS_H
#define SMART_HOME_CAMERA_TOOLS_H

#include <agent.h>

/* 注册 agent 本地 camera 工具（status/on/off/shot）。
 * 依赖 CONFIG_SMART_HOME_CAMERA_PREVIEW 提供的摄像头服务。 */
int smart_home_camera_tools_register(agent_t *agent);

#endif /* SMART_HOME_CAMERA_TOOLS_H */
