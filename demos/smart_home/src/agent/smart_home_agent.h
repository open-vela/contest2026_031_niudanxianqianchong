#pragma once

#include "../device/smart_home_device_service.h"
#include "../net/smart_home_network.h"
#include "../skills/smart_home_skill_loader.h"
#include "smart_home_agent_backend.h"
#include "smart_home_tool_metadata.h"
#ifdef CONFIG_SMART_HOME_USER_MEMORY
#include "../memory/smart_home_user_memory.h"
#endif
#ifdef CONFIG_FEATURE_SYSTEM_SMARTHOME
#include "../quickapp/smart_home_quickapp_provider.h"
#include "../quickapp/smart_home_quickapp_ipc_server.h"
#endif

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#include <agent.h>

#define SMART_HOME_STATUS_UNKNOWN 1

typedef struct {
    smart_home_network_status_t network_status;
    int agent_status;
    int skills_status;
    int tools_status;
    int model_status;
    int session_status;
    int node_gateway_status;
    int mcp_bridge_status;
    size_t skills_loaded;
    char last_error[96];
} smart_home_system_status_t;

/* 本地工具的用户访问策略；底层工具是否可用仍由 cAGENT enabled 表示。 */
#define SMART_HOME_LOCAL_TOOL_ACCESS_MAX AGENT_MAX_TOOLS
#define SMART_HOME_LOCAL_TOOL_NAME_SIZE  64u

typedef struct {
    char name[SMART_HOME_LOCAL_TOOL_NAME_SIZE];
    int allowed;
} smart_home_local_tool_access_t;

#ifdef CONFIG_SMART_HOME_NODE_GATEWAY
typedef struct smart_home_node_gateway smart_home_node_gateway_t;
#endif
#ifdef CONFIG_SMART_HOME_MCP_BRIDGE
typedef struct smart_home_mcp_bridge smart_home_mcp_bridge_t;
#endif
#ifdef CONFIG_SMART_HOME_APP_BRIDGE
typedef struct smart_home_app_bridge smart_home_app_bridge_t;
typedef struct smart_home_agent_run_service smart_home_agent_run_service_t;
#endif

typedef struct smart_home_agent_app {
    agent_t *agent;
#ifdef CONFIG_SMART_HOME_USER_MEMORY
    smart_home_user_memory_t *user_memory;
#endif
    smart_home_agent_backend_t *backend;
    smart_home_state_t device_state;
    smart_home_device_service_t device_service;
#ifdef CONFIG_FEATURE_SYSTEM_SMARTHOME
    smart_home_quickapp_provider_t quickapp_provider;
    smart_home_quickapp_ipc_server_t quickapp_ipc_server;
#endif
    smart_home_skill_store_t skill_store;
    smart_home_model_config_t model_config;
    smart_home_system_status_t system_status;
    smart_home_local_tool_access_t
        local_tool_access[SMART_HOME_LOCAL_TOOL_ACCESS_MAX];
    size_t local_tool_access_count;
    struct {
        char name[AGENT_MAX_NAME_BYTES + 1u];
        uint16_t group;
        int read_only;
    } remote_routes[AGENT_MAX_TOOLS];
    size_t remote_route_count;
    uint64_t run_start_ms;
    agent_event_callback_t observer;
    void *observer_user_data;
    pthread_mutex_t agent_mutex;
    int agent_mutex_initialized;
#ifdef CONFIG_SMART_HOME_NODE_GATEWAY
    smart_home_node_gateway_t *node_gateway;
#endif
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
    struct smart_home_miloco *miloco;
#endif
#ifdef CONFIG_SMART_HOME_MCP_BRIDGE
    smart_home_mcp_bridge_t *mcp_bridge;
#endif
#ifdef CONFIG_SMART_HOME_APP_BRIDGE
    smart_home_app_bridge_t *app_bridge;
    smart_home_agent_run_service_t *run_service;
#endif
} smart_home_agent_app_t;

#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
int smart_home_miloco_tools_register(agent_t *agent,
                                     smart_home_agent_app_t *app);
#endif

int smart_home_agent_app_init(smart_home_agent_app_t *app);
void smart_home_agent_app_deinit(smart_home_agent_app_t *app);
void smart_home_agent_app_set_network_status(
    smart_home_agent_app_t *app,
    const smart_home_network_status_t *status);
int smart_home_agent_app_apply_model_config(
    smart_home_agent_app_t *app,
    const smart_home_model_config_t *config);
int smart_home_agent_run(smart_home_agent_app_t *app,
                         const char *input,
                         char *output,
                         size_t output_size);
/** 遍历当前 agent registry；回调内不得调用 Agent/应用接口或保存字符串指针。 */
int smart_home_agent_app_enumerate_tools(
    const smart_home_agent_app_t *app,
    smart_home_tool_visit_fn callback,
    void *user_data);
/** UI 查询与权限变更：锁被聊天任务占用时立即返回 BUSY。 */
int smart_home_agent_app_try_enumerate_tools(
    const smart_home_agent_app_t *app, smart_home_tool_visit_fn callback,
    void *user_data);
int smart_home_agent_app_try_set_local_tool_access(
    smart_home_agent_app_t *app, const char *tool_name, int allowed);
/** 设置本地工具的用户访问策略；拒绝时工具不会执行，但仍向模型声明。 */
int smart_home_agent_app_set_local_tool_access(smart_home_agent_app_t *app,
                                               const char *tool_name,
                                               int allowed);
/** 查询本地工具的用户访问策略；未显式配置的本地工具默认允许。 */
int smart_home_agent_app_local_tool_access_allowed(
    const smart_home_agent_app_t *app, const char *tool_name, int *allowed);

int smart_home_agent_app_clear_session(smart_home_agent_app_t *app);

int smart_home_agent_app_set_observer(smart_home_agent_app_t *app,
                                      agent_event_callback_t callback, void *user_data);

/* Complete groups newest first; callback must copy borrowed text and must not reenter. */
int smart_home_agent_app_visit_history(smart_home_agent_app_t *app,
                                       agent_session_visit_fn visit, void *context);
