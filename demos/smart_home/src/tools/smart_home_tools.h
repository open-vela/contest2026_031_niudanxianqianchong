#pragma once

#include "../agent/smart_home_tool_metadata.h"
#include "../device/smart_home_device_service.h"

#include <agent.h>

int smart_home_tools_register(agent_t *agent,
                              smart_home_device_service_t *device_service);

struct smart_home_agent_app;
int smart_home_device_inventory_register(struct smart_home_agent_app *app);
int smart_home_device_inventory_prepare(struct smart_home_agent_app *app);

/* 本地工具注册 helper：新增工具文件（如 camera tools）复用它，
 * 避免每处重复构造 agent_tool_t 与 category 映射。 */
int smart_home_tool_register_local(agent_t *agent,
                                   const char *name,
                                   const char *description,
                                   const char *input_schema_json,
                                   smart_home_tool_category_t category,
                                   agent_tool_execute_fn execute,
                                   void *user_data,
                                   uint32_t flags);
