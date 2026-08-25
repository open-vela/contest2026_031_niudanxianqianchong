#include "smart_home_agent.h"
#ifdef CONFIG_SMART_HOME_APP_BRIDGE
#include "smart_home_agent_run_service.h"
#endif
#ifdef CONFIG_SMART_HOME_NODE_GATEWAY
#include "../addons/smart_home_node_gateway.h"
#endif
#ifdef CONFIG_SMART_HOME_MCP_BRIDGE
#include "../addons/smart_home_mcp_bridge.h"
#endif

#include "../config/smart_home_backends.h"
#include "../config/smart_home_config.h"
#include "../config/smart_home_config_store.h"
#include "../config/smart_home_secrets.h"
#include "../device/smart_home_device.h"
#include "smart_home_tool_metadata.h"
#include "../smart_home_cpu_debug.h"
#include <cagent/runtime_openvela.h>
#include "../skills/smart_home_scene_catalog.h"
#include "../skills/smart_home_skills.h"
#include "../tools/smart_home_tools.h"
#include "../ui/smart_home_ui.h"

#include <stdio.h>
#include <string.h>
#include <syslog.h>

static const char g_system_prompt[] =
    "You are a smart home assistant running on an embedded device. "
    "Use tools for every device state query or device change. "
    "If a tool result reports policy_denied, say that the operation was not "
    "performed because the relevant tool is disabled in Settings, and explain "
    "how to enable it. Never claim success after a denied tool call. "
    "Reply concisely in the user's language.";

#define SMART_HOME_OPENAI_REQUEST_BUFFER_MIN  12288u
#define SMART_HOME_OPENAI_RESPONSE_BUFFER_MIN 8192u

#if defined(CONFIG_SMART_HOME_NODE_GATEWAY) || defined(CONFIG_SMART_HOME_MCP_BRIDGE) || \
    defined(CONFIG_SMART_HOME_APP_BRIDGE)
#define SMART_HOME_HAS_REMOTE_TOOLS 1
#endif

/* Keep the P4X early-boot trace deliberately sparse: USB console writes are
 * synchronous on this profile, so verbose diagnostics can delay start-up. */
static void smart_home_init_trace(const char *stage, int ret)
{
#ifdef CONFIG_SMART_HOME_DEMO_DEBUG_LOG
    if (!stage ||
        (strcmp(stage, "begin") != 0 &&
         strcmp(stage, "agent-create-begin") != 0 &&
         strcmp(stage, "agent-create-done") != 0)) {
        return;
    }

    printf("[smart_home_init] stage=%s ret=%d\n",
           stage, ret);
#else
    (void)stage;
    (void)ret;
#endif
}

static uint32_t max_u32(uint32_t a, uint32_t b)
{
    return a > b ? a : b;
}

static void secure_clear(void *memory, size_t size)
{
    volatile unsigned char *p = memory;

    while (p && size-- > 0u) {
        *p++ = 0u;
    }
}

static int local_tool_access_index(const smart_home_agent_app_t *app,
                                   const char *tool_name)
{
    size_t index;

    if (!app || !tool_name || !tool_name[0]) {
        return -1;
    }

    for (index = 0u; index < app->local_tool_access_count; index++) {
        if (strcmp(app->local_tool_access[index].name, tool_name) == 0) {
            return (int)index;
        }
    }

    return -1;
}

static int local_tool_access_allowed(const smart_home_agent_app_t *app,
                                     const char *tool_name)
{
    int index = local_tool_access_index(app, tool_name);

    return index < 0 || app->local_tool_access[index].allowed;
}

static agent_policy_decision_t smart_home_local_tool_policy(
    const agent_policy_request_t *request, void *user_data)
{
    const smart_home_agent_app_t *app = user_data;

    if (!app || !request || request->action != AGENT_POLICY_ACTION_TOOL_CALL ||
        !request->tool_call || !request->tool_call->name) {
        return AGENT_POLICY_ALLOW;
    }

    return local_tool_access_allowed(app, request->tool_call->name)
               ? AGENT_POLICY_ALLOW
               : AGENT_POLICY_DENY;
}

