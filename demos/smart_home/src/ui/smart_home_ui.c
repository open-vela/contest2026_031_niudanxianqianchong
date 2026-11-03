#include "smart_home_ui.h"
#include "../config/smart_home_config.h"
#include "../agent/smart_home_tool_metadata.h"
#ifdef CONFIG_SMART_HOME_MCP_BRIDGE
#include "../addons/smart_home_mcp_bridge.h"
#endif

#include <stdio.h>
#include <inttypes.h>
#include <string.h>

static const char *event_type_name(agent_event_type_t type)
{
    switch (type) {
    case AGENT_EVENT_TURN_BEGIN: return "TURN_BEGIN";
    case AGENT_EVENT_MODEL_BEGIN: return "MODEL_BEGIN";
    case AGENT_EVENT_MODEL_END: return "MODEL_END";
    case AGENT_EVENT_TOOL_BEGIN: return "TOOL_BEGIN";
    case AGENT_EVENT_TOOL_END: return "TOOL_END";
    case AGENT_EVENT_TURN_END: return "TURN_END";
    default:                          return " UNKNOWN  ";
    }
}

static void trim_newline(char *text)
{
    size_t len;

    if (!text) {
        return;
    }

    len = strlen(text);
    while (len > 0u &&
           (text[len - 1u] == '\n' || text[len - 1u] == '\r')) {
        text[--len] = '\0';
    }
}

static int handle_env_command(smart_home_agent_app_t *app, const char *input)
{
    smart_home_state_t *state;
    char key[16] = "";
    int value = 0;
    int ret;

    if (!app || !input || strncmp(input, "env", 3) != 0 ||
        (input[3] != '\0' && input[3] != ' ' && input[3] != '\t')) {
        return 0;
    }

    state = &app->device_state;
    if (sscanf(input, "env %15s %d", key, &value) == 2) {
        int temperature = state->env_temperature;
        int humidity = state->env_humidity;
        int ambient_light = state->env_light;

        if (strcmp(key, "temp") == 0 || strcmp(key, "temperature") == 0) {
            temperature = value;
        } else if (strcmp(key, "hum") == 0 ||
                   strcmp(key, "humidity") == 0) {
            humidity = value;
        } else if (strcmp(key, "light") == 0 ||
                   strcmp(key, "ambient_light") == 0) {
            ambient_light = value;
        } else {
            printf("usage: env temp <0-45> | env hum <0-100> | "
                   "env light <0-1000>\n");
            return 1;
        }

        ret = smart_home_device_service_set_environment(&app->device_service,
                                                        temperature,
                                                        humidity,
                                                        ambient_light);
        if (ret != AGENT_OK) {
            printf("env update failed: %d\n", ret);
            return 1;
        }
    } else if (input[3] != '\0') {
        printf("usage: env temp <0-45> | env hum <0-100> | "
               "env light <0-1000>\n");
        return 1;
    }

    printf("environment: Temp %dC | Hum %d%% | Light %dlx\n",
           state->env_temperature,
           state->env_humidity,
           state->env_light);
    return 1;
}

static int handle_mcp_command(smart_home_agent_app_t *app, const char *input)
{
    if (!app || !input || strncmp(input, "mcp", 3) != 0 ||
        (input[3] != '\0' && input[3] != ' ' && input[3] != '\t')) {
        return 0;
    }

#ifdef CONFIG_SMART_HOME_MCP_BRIDGE
    if (strcmp(input, "mcp discover") == 0) {
        int ret;

        if (!app->mcp_bridge) {
            printf("MCP bridge is unavailable; check its startup configuration.\n");
            return 1;
        }

        ret = smart_home_mcp_bridge_request_discover(app->mcp_bridge);
        if (ret == AGENT_OK) {
            printf("MCP discovery requested. Use 'mcp status' to check progress.\n");
        } else if (ret == AGENT_ERROR_BUSY) {
            printf("MCP discovery is already running.\n");
        } else {
            printf("MCP discovery request failed: %d\n", ret);
        }
        return 1;
    }

    if (strcmp(input, "mcp status") == 0) {
        static const char *const states[] = {
            "Manual", "Connecting", "Connected", "Failed"
        };
        smart_home_mcp_state_t state;
        int last_error = 0;
        int ret;

        if (!app->mcp_bridge) {
            printf("MCP bridge is unavailable; check its startup configuration.\n");
            return 1;
        }

        ret = smart_home_mcp_bridge_get_state(app->mcp_bridge,
                                               &state, &last_error);
        if (ret != AGENT_OK || state < SMART_HOME_MCP_STATE_IDLE ||
            state > SMART_HOME_MCP_STATE_FAILED) {
            printf("MCP status read failed: %d\n", ret);
            return 1;
        }

        printf("MCP: %s (last_error=%d)\n", states[state], last_error);
        return 1;
    }
#else
    (void)app;
    if (strcmp(input, "mcp discover") == 0 || strcmp(input, "mcp status") == 0) {
        printf("MCP bridge is not enabled in this build.\n");
        return 1;
    }
#endif

    printf("usage: mcp discover | mcp status\n");
    return 1;
}

