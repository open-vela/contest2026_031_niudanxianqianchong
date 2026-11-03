/* SPDX-License-Identifier: Apache-2.0 */
/**
 * Dynamic remote-tool catalog.
 *
 * agent_run(), mutation application, enable changes, and catalog destruction
 * must be serialized by one application worker. Transport callbacks may call
 * discover/remove_source and only enqueue registry mutations. Result storage is
 * owned by the catalog and remains valid until the next remote execution.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <agent.h>

#include "cagent_addons/errors.h"
#include "cagent_addons/limits.h"
#include "cagent_addons/node_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct caddons_remote_catalog caddons_remote_catalog_t;

typedef enum {
    CADDONS_TOOL_LOCAL = 0,
    CADDONS_TOOL_NODE = 10,
    CADDONS_TOOL_MCP = 20,
} caddons_tool_origin_t;

typedef enum {
    CADDONS_REMOTE_DISCOVERED = 0,
    CADDONS_REMOTE_REGISTER_QUEUED,
    CADDONS_REMOTE_REGISTERED,
    CADDONS_REMOTE_DISABLE_QUEUED,
    CADDONS_REMOTE_DRAINING,
    CADDONS_REMOTE_REMOVED,
} caddons_remote_state_t;

typedef int (*caddons_remote_execute_fn)(void *backend_context,
                                         const char *route_id,
                                         uint64_t connection_gen,
                                         const char *arguments_json,
                                         uint64_t deadline_ms,
                                         char *result_json,
                                         size_t result_size);

typedef struct {
    caddons_tool_origin_t origin;
    char route_id[CAGENT_ADDONS_ROUTE_ID_SIZE + 1];
    char source_name[CAGENT_ADDONS_COMMAND_NAME_SIZE + 1];
    char public_name[CAGENT_ADDONS_MAX_PUBLIC_NAME_SIZE + 1];
    char description[CAGENT_ADDONS_MAX_DESCRIPTION_SIZE + 1];
    char input_schema_json[CAGENT_ADDONS_MAX_SCHEMA_SIZE + 1];
    caddons_tool_risk_t risk;
    uint32_t timeout_ms;
    uint64_t connection_gen;
    caddons_remote_execute_fn execute;
    void *backend_context;
} caddons_remote_descriptor_t;

typedef struct {
    caddons_remote_descriptor_t descriptor;
    caddons_remote_state_t state;
    bool online;
    bool enabled;
    uint32_t inflight;
} caddons_remote_view_t;

typedef enum {
    CADDONS_MUTATION_REGISTER = 0,
    CADDONS_MUTATION_UNREGISTER,
} caddons_remote_mutation_type_t;

typedef struct {
    caddons_remote_mutation_type_t type;
    char public_name[CAGENT_ADDONS_MAX_PUBLIC_NAME_SIZE + 1];
    uint64_t connection_gen;
} caddons_remote_mutation_t;

caddons_remote_catalog_t *caddons_remote_catalog_create(void);
/* Destroy only after all unregister mutations have been applied and calls drained. */
void caddons_remote_catalog_destroy(caddons_remote_catalog_t *catalog);
int caddons_remote_catalog_discover(caddons_remote_catalog_t *catalog,
                                    const caddons_remote_descriptor_t *tool);
int caddons_remote_catalog_remove_source(caddons_remote_catalog_t *catalog,
                                         caddons_tool_origin_t origin,
                                         const char *route_prefix,
                                         uint64_t connection_gen);
int caddons_remote_catalog_next_mutation(caddons_remote_catalog_t *catalog,
                                         caddons_remote_mutation_t *mutation);
int caddons_remote_catalog_apply_mutation(caddons_remote_catalog_t *catalog,
                                          agent_t *agent,
                                          const caddons_remote_mutation_t *mutation);
size_t caddons_remote_catalog_list(const caddons_remote_catalog_t *catalog,
                                   caddons_remote_view_t *tools,
                                   size_t capacity);
int caddons_remote_catalog_set_enabled(caddons_remote_catalog_t *catalog,
                                       agent_t *agent,
                                       const char *public_name,
                                       bool enabled);
int caddons_error_to_agent(int error);

#ifdef __cplusplus
}
#endif