static void set_empty_reply_fallback(char *output, size_t output_size)
{
    if (!output || output_size == 0u) {
        return;
    }

    snprintf(output,
             output_size,
             "抱歉，当前没有可用工具完成该操作。请检查“设置 → 工具访问”中是否已启用相应控制工具。");
}

static void smart_home_status_init(smart_home_system_status_t *status)
{
    if (!status) {
        return;
    }

    memset(status, 0, sizeof(*status));
    smart_home_network_status_init(&status->network_status);
    status->agent_status = SMART_HOME_STATUS_UNKNOWN;
    status->skills_status = SMART_HOME_STATUS_UNKNOWN;
    status->tools_status = SMART_HOME_STATUS_UNKNOWN;
    status->model_status = SMART_HOME_STATUS_UNKNOWN;
    status->node_gateway_status = SMART_HOME_STATUS_UNKNOWN;
    status->mcp_bridge_status = SMART_HOME_STATUS_UNKNOWN;
}

static void smart_home_status_error(smart_home_agent_app_t *app,
                                    const char *scope,
                                    int ret)
{
    if (!app) {
        return;
    }

    snprintf(app->system_status.last_error,
             sizeof(app->system_status.last_error),
             "%s failed: %d",
             scope ? scope : "Subsystem",
             ret);
}

#ifdef CONFIG_SMART_HOME_NODE_GATEWAY
static void smart_home_status_node_gateway_disabled(
    smart_home_agent_app_t *app, int ret)
{
    const char *reason = "initialization failed";

    if (!app) {
        return;
    }

    if (ret == AGENT_ERROR_NOTFOUND) {
        reason = "credentials file is missing";
    } else if (ret == AGENT_ERROR_PARSE) {
        reason = "credentials JSON is invalid";
    } else if (ret == AGENT_ERROR_INVALID) {
        reason = "credentials or gateway configuration is invalid";
    } else if (ret == AGENT_ERROR_NOMEM) {
        reason = "memory is unavailable";
    } else if (ret == AGENT_ERROR_NETWORK) {
        reason = "listen transport is unavailable";
    }

    snprintf(app->system_status.last_error,
             sizeof(app->system_status.last_error),
             "Node gateway disabled: %s (%d)", reason, ret);
    syslog(LOG_WARNING, "smart_home: %s\n", app->system_status.last_error);
}
#endif

#ifdef CONFIG_SMART_HOME_MCP_BRIDGE
static void smart_home_status_mcp_bridge_disabled(smart_home_agent_app_t *app,
                                                  int ret)
{
    snprintf(app->system_status.last_error, sizeof(app->system_status.last_error),
             "MCP bridge disabled: initialization/discovery failed (%d)", ret);
    syslog(LOG_WARNING, "smart_home: %s\n", app->system_status.last_error);
}
#endif
static void smart_home_model_config_default(smart_home_model_config_t *config)
{
    const smart_home_llm_backend_preset_t *preset;

    if (!config) {
        return;
    }

    memset(config, 0, sizeof(*config));
    preset = smart_home_llm_backend_default();
    if (preset) {
        (void)smart_home_model_config_from_preset(config, preset, 0);
    }
    config->request_buffer_size =
        max_u32(CAGENT_HTTP_REQUEST_BUFFER_SIZE,
                SMART_HOME_OPENAI_REQUEST_BUFFER_MIN);
    config->response_buffer_size =
        max_u32(CAGENT_HTTP_RESPONSE_BUFFER_SIZE,
                SMART_HOME_OPENAI_RESPONSE_BUFFER_MIN);
    config->max_output_tokens = 512u;
}

/* settings.json 只选择内置后端；未知/custom 项回退内置默认，避免启动时
 * 得到没有 host/model 的不可用 provider。密钥随后始终由 secrets loader 提供。 */