#ifdef CONFIG_SMART_HOME_NODE_GATEWAY
typedef struct {
    size_t count;
} node_status_writer_t;

static int print_node_tool_status(const smart_home_tool_info_t *tool,
                                  void *user_data)
{
    node_status_writer_t *writer = user_data;

    if (!tool || !writer) {
        return AGENT_ERROR_INVALID;
    }
    if (tool->group_id != SMART_HOME_TOOL_GROUP_NODE) {
        return AGENT_OK;
    }

    if (tool->description && tool->description[0]) {
        printf("  %s [%s]: %s\n",
               tool->name ? tool->name : "-",
               tool->enabled ? "enabled" : "disabled",
               tool->description);
    } else {
        printf("  %s [%s]\n",
               tool->name ? tool->name : "-",
               tool->enabled ? "enabled" : "disabled");
    }
    writer->count++;
    return AGENT_OK;
}
#endif

static int handle_node_command(smart_home_agent_app_t *app, const char *input)
{
    if (!app || !input || strncmp(input, "node", 4) != 0 ||
        (input[4] != '\0' && input[4] != ' ' && input[4] != '\t')) {
        return 0;
    }

    if (strcmp(input, "node status") != 0) {
        printf("usage: node status\n");
        return 1;
    }

#ifndef CONFIG_SMART_HOME_NODE_GATEWAY
    printf("Node gateway is not enabled in this build.\n");
    return 1;
#else
    node_status_writer_t writer = {0};
    int ret;

    if (!app->node_gateway) {
        printf("Node gateway is unavailable; check its startup configuration.\n");
        return 1;
    }

    ret = smart_home_agent_app_enumerate_tools(app,
                                               print_node_tool_status,
                                               &writer);
    if (ret != AGENT_OK) {
        printf("Node status read failed: %d\n", ret);
        return 1;
    }

    if (writer.count == 0u) {
        printf("Node: no registered tools.\n");
    } else {
        printf("Node: %lu registered tool(s).\n", (unsigned long)writer.count);
    }
    return 1;
#endif
}

void smart_home_ui_event_cb(const agent_event_t *event, void *user_data)
{
    uint64_t *start = user_data;
    uint64_t elapsed = 0u;
    if (!event) return;
    if (start) {
        if (event->type == AGENT_EVENT_TURN_BEGIN) *start = event->timestamp_ms;
        if (event->timestamp_ms >= *start) elapsed = event->timestamp_ms - *start;
    }
    fprintf(stderr, "%8llums | %-11s | model=%" PRIu32 " | status=%d | tool=%.*s | id=%.*s\n",
            (unsigned long long)elapsed, event_type_name(event->type),
            event->summary.model_calls, event->status,
            (int)event->tool_call.name.size, event->tool_call.name.data ? event->tool_call.name.data : "",
            (int)event->tool_call.id.size, event->tool_call.id.data ? event->tool_call.id.data : "");
}

int smart_home_ui_run_once(smart_home_agent_app_t *app, const char *input)
{
    char output[SMART_HOME_OUTPUT_SIZE];
    int ret;

    ret = smart_home_agent_run(app, input, output, sizeof(output));
    if (ret == AGENT_OK || ret == AGENT_ERROR_TRUNCATED) {
        printf("\nassistant: %s%s\n", output, ret == AGENT_ERROR_TRUNCATED ? " [reply truncated]" : "");
    } else {
        printf("\nERROR (%d): %s\n", ret, output[0] ? output : "no detail");
    }

    return ret;
}

int smart_home_ui_run(smart_home_agent_app_t *app)
{
    char input[SMART_HOME_INPUT_SIZE];

    for (;;) {
        printf("home> ");
        fflush(stdout);

        if (!fgets(input, sizeof(input), stdin)) {
            printf("\n");
            break;
        }

        trim_newline(input);
        if (input[0] == '\0') {
            continue;
        }

        if (strcmp(input, "quit") == 0 || strcmp(input, "exit") == 0) {
            break;
        }

        if (strcmp(input, "clear") == 0) {
            printf("clear session: %d\n", smart_home_agent_app_clear_session(app));
            continue;
        }

        if (handle_env_command(app, input)) {
            continue;
        }

        if (handle_mcp_command(app, input)) {
            continue;
        }

        if (handle_node_command(app, input)) {
            continue;
        }

        /* A failed model or tool call affects only this request.  Keep the
         * interactive console alive so the user can inspect and retry it.
         */

        (void)smart_home_ui_run_once(app, input);
    }

    return AGENT_OK;
}
