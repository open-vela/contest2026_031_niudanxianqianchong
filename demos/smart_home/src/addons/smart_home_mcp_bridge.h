/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include <agent.h>

typedef struct smart_home_mcp_bridge smart_home_mcp_bridge_t;

/* Runtime configuration for the V9 settings dialog.  The bridge is rebuilt
 * with these values; tools from the previous endpoint are unregistered by
 * caddons_mcp_bridge_destroy() after a replacement is successfully created. */
typedef struct {
    char server_id[64];
    char host[256];
    char path[128];
    char authorization[384];
    uint16_t port;
    bool use_tls;
} smart_home_mcp_runtime_config_t;

/* 手动发现状态机：start 返回后为 IDLE；应用显式请求发现后，操作 worker
 * 在独立大栈中收敛到 READY 或 FAILED。 */
typedef enum {
    SMART_HOME_MCP_STATE_IDLE = 0,
    SMART_HOME_MCP_STATE_CONNECTING,
    SMART_HOME_MCP_STATE_READY,
    SMART_HOME_MCP_STATE_FAILED
} smart_home_mcp_state_t;

/* Creates the bridge and starts an idle operation worker. No network I/O is
 * performed until smart_home_mcp_bridge_request_discover() is called. */
int smart_home_mcp_bridge_start(smart_home_mcp_bridge_t **bridge_out,
                                agent_t *agent,
                                pthread_mutex_t *agent_mutex);

/* Queue one synchronous MCP discovery operation on the bridge worker. The
 * caller returns immediately; the worker performs initialize/tools/list and
 * then applies catalog mutations. The LVGL thread must use this API instead
 * of performing network I/O directly. */
int smart_home_mcp_bridge_request_discover(smart_home_mcp_bridge_t *bridge);

/* Rebuild the bridge with a new endpoint/auth.  Returns BUSY while a
 * discovery is in flight; the caller should retry after it settles. */
int smart_home_mcp_bridge_reconfigure(
    smart_home_mcp_bridge_t *bridge,
    const smart_home_mcp_runtime_config_t *config);

int smart_home_mcp_bridge_get_config(smart_home_mcp_bridge_t *bridge,
                                      smart_home_mcp_runtime_config_t *config);

/* Stops the bridge, drains queued unregister operations, and frees state.
 * Joins an in-flight discovery first (bounded by the network deadline). */
void smart_home_mcp_bridge_stop(smart_home_mcp_bridge_t **bridge_ptr);

/* Reads the current discovery state; last_error receives the CADDONS_*
 * discovery result when state is FAILED (0 otherwise). */
int smart_home_mcp_bridge_get_state(smart_home_mcp_bridge_t *bridge,
                                    smart_home_mcp_state_t *state,
                                    int *last_error);

/* Bounded wait for a requested discovery to reach a terminal state. */
int smart_home_mcp_bridge_wait_ready(smart_home_mcp_bridge_t *bridge,
                                     uint32_t timeout_ms);