static void smart_home_model_config_load_active_backend(
    smart_home_model_config_t *config)
{
    cJSON *settings = NULL;
    cJSON *active;
    const smart_home_llm_backend_preset_t *preset;
    char error[64];
    int index;
    int ret;

    if (!config) {
        return;
    }

    ret = smart_home_config_settings_load(&settings, error, sizeof(error));
    if (ret != AGENT_OK || !settings) {
        syslog(LOG_WARNING, "smart_home: settings unavailable (%d)\n", ret);
        return;
    }

    active = cJSON_GetObjectItemCaseSensitive(settings, "active_backend_id");
    index = cJSON_IsString(active)
                ? smart_home_llm_backend_index_by_id(active->valuestring) : -1;
    preset = index >= 0 ? smart_home_llm_backend_get((size_t)index) : NULL;
    if (preset && (preset->flags & SMART_HOME_LLM_FLAG_CUSTOM) == 0u) {
        (void)smart_home_model_config_from_preset(config, preset, 0);
    } else if (cJSON_IsString(active)) {
        syslog(LOG_WARNING,
               "smart_home: unsupported saved backend id; using default\n");
    }
    cJSON_Delete(settings);
}

static int smart_home_model_config_resolve_key(
    const smart_home_model_config_t *config,
    smart_home_model_config_t *resolved)
{
    const smart_home_llm_backend_preset_t *preset;
    char key[sizeof(resolved->api_key)];
    int index;
    int ret;

    if (!config || !resolved) {
        return AGENT_ERROR_INVALID;
    }

    *resolved = *config;
    resolved->api_key[0] = '\0';
    index = smart_home_llm_backend_index_by_id(config->backend_id);
    preset = index >= 0 ? smart_home_llm_backend_get((size_t)index) : NULL;
    if (!preset) {
        return AGENT_ERROR_NOTFOUND;
    }

    if ((preset->flags & SMART_HOME_LLM_FLAG_CUSTOM) != 0u) {
        if (!config->api_key[0]) {
            return AGENT_ERROR_NOTFOUND;
        }
        strncpy(resolved->api_key, config->api_key,
                sizeof(resolved->api_key) - 1u);
        resolved->api_key[sizeof(resolved->api_key) - 1u] = '\0';
        return AGENT_OK;
    }

    ret = smart_home_secrets_get_model_api_key(config->backend_id,
                                                key,
                                                sizeof(key));
    if (ret == AGENT_OK) {
        memcpy(resolved->api_key, key, strlen(key) + 1u);
    }
    secure_clear(key, sizeof(key));
    return ret;
}

static int smart_home_save_active_backend(const char *backend_id)
{
    cJSON *settings = NULL;
    cJSON *replacement;
    char error[64];
    int ret;

    if (!backend_id || !backend_id[0]) {
        return AGENT_ERROR_INVALID;
    }

    ret = smart_home_config_settings_load(&settings, error, sizeof(error));
    if (ret != AGENT_OK || !settings) {
        cJSON_Delete(settings);
        return ret != AGENT_OK ? ret : AGENT_ERROR_PARSE;
    }
    replacement = cJSON_CreateString(backend_id);
    if (!replacement) {
        cJSON_Delete(settings);
        return AGENT_ERROR_NOMEM;
    }
    if (!cJSON_ReplaceItemInObjectCaseSensitive(settings,
                                                 "active_backend_id",
                                                 replacement)) {
        cJSON_Delete(replacement);
        cJSON_Delete(settings);
        return AGENT_ERROR_PARSE;
    }
    ret = smart_home_config_settings_save(settings);
    cJSON_Delete(settings);
    return ret;
}

static void smart_home_status_model_key_error(smart_home_agent_app_t *app,
                                              const char *backend_id,
                                              int ret)
{
    const smart_home_llm_backend_preset_t *preset = NULL;
    int index = smart_home_llm_backend_index_by_id(backend_id);

    if (!app) {
        return;
    }
    if (index >= 0) {
        preset = smart_home_llm_backend_get((size_t)index);
    }
    snprintf(app->system_status.last_error,
             sizeof(app->system_status.last_error),
             "Model API key unavailable for %s (%d)",
             preset && preset->name ? preset->name : "backend", ret);
}

