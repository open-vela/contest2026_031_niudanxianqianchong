/* Exercise the product adapter against the real MCP constructor/catalog. */
#include "cagent_addons/mcp_bridge.h"
#include <assert.h>
#include <string.h>
static int fail_create;
static caddons_mcp_bridge_t *checked_create(const caddons_mcp_bridge_config_t *config)
{
    assert(config->tool_policy_count == 1);
    assert(!strcmp(config->tool_policies[0].name, "get_status"));
    assert(!strcmp(config->extra_headers, "X-Test: test\r\n"));
    return fail_create ? NULL : caddons_mcp_bridge_create(config);
}
#define caddons_mcp_bridge_create checked_create
#include "../../src/addons/smart_home_mcp_bridge.c"
#undef caddons_mcp_bridge_create
void smart_home_memory_log_region(const char *label, const void *pointer)
{ (void)label; (void)pointer; }
agent_error_t agent_port_openvela_tls_init(agent_port_openvela_tls_t *state,
    const agent_port_openvela_tls_config_t *config)
{ (void)state; (void)config; return AGENT_ERROR_NOT_SUPPORTED; }
int main(void)
{
    smart_home_mcp_bridge_t bridge = {0};
    smart_home_mcp_runtime_config_t config = {
        .host = "127.0.0.1", .path = "/mcp", .server_id = "test", .port = 18791
    }, loaded;
    assert(pthread_mutex_init(&bridge.state_mutex, NULL) == 0);
    bridge.state_mutex_ready = true;
    pthread_mutex_t agent_mutex;
    assert(pthread_mutex_init(&agent_mutex, NULL) == 0);
    bridge.agent_mutex = &agent_mutex;
    bridge.catalog = caddons_remote_catalog_create();
    bridge.bridge = (void *)&bridge; /* First replacement must not destroy a fake handle. */
    bridge.settings.policy_count = 1;
    strcpy(bridge.settings.policy_names[0], "get_status");
    bridge.settings.policies[0] = (caddons_mcp_tool_policy_t){
        bridge.settings.policy_names[0], CADDONS_TOOL_RISK_READ_ONLY, 1000};
    strcpy(bridge.settings.extra_headers, "X-Test: test\r\n");
    fail_create = 1;
    assert(smart_home_mcp_bridge_reconfigure(&bridge, &config) == AGENT_ERROR_NOMEM);
    assert(bridge.bridge == (void *)&bridge);
    bridge.bridge = NULL;
    /* A real original instance exercises replacement and repeated policy copies. */
    caddons_mcp_bridge_config_t initial = {.host = "127.0.0.1", .port = 18790,
        .path = "/mcp", .server_id = "initial", .catalog = bridge.catalog,
        .http_request = mcp_http_request, .http_context = &bridge,
        .tool_policies = bridge.settings.policies, .tool_policy_count = 1,
        .extra_headers = bridge.settings.extra_headers, .request_timeout_ms = 1000};
    bridge.bridge = caddons_mcp_bridge_create(&initial);
    assert(bridge.bridge);
    fail_create = 0;
    assert(smart_home_mcp_bridge_reconfigure(&bridge, &config) == AGENT_OK);
    config.port++;
    assert(smart_home_mcp_bridge_reconfigure(&bridge, &config) == AGENT_OK);
    assert(smart_home_mcp_bridge_get_config(&bridge, &loaded) == AGENT_OK);
    assert(loaded.port == config.port && !strcmp(loaded.host, config.host));
    caddons_mcp_bridge_t *previous = bridge.bridge;
    memset(config.host, 'x', sizeof(config.host) - 1);
    config.host[sizeof(config.host) - 1] = '\0';
    /* The public UI buffer accepts a longer server ID than the backend. */
    memset(config.server_id, 'x', sizeof(config.server_id) - 1);
    config.server_id[sizeof(config.server_id) - 1] = '\0';
    assert(smart_home_mcp_bridge_reconfigure(&bridge, &config) == AGENT_ERROR_LIMIT);
    assert(bridge.bridge == previous);
    caddons_mcp_bridge_destroy(bridge.bridge);
    caddons_remote_catalog_destroy(bridge.catalog);
    pthread_mutex_destroy(&bridge.state_mutex);
    pthread_mutex_destroy(&agent_mutex);
    puts("PASS: reconfigure preserves policies/headers and original bridge on failure");
    return 0;
}