int smart_home_agent_app_init(smart_home_agent_app_t *app)
{
    agent_model_openai_config_t model_config;
    agent_model_t *model;
    const smart_home_loaded_skill_t *scene_skill;
    smart_home_scene_catalog_t scene_catalog;
    int ret;

    if (!app) {
        return AGENT_ERROR_INVALID;
    }

    smart_home_init_trace("begin", AGENT_OK);
    memset(app, 0, sizeof(*app));
    smart_home_status_init(&app->system_status);
    smart_home_init_trace("status-ready", AGENT_OK);

    smart_home_device_init(&app->device_state);
    smart_home_init_trace("device-state-ready", AGENT_OK);

    smart_home_init_trace("device-service-begin", AGENT_OK);
    ret = smart_home_device_service_init(&app->device_service,
                                         &app->device_state);
    smart_home_init_trace("device-service-done", ret);
    if (ret != AGENT_OK) {
        app->system_status.agent_status = ret;
        smart_home_status_error(app, "Device service", ret);
        return ret;
    }

    smart_home_skill_store_init(&app->skill_store);
    smart_home_init_trace("skill-store-ready", AGENT_OK);

    smart_home_model_config_default(&app->model_config);
    smart_home_init_trace("model-default-ready", AGENT_OK);

    smart_home_init_trace("settings-load-begin", AGENT_OK);
    smart_home_model_config_load_active_backend(&app->model_config);
    smart_home_init_trace("settings-load-done", AGENT_OK);

#ifdef SMART_HOME_HAS_REMOTE_TOOLS
    smart_home_init_trace("agent-mutex-begin", AGENT_OK);
    if (pthread_mutex_init(&app->agent_mutex, NULL) != 0) {
        smart_home_init_trace("agent-mutex-done", AGENT_ERROR);
        app->system_status.agent_status = AGENT_ERROR;
        smart_home_status_error(app, "Agent mutex", AGENT_ERROR);
        return AGENT_ERROR;
    }
    app->agent_mutex_initialized = 1;
    smart_home_init_trace("agent-mutex-done", AGENT_OK);
#endif

    smart_home_init_trace("agent-create-begin", AGENT_OK);
    app->agent = agent_create_simple("smart_home_agent", g_system_prompt);
    smart_home_init_trace("agent-create-done",
                          app->agent ? AGENT_OK : AGENT_ERROR_NOMEM);
    if (!app->agent) {
        app->system_status.agent_status = AGENT_ERROR_NOMEM;
        smart_home_status_error(app, "Agent create", AGENT_ERROR_NOMEM);
        smart_home_agent_app_deinit(app);
        return AGENT_ERROR_NOMEM;
    }

    smart_home_init_trace("device-context-begin", AGENT_OK);
    ret = smart_home_device_register_context(app->agent, &app->device_state);
    smart_home_init_trace("device-context-done", ret);
    if (ret != AGENT_OK) {
        app->system_status.agent_status = ret;
        smart_home_status_error(app, "Device context", ret);
        smart_home_agent_app_deinit(app);
        return ret;
    }

    smart_home_init_trace("skills-load-begin", AGENT_OK);
    ret = smart_home_skills_register(app->agent, &app->skill_store);
    app->system_status.skills_status = ret;
    app->system_status.skills_loaded = app->skill_store.count;
    smart_home_init_trace("skills-load-done", ret);
    if (ret == AGENT_ERROR_NOTFOUND) {
        /* Runtime skill files are optional for the local UI bring-up path.
         * A board without /data/res/skills can still show device panels,
         * settings and model diagnostics.  Do not hide malformed skill
         * files: parse, limit and registration failures remain fatal below.
         */
        syslog(LOG_WARNING,
               "smart_home: runtime skills unavailable; "
               "starting without skills and scene catalog\n");
    } else if (ret != AGENT_OK) {
        smart_home_status_error(app, "Skills", ret);
        smart_home_agent_app_deinit(app);
        return ret;
    }

    if (ret == AGENT_OK) {
        smart_home_init_trace("scene-catalog-begin", AGENT_OK);
        scene_skill = smart_home_skill_store_find(&app->skill_store,
                                                  "smart_home_scenes");
        ret = smart_home_scene_catalog_load(
            scene_skill ? scene_skill->context_text : NULL, &scene_catalog);
        smart_home_init_trace("scene-catalog-load-done", ret);
        if (ret != AGENT_OK) {
            app->system_status.skills_status = ret;
            smart_home_status_error(app, "Scene catalog", ret);
            smart_home_agent_app_deinit(app);
            return ret;
        }

        smart_home_init_trace("scene-service-begin", AGENT_OK);
        ret = smart_home_device_service_set_scene_catalog(&app->device_service,
                                                           &scene_catalog);
        smart_home_init_trace("scene-service-done", ret);
        if (ret != AGENT_OK) {
            app->system_status.tools_status = ret;
            smart_home_status_error(app, "Scene service", ret);
            smart_home_agent_app_deinit(app);
            return ret;
        }
    }

    smart_home_init_trace("tools-register-begin", AGENT_OK);
    ret = smart_home_tools_register(app->agent, &app->device_service);
    app->system_status.tools_status = ret;
    smart_home_init_trace("tools-register-done", ret);
    if (ret != AGENT_OK) {
        smart_home_status_error(app, "Tools", ret);
    }

    smart_home_init_trace("policy-bind-begin", AGENT_OK);
    ret = agent_set_policy_callback(app->agent,
                                    smart_home_local_tool_policy,
                                    app);
    smart_home_init_trace("policy-bind-done", ret);
    if (ret != AGENT_OK) {
        app->system_status.tools_status = ret;
        smart_home_status_error(app, "Tool policy", ret);
        smart_home_agent_app_deinit(app);
        return ret;
    }

    smart_home_init_trace("event-bind-begin", AGENT_OK);
    agent_set_event_callback(app->agent,
                             smart_home_ui_event_cb,
                             &app->run_start_ms);
    smart_home_init_trace("event-bind-done", AGENT_OK);

    model_config = agent_model_openai_config_default();
    model_config.host = app->model_config.host;
    model_config.path = app->model_config.path;
    model_config.port = app->model_config.port;
    model_config.api_key = app->model_config.api_key;
    model_config.model = app->model_config.model;
    model_config.timeout_ms = (uint32_t)app->model_config.timeout_ms;
    model_config.request_buffer_size = app->model_config.request_buffer_size;
    model_config.response_buffer_size = app->model_config.response_buffer_size;

    smart_home_init_trace("model-create-begin", AGENT_OK);
    model = agent_model_openai_create(&model_config);
    smart_home_init_trace("model-create-done",
                          model ? AGENT_OK : AGENT_ERROR_NOMEM);
    if (!model) {
        app->system_status.model_status = AGENT_ERROR_NOMEM;
        smart_home_status_error(app, "Model create", AGENT_ERROR_NOMEM);
        smart_home_agent_app_deinit(app);
        return AGENT_ERROR_NOMEM;
    }

    smart_home_init_trace("model-bind-begin", AGENT_OK);
    ret = agent_set_model_owned(app->agent, model);
    smart_home_init_trace("model-bind-done", ret);
    if (ret != AGENT_OK) {
        app->system_status.model_status = ret;
        smart_home_status_error(app, "Model bind", ret);
        agent_model_destroy(model);
        smart_home_agent_app_deinit(app);
        return ret;
    }

    smart_home_init_trace("model-config-apply-begin", AGENT_OK);
    ret = smart_home_agent_app_apply_model_config(app, &app->model_config);
    smart_home_init_trace("model-config-apply-done", ret);
    if (ret != AGENT_OK) {
        /* 密钥缺失只禁用当前云 provider；设备控制与设置页仍可启动，供用户
         * 将 secrets.json 写入 data 分区后切换/重试。 */
        app->system_status.model_status = ret;
        smart_home_status_model_key_error(app, app->model_config.backend_id, ret);
    }

#ifdef CONFIG_SMART_HOME_APP_BRIDGE_CHAT
    smart_home_init_trace("app-run-service-begin", AGENT_OK);
    ret = smart_home_agent_run_service_start(&app->run_service, app);
    smart_home_init_trace("app-run-service-done", ret);
    if (ret != AGENT_OK) {
        smart_home_status_error(app, "Agent run service", ret);
        smart_home_agent_app_deinit(app);
        return ret;
    }
#endif

#ifdef CONFIG_SMART_HOME_NODE_GATEWAY
    smart_home_init_trace("node-gateway-begin", AGENT_OK);
    ret = smart_home_node_gateway_start(&app->node_gateway, app->agent,
                                        &app->agent_mutex);
    app->system_status.node_gateway_status = ret;
    smart_home_init_trace("node-gateway-done", ret);
    if (ret != AGENT_OK) {
#ifdef CONFIG_SMART_HOME_NODE_GATEWAY_REQUIRED
        smart_home_status_error(app, "Node gateway", ret);
        smart_home_agent_app_deinit(app);
        return ret;
#else
        smart_home_status_node_gateway_disabled(app, ret);
#endif
    }
#endif

#ifdef CONFIG_SMART_HOME_MCP_BRIDGE
    smart_home_init_trace("mcp-bridge-begin", AGENT_OK);
    ret = smart_home_mcp_bridge_start(&app->mcp_bridge, app->agent,
                                      &app->agent_mutex);
    app->system_status.mcp_bridge_status = ret;
    smart_home_init_trace("mcp-bridge-done", ret);
    if (ret != AGENT_OK) {
        /* 手动发现模式下，配置错误只禁用 MCP，不影响本地和 Node 功能。 */
        smart_home_status_mcp_bridge_disabled(app, ret);
    }
#endif

    app->system_status.agent_status = AGENT_OK;
    smart_home_init_trace("done", AGENT_OK);
    return AGENT_OK;
}

void smart_home_agent_app_deinit(smart_home_agent_app_t *app)
{
    if (!app) {
        return;
    }

#ifdef CONFIG_SMART_HOME_MCP_BRIDGE
    smart_home_mcp_bridge_stop(&app->mcp_bridge);
#endif
#ifdef CONFIG_SMART_HOME_NODE_GATEWAY
    smart_home_node_gateway_stop(&app->node_gateway);
#endif
#ifdef CONFIG_SMART_HOME_APP_BRIDGE_CHAT
    smart_home_agent_run_service_stop(&app->run_service);
#endif
    if (app->agent) {
        agent_destroy(app->agent);
    }
    smart_home_skill_store_deinit(&app->skill_store);
    smart_home_device_service_deinit(&app->device_service);
#ifdef SMART_HOME_HAS_REMOTE_TOOLS
    if (app->agent_mutex_initialized) {
        pthread_mutex_destroy(&app->agent_mutex);
    }
#endif
    memset(app, 0, sizeof(*app));
}

void smart_home_agent_app_set_network_status(
    smart_home_agent_app_t *app,
    const smart_home_network_status_t *status)
{
    if (!app) {
        return;
    }

    if (status) {
        app->system_status.network_status = *status;
    } else {
        smart_home_network_status_init(&app->system_status.network_status);
    }

    if (app->system_status.network_status.init_status ==
        SMART_HOME_NETWORK_STATUS_NA) {
        return;
    }

    if (!app->system_status.network_status.online) {
        int ret = app->system_status.network_status.dns_status;

        if (ret >= 0) {
            ret = app->system_status.network_status.ip_status;
        }
        if (ret >= 0) {
            ret = app->system_status.network_status.init_status;
        }
        smart_home_status_error(app, "Network", ret);
    }
}

int smart_home_agent_app_apply_model_config(
    smart_home_agent_app_t *app,
    const smart_home_model_config_t *config)
{
    agent_model_t *model;
    agent_limits_t limits = AGENT_LIMITS_DEFAULT;
    smart_home_model_config_t resolved;
    char error[96];
    int ret;

    if (!app || !app->agent || !config) {
        if (app) {
            app->system_status.model_status = AGENT_ERROR_INVALID;
            smart_home_status_error(app, "Model config", AGENT_ERROR_INVALID);
        }
        return AGENT_ERROR_INVALID;
    }

    ret = smart_home_model_config_validate(config, error, sizeof(error));
    if (ret != AGENT_OK) {
        app->system_status.model_status = ret;
        strncpy(app->system_status.last_error,
                error,
                sizeof(app->system_status.last_error) - 1u);
        app->system_status.last_error[
            sizeof(app->system_status.last_error) - 1u] = '\0';
        return ret;
    }

    ret = smart_home_model_config_resolve_key(config, &resolved);
    if (ret != AGENT_OK) {
        app->system_status.model_status = ret;
        smart_home_status_model_key_error(app, config->backend_id, ret);
        return ret;
    }

    model = agent_get_model(app->agent);
    if (!model) {
        app->system_status.model_status = AGENT_ERROR_MODEL;
        smart_home_status_error(app, "Model", AGENT_ERROR_MODEL);
        return AGENT_ERROR_MODEL;
    }

#ifdef SMART_HOME_HAS_REMOTE_TOOLS
    pthread_mutex_lock(&app->agent_mutex);
#endif
    ret = agent_model_openai_set_backend(model,
                                         resolved.host,
                                         resolved.path,
                                         resolved.port);
    if (ret != AGENT_OK) {
#ifdef SMART_HOME_HAS_REMOTE_TOOLS
        pthread_mutex_unlock(&app->agent_mutex);
#endif
        app->system_status.model_status = ret;
        smart_home_status_error(app, "Model backend", ret);
        return ret;
    }

    ret = agent_model_openai_set_api_key(model, resolved.api_key);
    if (ret != AGENT_OK) {
#ifdef SMART_HOME_HAS_REMOTE_TOOLS
        pthread_mutex_unlock(&app->agent_mutex);
#endif
        app->system_status.model_status = ret;
        smart_home_status_error(app, "Model API key", ret);
        return ret;
    }

    ret = agent_model_openai_set_model(model, resolved.model);
    if (ret != AGENT_OK) {
#ifdef SMART_HOME_HAS_REMOTE_TOOLS
        pthread_mutex_unlock(&app->agent_mutex);
#endif
        app->system_status.model_status = ret;
        smart_home_status_error(app, "Model name", ret);
        return ret;
    }

    limits.timeout_ms = (uint32_t)resolved.timeout_ms;
    limits.per_model_timeout_ms = (uint32_t)resolved.timeout_ms;
    limits.max_output_tokens = resolved.max_output_tokens > 0u
                                   ? resolved.max_output_tokens
                                   : limits.max_output_tokens;
    ret = agent_set_limits(app->agent, &limits);
#ifdef SMART_HOME_HAS_REMOTE_TOOLS
    pthread_mutex_unlock(&app->agent_mutex);
#endif
    if (ret != AGENT_OK) {
        app->system_status.model_status = ret;
        smart_home_status_error(app, "Agent limits", ret);
        return ret;
    }

    app->model_config = resolved;
    if (smart_home_llm_backend_index_by_id(resolved.backend_id) >= 0 &&
        strcmp(resolved.backend_id, "custom") != 0) {
        /* cAGENT provider owns its own copy; the app/UI config need not keep
         * a built-in backend credential resident or accidentally expose it. */
        secure_clear(app->model_config.api_key,
                     sizeof(app->model_config.api_key));
    }
    secure_clear(resolved.api_key, sizeof(resolved.api_key));
    app->system_status.model_status = AGENT_OK;
    if (app->system_status.agent_status == AGENT_OK &&
        strcmp(resolved.backend_id, "custom") != 0) {
        ret = smart_home_save_active_backend(resolved.backend_id);
        if (ret != AGENT_OK) {
            smart_home_status_error(app, "Active backend save", ret);
            return ret;
        }
    }
    return AGENT_OK;
}

int smart_home_agent_run(smart_home_agent_app_t *app,
                         const char *input,
                         char *output,
                         size_t output_size)
{
    char stack_marker;
    agent_request_t request;
    agent_response_t response;
    int ret;

    if (!app || !app->agent || !input || !output || output_size == 0u) {
        return AGENT_ERROR_INVALID;
    }

    if (app->system_status.model_status != AGENT_OK) {
        snprintf(output,
                 output_size,
                 "当前模型不可用。请在“设置 → 模型 API”中选择后端，并在 secrets.json 配置对应 API Key。");
        return app->system_status.model_status;
    }

    memset(&request, 0, sizeof(request));
    memset(&response, 0, sizeof(response));
    output[0] = '\0';

    request.session_id = SMART_HOME_SESSION_ID;
    request.input = input;
    response.output = output;
    response.output_size = output_size;
    smart_home_cpu_debug_log("agent-run-start");
    ov_mem_region_log("agent-run-stack", &stack_marker);

#ifdef SMART_HOME_HAS_REMOTE_TOOLS
    pthread_mutex_lock(&app->agent_mutex);
#endif
    ret = agent_run(app->agent, &request, &response);
#ifdef SMART_HOME_HAS_REMOTE_TOOLS
    pthread_mutex_unlock(&app->agent_mutex);
#endif
    if (ret == AGENT_OK && output[0] == '\0') {
        set_empty_reply_fallback(output, output_size);
    }
    return ret;
}

int smart_home_agent_app_enumerate_tools(
    const smart_home_agent_app_t *app,
    agent_tool_enumerate_fn callback,
    void *user_data)
{
    int ret;

    if (!app || !app->agent || !callback) {
        return AGENT_ERROR_INVALID;
    }

#ifdef SMART_HOME_HAS_REMOTE_TOOLS
    pthread_mutex_lock((pthread_mutex_t *)&app->agent_mutex);
#endif
    ret = agent_tool_enumerate(app->agent, callback, user_data);
#ifdef SMART_HOME_HAS_REMOTE_TOOLS
    pthread_mutex_unlock((pthread_mutex_t *)&app->agent_mutex);
#endif
    return ret;
}

typedef struct {
    const char *name;
    int is_local;
    int core_enabled;
} local_tool_match_t;

static int match_local_tool(const agent_tool_info_t *tool, void *user_data)
{
    local_tool_match_t *match = user_data;

    if (tool && match && tool->name && match->name &&
        strcmp(tool->name, match->name) == 0 &&
        tool->group_id == SMART_HOME_TOOL_GROUP_LOCAL) {
        match->is_local = 1;
        match->core_enabled = tool->enabled;
    }

    return AGENT_OK;
}

int smart_home_agent_app_set_local_tool_access(smart_home_agent_app_t *app,
                                               const char *tool_name,
                                               int allowed)
{
    local_tool_match_t match = {
        .name = tool_name,
        .is_local = 0,
    };
    int ret;

    if (!app || !app->agent || !tool_name || !tool_name[0]) {
        if (app) {
            app->system_status.tools_status = AGENT_ERROR_INVALID;
            smart_home_status_error(app, "Tool access", AGENT_ERROR_INVALID);
        }
        return AGENT_ERROR_INVALID;
    }

#ifdef SMART_HOME_HAS_REMOTE_TOOLS
    pthread_mutex_lock(&app->agent_mutex);
#endif
    ret = agent_tool_enumerate(app->agent, match_local_tool, &match);
    if (ret == AGENT_OK && !match.is_local) {
        ret = AGENT_ERROR_NOTSUP;
    }
    if (ret == AGENT_OK && !match.core_enabled) {
        ret = AGENT_ERROR_NOTSUP;
    }
    if (ret == AGENT_OK) {
        int index = local_tool_access_index(app, tool_name);

        if (index < 0) {
            size_t name_length = strlen(tool_name);

            if (name_length >= SMART_HOME_LOCAL_TOOL_NAME_SIZE) {
                ret = AGENT_ERROR_LIMIT;
            } else if (app->local_tool_access_count >=
                       SMART_HOME_LOCAL_TOOL_ACCESS_MAX) {
                ret = AGENT_ERROR_LIMIT;
            } else {
                index = (int)app->local_tool_access_count++;
                memcpy(app->local_tool_access[index].name,
                       tool_name,
                       name_length + 1u);
            }
        }

        if (ret == AGENT_OK && index >= 0) {
            app->local_tool_access[index].allowed = allowed ? 1 : 0;
        }
    }
#ifdef SMART_HOME_HAS_REMOTE_TOOLS
    pthread_mutex_unlock(&app->agent_mutex);
#endif

    app->system_status.tools_status = ret;
    if (ret != AGENT_OK) {
        smart_home_status_error(app, "Tool access", ret);
        return ret;
    }

    app->system_status.last_error[0] = '\0';
    return AGENT_OK;
}

int smart_home_agent_app_local_tool_access_allowed(
    const smart_home_agent_app_t *app, const char *tool_name, int *allowed)
{
    if (!app || !tool_name || !tool_name[0] || !allowed) {
        return AGENT_ERROR_INVALID;
    }

#ifdef SMART_HOME_HAS_REMOTE_TOOLS
    pthread_mutex_lock((pthread_mutex_t *)&app->agent_mutex);
#endif
    *allowed = local_tool_access_allowed(app, tool_name) ? 1 : 0;
#ifdef SMART_HOME_HAS_REMOTE_TOOLS
    pthread_mutex_unlock((pthread_mutex_t *)&app->agent_mutex);
#endif
    return AGENT_OK;
}
