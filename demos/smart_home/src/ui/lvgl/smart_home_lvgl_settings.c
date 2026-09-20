/**
 * smart_home LVGL settings screen.
 */

#include "smart_home_lvgl_internal.h"
#include "images/smart_home_icons.h"
#include "../../agent/smart_home_tool_metadata.h"
#include "../../config/smart_home_backends.h"
#include "../../config/smart_home_secrets.h"
#include "../../net/smart_home_network.h"
#include "../../net/smart_home_wifi.h"
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
#include "../../miloco/smart_home_miloco.h"
#endif
#ifdef CONFIG_SMART_HOME_VOICE_TTS
#include "../../voice/smart_home_tts.h"
#include "../../voice/smart_home_voice_play.h"
#endif
#ifdef CONFIG_SMART_HOME_VOICE_ASR
#include "../../voice/smart_home_asr.h"
#endif
#ifdef CONFIG_SMART_HOME_KWS
#include "../../voice/smart_home_kws_service.h"
#endif
#ifdef CONFIG_SMART_HOME_MCP_BRIDGE
#include "../../addons/smart_home_mcp_bridge.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SMART_HOME_BACKEND_OPTIONS_CAP 256u

/* 更多页卡片跳转定位用：三张可折叠卡的卡片对象（单实例，构建时记录）。
 * 声明必须位于 create_tool_directory_card 等首个使用点之前。 */
static lv_obj_t *s_settings_model_card;
static lv_obj_t *s_settings_tool_card;
static lv_obj_t *s_settings_system_card;

/*
 * Keep the dropdown order identical to smart_home_backends.c.  The selected
 * index is used to retrieve the preset, so a separately maintained literal
 * string can silently point a visible label at the wrong backend.
 */
static void build_backend_dropdown_options(char *options, size_t options_size)
{
    size_t index;
    size_t used = 0u;

    if (!options || options_size == 0u) {
        return;
    }

    options[0] = '\0';
    for (index = 0u; index < smart_home_llm_backend_count(); index++) {
        const smart_home_llm_backend_preset_t *preset =
            smart_home_llm_backend_get(index);
        const char *label = preset && preset->name ? preset->name : "Unknown";
        int written;

        written = snprintf(options + used,
                           options_size - used,
                           "%s%s",
                           used == 0u ? "" : "\n",
                           label);
        if (written < 0 || (size_t)written >= options_size - used) {
            /* A truncated option list would break index-to-preset mapping. */
            options[0] = '\0';
            return;
        }

        used += (size_t)written;
    }
}

static void layout_settings_keyboard(smart_home_lvgl_t *ui, int visible)
{
    if (!ui || !ui->settings_keyboard) {
        return;
    }

    lv_obj_set_size(ui->settings_keyboard,
                    smart_home_lvgl_content_w(),
                    smart_home_lvgl_keyboard_h());
    lv_obj_align(ui->settings_keyboard,
                 LV_ALIGN_BOTTOM_MID,
                 0,
                 -SMART_HOME_NAV_H - SMART_HOME_NAV_BOTTOM_PAD - 8);

    if (visible) {
        lv_obj_clear_flag(ui->settings_keyboard, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui->settings_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

static void settings_input_event(lv_event_t *event)
{
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)lv_event_get_user_data(event);
    lv_obj_t *target = lv_event_get_target(event);
    lv_event_code_t code = lv_event_get_code(event);

    if (!ui) {
        return;
    }

    if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
        if (ui->settings_keyboard && target) {
            lv_keyboard_set_textarea(ui->settings_keyboard, target);
        }
        layout_settings_keyboard(ui, 1);
    } else if (code == LV_EVENT_CANCEL || code == LV_EVENT_READY ||
               code == LV_EVENT_DEFOCUSED) {
        layout_settings_keyboard(ui, 0);
    }
}

static void settings_keyboard_event(lv_event_t *event)
{
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_CANCEL || code == LV_EVENT_READY) {
        layout_settings_keyboard(ui, 0);
    }
}

static lv_obj_t *create_settings_input(lv_obj_t *parent,
                                       smart_home_lvgl_t *ui,
                                       const char *title,
                                       const char *value,
                                       uint32_t max_length,
                                       int password)
{
    lv_obj_t *wrap;
    lv_obj_t *label;
    lv_obj_t *input;

    wrap = lv_obj_create(parent);
    lv_obj_remove_style_all(wrap);
    lv_obj_set_size(wrap, lv_pct(100), smart_home_lvgl_compact() ? 54 : 58);
    lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(wrap, 4, 0);

    label = smart_home_lvgl_label_create(wrap,
                                         title,
                                         SMART_HOME_UI_COLOR_TEXT_SECONDARY,
                                         12);
    lv_obj_set_width(label, lv_pct(100));

    input = lv_textarea_create(wrap);
    lv_textarea_set_one_line(input, true);
    lv_textarea_set_text(input, value ? value : "");
    lv_textarea_set_max_length(input, max_length);
    if (password) {
        lv_textarea_set_password_mode(input, true);
    }

    lv_obj_set_size(input, lv_pct(100), 32);
    lv_obj_set_style_text_font(input, smart_home_lvgl_font(12), 0);
    lv_obj_set_style_text_color(input, SMART_HOME_UI_COLOR_TEXT_PRIMARY, 0);
    lv_obj_set_style_bg_color(input, SMART_HOME_UI_COLOR_SURFACE_SOFT, 0);
    lv_obj_set_style_bg_opa(input, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(input, 1, 0);
    lv_obj_set_style_border_color(input, SMART_HOME_UI_COLOR_BORDER, 0);
    lv_obj_set_style_radius(input, 6, 0);
    lv_obj_add_event_cb(input, settings_input_event, LV_EVENT_ALL, ui);

    return input;
}

static void copy_textarea(char *dst, size_t dst_size, lv_obj_t *textarea)
{
    const char *text;

    if (!dst || dst_size == 0u) {
        return;
    }

    dst[0] = '\0';
    if (!textarea) {
        return;
    }

    text = lv_textarea_get_text(textarea);
    if (text) {
        strncpy(dst, text, dst_size - 1u);
        dst[dst_size - 1u] = '\0';
    }
}

static void set_settings_status(smart_home_lvgl_t *ui,
                                const char *text,
                                lv_color_t color)
{
    if (!ui || !ui->settings_status_label) {
        return;
    }

    lv_label_set_text(ui->settings_status_label, text ? text : "");
    lv_obj_set_style_text_color(ui->settings_status_label, color, 0);
}

/* 内置云后端的凭据只来自 secrets.json。设置页既不显示也不接受其明文；
 * Custom 仍可使用一次性、仅当前运行期的手工 Key。 */
static void update_api_key_input_for_backend(
    smart_home_lvgl_t *ui,
    const smart_home_llm_backend_preset_t *preset)
{
    if (!ui || !ui->settings_api_key_input || !preset) {
        return;
    }

    if ((preset->flags & SMART_HOME_LLM_FLAG_CUSTOM) != 0u) {
        lv_obj_clear_state(ui->settings_api_key_input, LV_STATE_DISABLED);
        lv_textarea_set_text(ui->settings_api_key_input, "");
    } else {
        lv_textarea_set_text(ui->settings_api_key_input,
                             "Managed by secrets.json");
        lv_obj_add_state(ui->settings_api_key_input, LV_STATE_DISABLED);
    }
}

static void show_backend_key_status(smart_home_lvgl_t *ui,
                                    const smart_home_llm_backend_preset_t *preset)
{
    int ret;
    char message[96];

    if (!ui || !preset) {
        return;
    }
    if ((preset->flags & SMART_HOME_LLM_FLAG_CUSTOM) != 0u) {
        set_settings_status(ui,
                            "Custom backend: API key is session-only and not saved.",
                            SMART_HOME_UI_COLOR_TEXT_MUTED);
        return;
    }

    ret = smart_home_secrets_model_api_key_status(preset->id);
    if (ret == AGENT_OK) {
        snprintf(message, sizeof(message), "%s key is configured in secrets.json.",
                 preset->name);
        set_settings_status(ui, message, SMART_HOME_UI_COLOR_SUCCESS);
    } else if (ret == AGENT_ERROR_NOTFOUND) {
        snprintf(message, sizeof(message), "%s API key is not configured.",
                 preset->name);
        set_settings_status(ui, message, SMART_HOME_UI_COLOR_WARNING);
    } else {
        snprintf(message, sizeof(message), "Cannot read %s key (%d).",
                 preset->name, ret);
        set_settings_status(ui, message, SMART_HOME_UI_COLOR_DANGER);
    }
}

static void set_textarea_uint(lv_obj_t *textarea, uint32_t value)
{
    char text[16];

    if (!textarea) {
        return;
    }

    snprintf(text, sizeof(text), "%lu", (unsigned long)value);
    lv_textarea_set_text(textarea, text);
}

static void model_api_toggle_event(lv_event_t *event)
{
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)lv_event_get_user_data(event);

    if (lv_event_get_code(event) != LV_EVENT_CLICKED ||
        !ui || !ui->settings_model_api_body) {
        return;
    }

    if (lv_obj_has_flag(ui->settings_model_api_body, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(ui->settings_model_api_body, LV_OBJ_FLAG_HIDDEN);
        if (ui->settings_model_api_toggle) {
            lv_label_set_text(ui->settings_model_api_toggle, "-");
        }
    } else {
        lv_obj_add_flag(ui->settings_model_api_body, LV_OBJ_FLAG_HIDDEN);
        if (ui->settings_model_api_toggle) {
            lv_label_set_text(ui->settings_model_api_toggle, "+");
        }
    }
}

static void tool_directory_toggle_event(lv_event_t *event)
{
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)lv_event_get_user_data(event);

    if (lv_event_get_code(event) != LV_EVENT_CLICKED ||
        !ui || !ui->settings_tool_directory_body) {
        return;
    }

    if (lv_obj_has_flag(ui->settings_tool_directory_body, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(ui->settings_tool_directory_body, LV_OBJ_FLAG_HIDDEN);
        if (ui->settings_tool_directory_toggle) {
            lv_label_set_text(ui->settings_tool_directory_toggle, "-");
        }
    } else {
        lv_obj_add_flag(ui->settings_tool_directory_body, LV_OBJ_FLAG_HIDDEN);
        if (ui->settings_tool_directory_toggle) {
            lv_label_set_text(ui->settings_tool_directory_toggle, "+");
        }
    }
}

static void system_status_toggle_event(lv_event_t *event)
{
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)lv_event_get_user_data(event);

    if (lv_event_get_code(event) != LV_EVENT_CLICKED ||
        !ui || !ui->settings_system_status_body) {
        return;
    }

    if (lv_obj_has_flag(ui->settings_system_status_body, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(ui->settings_system_status_body, LV_OBJ_FLAG_HIDDEN);
        if (ui->settings_system_status_toggle) {
            lv_label_set_text(ui->settings_system_status_toggle, "-");
        }
    } else {
        lv_obj_add_flag(ui->settings_system_status_body, LV_OBJ_FLAG_HIDDEN);
        if (ui->settings_system_status_toggle) {
            lv_label_set_text(ui->settings_system_status_toggle, "+");
        }
    }
}

#ifdef CONFIG_SMART_HOME_MCP_BRIDGE
static void mcp_discover_event(lv_event_t *event)
{
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)lv_event_get_user_data(event);
    int ret;

    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !ui || !ui->app
        || !ui->app->mcp_bridge) {
        return;
    }

    ret = smart_home_mcp_bridge_request_discover(ui->app->mcp_bridge);
    if (ret == AGENT_OK) {
        set_settings_status(ui,
                            "MCP discovery requested.",
                            SMART_HOME_UI_COLOR_TEXT_SECONDARY);
    } else if (ret == AGENT_ERROR_BUSY) {
        set_settings_status(ui,
                            "MCP discovery is already running.",
                            SMART_HOME_UI_COLOR_WARNING);
    } else {
        char message[64];

        snprintf(message, sizeof(message), "MCP discovery request failed (%d).", ret);
        set_settings_status(ui, message, SMART_HOME_UI_COLOR_DANGER);
    }
}
#endif

static void backend_dropdown_event(lv_event_t *event)
{
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)lv_event_get_user_data(event);
    const smart_home_llm_backend_preset_t *preset;
    int selected;

    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED ||
        !ui || !ui->settings_backend_dd) {
        return;
    }

    selected = (int)lv_dropdown_get_selected(ui->settings_backend_dd);
    preset = smart_home_llm_backend_get((size_t)selected);
    if (!preset) {
        return;
    }

    if ((preset->flags & SMART_HOME_LLM_FLAG_CUSTOM) == 0u) {
        lv_textarea_set_text(ui->settings_host_input, preset->host);
        lv_textarea_set_text(ui->settings_path_input, preset->path);
        lv_textarea_set_text(ui->settings_port_input, preset->port);
        lv_textarea_set_text(ui->settings_model_input, preset->default_model);
        set_textarea_uint(ui->settings_timeout_input,
                          preset->default_timeout_ms);
        update_api_key_input_for_backend(ui, preset);
        show_backend_key_status(ui, preset);
    } else {
        const char *port = lv_textarea_get_text(ui->settings_port_input);
        if (!port || !port[0]) {
            lv_textarea_set_text(ui->settings_port_input, preset->port);
        }
        update_api_key_input_for_backend(ui, preset);
        show_backend_key_status(ui, preset);
    }
}

static void apply_settings_event(lv_event_t *event)
{
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)lv_event_get_user_data(event);
    smart_home_model_config_t config;
    const smart_home_llm_backend_preset_t *preset;
    const char *timeout_text;
    const char *max_tokens_text;
    int timeout_ms;
    int max_tokens;
    int selected;
    int ret;
    char error[96];

    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !ui || !ui->app) {
        return;
    }

    if (ui->request_inflight) {
        set_settings_status(ui,
                            "Wait for the current request to finish.",
                            SMART_HOME_UI_COLOR_WARNING);
        return;
    }

    config = ui->app->model_config;
    selected = ui->settings_backend_dd
                   ? (int)lv_dropdown_get_selected(ui->settings_backend_dd)
                   : smart_home_llm_backend_index_for_config(&config);
    preset = smart_home_llm_backend_get((size_t)selected);
    if (preset) {
        strncpy(config.backend_id,
                preset->id,
                sizeof(config.backend_id) - 1u);
        config.backend_id[sizeof(config.backend_id) - 1u] = '\0';
    } else {
        strncpy(config.backend_id, "custom", sizeof(config.backend_id) - 1u);
        config.backend_id[sizeof(config.backend_id) - 1u] = '\0';
    }

    copy_textarea(config.host, sizeof(config.host), ui->settings_host_input);
    copy_textarea(config.path, sizeof(config.path), ui->settings_path_input);
    copy_textarea(config.port, sizeof(config.port), ui->settings_port_input);
    copy_textarea(config.model, sizeof(config.model), ui->settings_model_input);
    if (preset && (preset->flags & SMART_HOME_LLM_FLAG_CUSTOM) != 0u) {
        copy_textarea(config.api_key,
                      sizeof(config.api_key),
                      ui->settings_api_key_input);
    } else {
        /* Never carry a prior backend's key into a built-in backend. */
        config.api_key[0] = '\0';
    }

    timeout_text = lv_textarea_get_text(ui->settings_timeout_input);
    timeout_ms = timeout_text ? atoi(timeout_text) : 0;
    if (timeout_ms <= 0) {
        timeout_ms = 30000;
    }
    config.timeout_ms = timeout_ms;

    max_tokens_text = lv_textarea_get_text(ui->settings_max_tokens_input);
    max_tokens = max_tokens_text ? atoi(max_tokens_text) : 0;
    if (max_tokens > 0) {
        config.max_output_tokens = (uint32_t)max_tokens;
    }

    ret = smart_home_model_config_validate(&config, error, sizeof(error));
    if (ret != AGENT_OK) {
        if (ui->app) {
            ui->app->system_status.model_status = ret;
            strncpy(ui->app->system_status.last_error,
                    error,
                    sizeof(ui->app->system_status.last_error) - 1u);
            ui->app->system_status.last_error[
                sizeof(ui->app->system_status.last_error) - 1u] = '\0';
        }
        set_settings_status(ui, error, SMART_HOME_UI_COLOR_DANGER);
        return;
    }

    ret = smart_home_agent_app_apply_model_config(ui->app, &config);
    if (ret == AGENT_OK) {
        set_settings_status(ui,
                            "Model settings applied.",
                            SMART_HOME_UI_COLOR_SUCCESS);
    } else {
        char message[96];
        snprintf(message, sizeof(message), "Apply failed: %d", ret);
        set_settings_status(ui, message, SMART_HOME_UI_COLOR_DANGER);
    }
    /* No keyboard in NSH mode — nothing to hide. */
}

typedef struct {
    lv_obj_t *body;
    smart_home_lvgl_t *ui;
    uint16_t category;
    size_t count;
    int have_category;
} tool_directory_builder_t;

typedef struct {
    smart_home_lvgl_t *ui;
    char name[];
} local_tool_switch_context_t;

static const char *tool_category_name(uint16_t category)
{
    switch (category) {
    case SMART_HOME_TOOL_CATEGORY_QUERY:
        return "Query";
    case SMART_HOME_TOOL_CATEGORY_CONTROL:
        return "Control";
    case SMART_HOME_TOOL_CATEGORY_SKILL:
        return "Skill";
    default:
        return "Other";
    }
}

static void create_tool_category_label(lv_obj_t *parent,
                                       uint16_t category)
{
    lv_obj_t *label;

    label = smart_home_lvgl_label_create(parent,
                                         tool_category_name(category),
                                         SMART_HOME_UI_COLOR_TEXT_SECONDARY,
                                         12);
    lv_obj_set_width(label, lv_pct(100));
}

static const char *tool_source_name(uint16_t group_id)
{
    switch (group_id) {
    case 0u:
        return "Local";
    case 10u:
        return "Node";
    case 20u:
        return "MCP";
    default:
        return "Other";
    }
}

static void set_tool_switch_state(lv_obj_t *sw, int enabled)
{
    if (!sw) {
        return;
    }

    if (enabled) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(sw, LV_STATE_CHECKED);
    }
}

static void local_tool_switch_event(lv_event_t *event)
{
    local_tool_switch_context_t *context = lv_event_get_user_data(event);
    lv_obj_t *target = lv_event_get_target(event);
    smart_home_lvgl_t *ui;
    char message[96];
    int allowed;
    int ret;

    if (lv_event_get_code(event) == LV_EVENT_DELETE) {
        free(context);
        return;
    }

    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED || !context ||
        !target || !context->ui) {
        return;
    }

    ui = context->ui;
    allowed = lv_obj_has_state(target, LV_STATE_CHECKED) ? 1 : 0;
    if (ui->request_inflight) {
        set_tool_switch_state(target, !allowed);
        set_settings_status(ui,
                            "Wait for the current request to finish.",
                            SMART_HOME_UI_COLOR_WARNING);
        return;
    }

    ret = smart_home_agent_app_set_local_tool_access(ui->app,
                                                      context->name,
                                                      allowed);
    if (ret != AGENT_OK) {
        set_tool_switch_state(target, !allowed);
        snprintf(message, sizeof(message), "Tool update failed: %d", ret);
        set_settings_status(ui, message, SMART_HOME_UI_COLOR_DANGER);
        return;
    }

    snprintf(message, sizeof(message), "%s %s.", context->name,
             allowed ? "allowed" : "blocked");
    set_settings_status(ui, message, SMART_HOME_UI_COLOR_SUCCESS);
}

static void create_local_tool_switch(lv_obj_t *parent,
                                     smart_home_lvgl_t *ui,
                                     const agent_tool_info_t *tool)
{
    local_tool_switch_context_t *context;
    lv_obj_t *sw;
    size_t name_length;
    int allowed = 1;

    if (!parent || !tool || !tool->name ||
        tool->group_id != SMART_HOME_TOOL_GROUP_LOCAL) {
        return;
    }

    name_length = strlen(tool->name);
    context = malloc(sizeof(*context) + name_length + 1u);
    if (!context) {
        return;
    }

    context->ui = ui;
    memcpy(context->name, tool->name, name_length + 1u);
    sw = lv_switch_create(parent);
    lv_obj_set_size(sw, 44, 24);
    if (smart_home_agent_app_local_tool_access_allowed(ui->app,
                                                        tool->name,
                                                        &allowed) != AGENT_OK) {
        allowed = 1;
    }
    set_tool_switch_state(sw, allowed);
    lv_obj_add_event_cb(sw, local_tool_switch_event, LV_EVENT_ALL, context);
}

static void create_tool_directory_row(lv_obj_t *parent,
                                      smart_home_lvgl_t *ui,
                                      const agent_tool_info_t *tool)
{
    lv_obj_t *row;
    lv_obj_t *title_row;
    lv_obj_t *label;
    char status[64];
    int access_allowed = 1;

    row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(row,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(row, 2, 0);

    title_row = lv_obj_create(row);
    lv_obj_remove_style_all(title_row);
    lv_obj_set_size(title_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_row,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(title_row, 8, 0);

    label = smart_home_lvgl_label_create(title_row,
                                         tool && tool->name ? tool->name : "",
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY,
                                         12);
    lv_obj_set_flex_grow(label, 1);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

    create_local_tool_switch(title_row, ui, tool);

    if (tool && tool->group_id == SMART_HOME_TOOL_GROUP_LOCAL) {
        (void)smart_home_agent_app_local_tool_access_allowed(ui->app,
                                                              tool->name,
                                                              &access_allowed);
    }
    snprintf(status, sizeof(status), "%s | %s%s",
             tool_source_name(tool ? tool->group_id : 0u),
             tool && tool->enabled
                 ? (access_allowed ? "allowed" : "blocked")
                 : "unavailable",
             tool && !tool->llm_visible ? " | hidden from AI" : "");
    label = smart_home_lvgl_label_create(row,
                                         status,
                                         SMART_HOME_UI_COLOR_TEXT_MUTED,
                                         10);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

    if (tool && tool->description && tool->description[0]) {
        label = smart_home_lvgl_label_create(row,
                                             tool->description,
                                             SMART_HOME_UI_COLOR_TEXT_SECONDARY,
                                             10);
        lv_obj_set_width(label, lv_pct(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    }
}

static int append_tool_directory_entry(const agent_tool_info_t *tool,
                                       void *user_data)
{
    tool_directory_builder_t *builder = user_data;

    if (!tool || !builder || !builder->body) {
        return AGENT_ERROR_INVALID;
    }

    if (!builder->have_category || builder->category != tool->category_id) {
        builder->category = tool->category_id;
        builder->have_category = 1;
        create_tool_category_label(builder->body, builder->category);
    }
    create_tool_directory_row(builder->body, builder->ui, tool);
    builder->count++;
    return AGENT_OK;
}

void smart_home_lvgl_refresh_tool_directory(smart_home_lvgl_t *ui)
{
    tool_directory_builder_t builder;
    int ret;

    if (!ui || !ui->settings_tool_directory_body) {
        return;
    }

    lv_obj_clean(ui->settings_tool_directory_body);
    memset(&builder, 0, sizeof(builder));
    builder.body = ui->settings_tool_directory_body;
    builder.ui = ui;
    ret = smart_home_agent_app_enumerate_tools(ui->app,
                                               append_tool_directory_entry,
                                               &builder);
    if (ret != AGENT_OK) {
        char message[64];

        snprintf(message, sizeof(message), "Tool directory unavailable: %d", ret);
        smart_home_lvgl_label_create(builder.body,
                                     message,
                                     SMART_HOME_UI_COLOR_WARNING,
                                     12);
    } else if (builder.count == 0u) {
        smart_home_lvgl_label_create(builder.body,
                                     "No tools registered.",
                                     SMART_HOME_UI_COLOR_TEXT_MUTED,
                                     12);
    }
}





#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
/* 天气城市：设置页输入英文/拼音城市名，保存后 worker 立即拉取。 */
static void weather_city_save_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);
    char city[24];
    const char *text;

    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !ui ||
        !ui->settings_weather_city_input || !ui->app || !ui->app->miloco) {
        return;
    }
    text = lv_textarea_get_text(ui->settings_weather_city_input);
    snprintf(city, sizeof(city), "%s", text ? text : "");
    if (!city[0]) {
        set_settings_status(ui, "城市不能为空",
                            SMART_HOME_UI_COLOR_WARNING);
        return;
    }
    smart_home_miloco_set_weather_city(ui->app->miloco, city);
    set_settings_status(ui, "天气城市已切换，即将刷新",
                        SMART_HOME_UI_COLOR_SUCCESS);
}

static void create_weather_card(lv_obj_t *content, smart_home_lvgl_t *ui)
{
    lv_obj_t *card;
    lv_obj_t *label;
    lv_obj_t *button;
    smart_home_miloco_weather_t wx;
    char buf[64];

    card = lv_obj_create(content);
    lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 6, 0);
    smart_home_lvgl_card_style(card);

    smart_home_lvgl_icon_create(card, ICON_SUN, 18, 18);
    label = smart_home_lvgl_label_create(card, "天气城市",
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY, 14);

    /* 当前城市与天气摘要。 */
    if (ui->app && ui->app->miloco &&
        smart_home_miloco_get_weather(ui->app->miloco, &wx)) {
        snprintf(buf, sizeof(buf), "当前 %s · %s %d°C",
                 wx.city, wx.condition_cn, wx.temperature);
    } else {
        snprintf(buf, sizeof(buf), "尚未获取");
    }
    label = smart_home_lvgl_label_create(card, buf,
                                         SMART_HOME_UI_COLOR_TEXT_SECONDARY, 12);

    ui->settings_weather_city_input = create_settings_input(
        card, ui, "城市名（英文/拼音，如 Shenzhen）",
        "Shenzhen", 23, 0);

    button = lv_btn_create(card);
    lv_obj_set_size(button, 100, 32);
    label = smart_home_lvgl_label_create(button, "切换城市",
                                         lv_color_white(), 12);
    lv_obj_center(label);
    lv_obj_add_event_cb(button, weather_city_save_cb, LV_EVENT_CLICKED, ui);
}
#endif

static void create_tool_directory_card(lv_obj_t *content,
                                       smart_home_lvgl_t *ui)
{
    lv_obj_t *card;
    lv_obj_t *header;
    lv_obj_t *label;
    lv_obj_t *body;

    if (!content || !ui) {
        return;
    }

    card = lv_obj_create(content);
    s_settings_tool_card = card;
    lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, 0);
    smart_home_lvgl_card_style(card);

    header = lv_obj_create(card);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, lv_pct(100), 28);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(header, 6, 0);
    lv_obj_add_flag(header, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(header,
                        tool_directory_toggle_event,
                        LV_EVENT_CLICKED,
                        ui);

    smart_home_lvgl_icon_create(header, ICON_TOOL, 18, 18);
    label = smart_home_lvgl_label_create(header,
                                         "工具与权限",
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY,
                                         14);
    lv_obj_set_flex_grow(label, 1);

    ui->settings_tool_directory_toggle =
        smart_home_lvgl_label_create(header,
                                     "+",
                                     SMART_HOME_UI_COLOR_TEXT_SECONDARY,
                                     16);
    lv_obj_set_width(ui->settings_tool_directory_toggle, 18);
    lv_obj_set_style_text_align(ui->settings_tool_directory_toggle,
                                LV_TEXT_ALIGN_CENTER,
                                0);

    body = lv_obj_create(card);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 4, 0);
    lv_obj_add_flag(body, LV_OBJ_FLAG_HIDDEN);
    ui->settings_tool_directory_body = body;
    smart_home_lvgl_refresh_tool_directory(ui);
}

static lv_color_t subsystem_status_color(int status)
{
    if (status == AGENT_OK || status == SMART_HOME_WIFI_OK) {
        return SMART_HOME_UI_COLOR_SUCCESS;
    }
    if (status == SMART_HOME_STATUS_UNKNOWN) {
        return SMART_HOME_UI_COLOR_TEXT_MUTED;
    }

    return SMART_HOME_UI_COLOR_WARNING;
}

static lv_obj_t *create_status_row(lv_obj_t *parent,
                                   const char *name,
                                   const char *value,
                                   lv_color_t value_color)
{
    lv_obj_t *row;
    lv_obj_t *label;

    row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), 22);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);

    label = smart_home_lvgl_label_create(row,
                                         name,
                                         SMART_HOME_UI_COLOR_TEXT_SECONDARY,
                                         12);
    lv_obj_set_width(label, smart_home_lvgl_compact() ? 72 : 96);

    label = smart_home_lvgl_label_create(row,
                                         value,
                                         value_color,
                                         12);
    lv_obj_set_flex_grow(label, 1);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    return label;
}

static int network_status_checked(const smart_home_network_status_t *network)
{
    return network &&
           network->init_status != SMART_HOME_NETWORK_STATUS_UNKNOWN;
}

static lv_color_t network_status_color(
    const smart_home_network_status_t *network)
{
    if (!network_status_checked(network)) {
        return SMART_HOME_UI_COLOR_TEXT_MUTED;
    }

    return network->online ? SMART_HOME_UI_COLOR_SUCCESS :
                             SMART_HOME_UI_COLOR_WARNING;
}

static const char *network_status_text(
    const smart_home_network_status_t *network,
    char *buffer,
    size_t size)
{
    const char *ifname = network && network->ifname ? network->ifname : "-";

    if (!network_status_checked(network)) {
        return "Not checked";
    }

    if (network->online) {
        snprintf(buffer, size, "Online (%s)", ifname);
        return buffer;
    }

    if (network->ip_status == SMART_HOME_NETWORK_ERR_NO_IP) {
        snprintf(buffer, size, "No IP (%s)", ifname);
        return buffer;
    }

    if (network->ip_status == SMART_HOME_NETWORK_ERR_DHCP) {
        snprintf(buffer, size, "DHCP failed (%s)", ifname);
        return buffer;
    }

    if (network->dns_status == SMART_HOME_NETWORK_ERR_DNS) {
        snprintf(buffer, size, "DNS failed (%s)", ifname);
        return buffer;
    }

    snprintf(buffer, size, "Offline (%s)", ifname);
    return buffer;
}

static const char *wifi_status_text(
    const smart_home_network_status_t *network,
    char *buffer,
    size_t size)
{
    int status;

    if (!network_status_checked(network)) {
        return "Not checked";
    }

    if (network->backend != SMART_HOME_NETWORK_BACKEND_WIFI) {
        return "N/A";
    }

    status = network->init_status;
    switch (status) {
    case SMART_HOME_WIFI_OK:
        return "Associated";
    case SMART_HOME_WIFI_ERR_CONFIG:
        return "Config missing";
    case SMART_HOME_WIFI_ERR_ASSOC:
        return "Association failed";
    case SMART_HOME_WIFI_ERR_DHCP:
        return "DHCP failed";
    case SMART_HOME_WIFI_ERR_DNS:
        return "DNS failed";
    default:
        snprintf(buffer, size, "Failed (%d)", status);
        return buffer;
    }
}

static lv_color_t wifi_status_color(const smart_home_network_status_t *network)
{
    if (!network_status_checked(network) ||
        network->backend != SMART_HOME_NETWORK_BACKEND_WIFI) {
        return SMART_HOME_UI_COLOR_TEXT_MUTED;
    }

    return network->init_status == SMART_HOME_WIFI_OK ?
               SMART_HOME_UI_COLOR_SUCCESS :
               SMART_HOME_UI_COLOR_WARNING;
}

static const char *skills_status_text(const smart_home_agent_app_t *app,
                                      char *buffer,
                                      size_t size)
{
    int status = app ? app->system_status.skills_status :
                       SMART_HOME_STATUS_UNKNOWN;

    if (status == AGENT_OK) {
        snprintf(buffer,
                 size,
                 "%lu loaded",
                 (unsigned long)(app ? app->system_status.skills_loaded : 0u));
        return buffer;
    }
    if (status == SMART_HOME_STATUS_UNKNOWN) {
        return "Not checked";
    }
    if (status == AGENT_ERROR_NOTFOUND) {
        return "Not found";
    }

    snprintf(buffer, size, "Failed (%d)", status);
    return buffer;
}

static const char *simple_agent_status_text(int status,
                                            const char *ready,
                                            char *buffer,
                                            size_t size)
{
    if (status == AGENT_OK) {
        return ready;
    }
    if (status == SMART_HOME_STATUS_UNKNOWN) {
        return "Not checked";
    }

    snprintf(buffer, size, "Failed (%d)", status);
    return buffer;
}

static const char *model_status_text(const smart_home_agent_app_t *app,
                                     char *buffer,
                                     size_t size)
{
    int status = app ? app->system_status.model_status :
                       SMART_HOME_STATUS_UNKNOWN;

    if (status == AGENT_OK) {
        snprintf(buffer,
                 size,
                 "Ready (%s)",
                 app && app->model_config.backend_id[0] ?
                     app->model_config.backend_id : "custom");
        return buffer;
    }

    return simple_agent_status_text(status, "Ready", buffer, size);
}

static const char *node_gateway_status_text(const smart_home_agent_app_t *app,
                                            char *buffer,
                                            size_t size)
{
#ifndef CONFIG_SMART_HOME_NODE_GATEWAY
    (void)app;
    (void)buffer;
    (void)size;
    return "Not built";
#else
    int status = app ? app->system_status.node_gateway_status :
                       SMART_HOME_STATUS_UNKNOWN;

    if (status == AGENT_OK) {
        snprintf(buffer,
                 size,
                 "Listening (%u)",
                 (unsigned int)CONFIG_SMART_HOME_NODE_GATEWAY_PORT);
        return buffer;
    }
    if (status == SMART_HOME_STATUS_UNKNOWN) {
        return "Not checked";
    }

    snprintf(buffer, size, "Disabled (%d)", status);
    return buffer;
#endif
}

static lv_color_t node_gateway_status_color(const smart_home_agent_app_t *app)
{
#ifndef CONFIG_SMART_HOME_NODE_GATEWAY
    (void)app;
    return SMART_HOME_UI_COLOR_TEXT_MUTED;
#else
    return subsystem_status_color(app ? app->system_status.node_gateway_status :
                                        SMART_HOME_STATUS_UNKNOWN);
#endif
}

static const char *mcp_bridge_status_text(const smart_home_agent_app_t *app,
                                          char *buffer, size_t size)
{
#ifndef CONFIG_SMART_HOME_MCP_BRIDGE
    (void)app;
    (void)buffer;
    (void)size;
    return "Not built";
#else
    int status = app ? app->system_status.mcp_bridge_status :
                       SMART_HOME_STATUS_UNKNOWN;
    smart_home_mcp_state_t state;
    int last_error = 0;

    if (status == SMART_HOME_STATUS_UNKNOWN) {
        return "Not checked";
    }
    if (status != AGENT_OK) {
        snprintf(buffer, size, "Disabled (%d)", status);
        return buffer;
    }
    /* Bridge creation does not access the network. Discovery is a manual
     * operation launched from Settings. */
    if (!app->mcp_bridge
        || smart_home_mcp_bridge_get_state(app->mcp_bridge,
                                           &state,
                                           &last_error) != AGENT_OK) {
        return "Not checked";
    }
    if (state == SMART_HOME_MCP_STATE_CONNECTING) {
        return "Connecting...";
    }
    if (state == SMART_HOME_MCP_STATE_IDLE) {
        return "Manual";
    }
    if (state == SMART_HOME_MCP_STATE_READY) {
        return "Connected";
    }
    snprintf(buffer, size, "Failed (%d)", last_error);
    return buffer;
#endif
}

static lv_color_t mcp_bridge_status_color(const smart_home_agent_app_t *app)
{
#ifndef CONFIG_SMART_HOME_MCP_BRIDGE
    (void)app;
    return SMART_HOME_UI_COLOR_TEXT_MUTED;
#else
    int status = app ? app->system_status.mcp_bridge_status :
                       SMART_HOME_STATUS_UNKNOWN;
    smart_home_mcp_state_t state;

    if (status != AGENT_OK) {
        return subsystem_status_color(status);
    }
    if (app->mcp_bridge
        && smart_home_mcp_bridge_get_state(app->mcp_bridge,
                                           &state,
                                           NULL) == AGENT_OK) {
        if (state == SMART_HOME_MCP_STATE_CONNECTING) {
            return SMART_HOME_UI_COLOR_WARNING;
        }
        if (state == SMART_HOME_MCP_STATE_IDLE) {
            return SMART_HOME_UI_COLOR_TEXT_MUTED;
        }
        if (state == SMART_HOME_MCP_STATE_READY) {
            return SMART_HOME_UI_COLOR_SUCCESS;
        }
        return SMART_HOME_UI_COLOR_DANGER;
    }
    return subsystem_status_color(status);
#endif
}

#ifdef CONFIG_SMART_HOME_MCP_BRIDGE
/* MCP discovery is manually requested, so keep this timer alive for future
 * requests and refresh both the status row and its command button. */
static lv_obj_t *s_mcp_status_label;
static lv_obj_t *s_mcp_discover_button;
static const smart_home_agent_app_t *s_mcp_app;
static lv_timer_t *s_mcp_timer;
static smart_home_lvgl_t *s_mcp_ui;
static smart_home_mcp_state_t s_mcp_last_state;
static int s_mcp_last_state_valid;

static void mcp_status_timer_cb(lv_timer_t *timer)
{
    char buffer[32];
    smart_home_mcp_state_t state;

    (void)timer;
    if (!s_mcp_status_label || !s_mcp_app || !s_mcp_app->mcp_bridge) {
        return;
    }

    lv_label_set_text(s_mcp_status_label,
                      mcp_bridge_status_text(s_mcp_app,
                                             buffer,
                                             sizeof(buffer)));
    lv_obj_set_style_text_color(s_mcp_status_label,
                                mcp_bridge_status_color(s_mcp_app),
                                0);

    if (smart_home_mcp_bridge_get_state(s_mcp_app->mcp_bridge,
                                        &state,
                                        NULL) == AGENT_OK) {
        if (!s_mcp_last_state_valid || s_mcp_last_state != state) {
            s_mcp_last_state = state;
            s_mcp_last_state_valid = 1;
            smart_home_lvgl_refresh_tool_directory(s_mcp_ui);
        }
    }

    if (s_mcp_last_state_valid && s_mcp_discover_button) {
        if (s_mcp_last_state == SMART_HOME_MCP_STATE_CONNECTING) {
            lv_obj_add_state(s_mcp_discover_button, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(s_mcp_discover_button, LV_STATE_DISABLED);
        }
    }
}

void smart_home_lvgl_settings_deinit(void)
{
    if (s_mcp_timer) {
        lv_timer_delete(s_mcp_timer);
        s_mcp_timer = NULL;
    }
    s_mcp_status_label = NULL;
    s_mcp_discover_button = NULL;
    s_mcp_app = NULL;
    s_mcp_ui = NULL;
    s_mcp_last_state_valid = 0;
}
#else
void smart_home_lvgl_settings_deinit(void)
{
}
#endif

static const char *agent_status_text(const smart_home_agent_app_t *app,
                                     char *buffer,
                                     size_t size)
{
    const smart_home_system_status_t *status;

    if (!app) {
        return "Not checked";
    }

    status = &app->system_status;
    if (status->agent_status != AGENT_OK) {
        snprintf(buffer, size, "Init failed (%d)", status->agent_status);
        return buffer;
    }
    if (status->skills_status != AGENT_OK ||
        status->tools_status != AGENT_OK ||
        (network_status_checked(&status->network_status) &&
         !status->network_status.online)) {
        return "Degraded";
    }

    return "Ready";
}

static lv_color_t agent_status_color(const smart_home_agent_app_t *app)
{
    const smart_home_system_status_t *status;

    if (!app) {
        return SMART_HOME_UI_COLOR_TEXT_MUTED;
    }

    status = &app->system_status;
    if (status->agent_status != AGENT_OK) {
        return SMART_HOME_UI_COLOR_WARNING;
    }
    if (status->skills_status != AGENT_OK ||
        status->tools_status != AGENT_OK ||
        (network_status_checked(&status->network_status) &&
         !status->network_status.online)) {
        return SMART_HOME_UI_COLOR_WARNING;
    }

    return SMART_HOME_UI_COLOR_SUCCESS;
}

static void create_system_status_card(lv_obj_t *content,
                                      smart_home_lvgl_t *ui)
{
    const smart_home_agent_app_t *app = ui ? ui->app : NULL;
    const smart_home_system_status_t *status =
        app ? &app->system_status : NULL;
    lv_obj_t *card;
    lv_obj_t *header;
    lv_obj_t *body;
    lv_obj_t *label;
    char buffer[96];
    char value[64];
    int device_count = app ? smart_home_device_count(&app->device_state) : 0;

    card = lv_obj_create(content);
    s_settings_system_card = card;
    lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 6, 0);
    smart_home_lvgl_card_style(card);

    header = lv_obj_create(card);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, lv_pct(100), 28);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(header, 6, 0);
    lv_obj_add_flag(header, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(header,
                        system_status_toggle_event,
                        LV_EVENT_CLICKED,
                        ui);

    smart_home_lvgl_icon_create(header, ICON_STATUS_OK, 18, 18);
    label = smart_home_lvgl_label_create(header,
                                         "系统健康",
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY,
                                         14);
    lv_obj_set_flex_grow(label, 1);
    ui->settings_system_status_toggle =
        smart_home_lvgl_label_create(header,
                                     "+",
                                     SMART_HOME_UI_COLOR_TEXT_SECONDARY,
                                     16);
    lv_obj_set_width(ui->settings_system_status_toggle, 18);
    lv_obj_set_style_text_align(ui->settings_system_status_toggle,
                                LV_TEXT_ALIGN_CENTER,
                                0);

    body = lv_obj_create(card);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 6, 0);
    lv_obj_add_flag(body, LV_OBJ_FLAG_HIDDEN);
    ui->settings_system_status_body = body;

    create_status_row(body,
                      "家庭网络",
                      network_status_text(status ? &status->network_status :
                                                   NULL,
                                          buffer,
                                          sizeof(buffer)),
                      network_status_color(status ? &status->network_status :
                                                    NULL));

    create_status_row(body,
                      "运行平台",
                      status ?
                          smart_home_network_platform_name(
                              status->network_status.platform) :
                          "Unknown",
                      SMART_HOME_UI_COLOR_TEXT_SECONDARY);

    create_status_row(body,
                      "Wi-Fi",
                      wifi_status_text(status ? &status->network_status : NULL,
                                       buffer,
                                       sizeof(buffer)),
                      wifi_status_color(status ? &status->network_status :
                                                 NULL));

    create_status_row(body,
                      "Skills",
                      skills_status_text(app, buffer, sizeof(buffer)),
                      subsystem_status_color(status ? status->skills_status :
                                                 SMART_HOME_STATUS_UNKNOWN));

    create_status_row(body,
                      "工具服务",
                      simple_agent_status_text(status ? status->tools_status :
                                                   SMART_HOME_STATUS_UNKNOWN,
                                               "Ready",
                                               buffer,
                                               sizeof(buffer)),
                      subsystem_status_color(status ? status->tools_status :
                                                 SMART_HOME_STATUS_UNKNOWN));

    create_status_row(body,
                      "模型服务",
                      model_status_text(app, buffer, sizeof(buffer)),
                      subsystem_status_color(status ? status->model_status :
                                                 SMART_HOME_STATUS_UNKNOWN));

    create_status_row(body,
                      "智能管家",
                      agent_status_text(app, buffer, sizeof(buffer)),
                      agent_status_color(app));

    create_status_row(body,
                      "设备网关",
                      node_gateway_status_text(app, buffer, sizeof(buffer)),
                      node_gateway_status_color(app));

#ifdef CONFIG_SMART_HOME_MCP_BRIDGE
    s_mcp_status_label =
        create_status_row(body,
                          "MCP 服务",
                          mcp_bridge_status_text(app, buffer, sizeof(buffer)),
                          mcp_bridge_status_color(app));
    s_mcp_app = app;
    s_mcp_ui = ui;
    s_mcp_last_state_valid = 0;
    if (s_mcp_timer) {
        lv_timer_delete(s_mcp_timer);
        s_mcp_timer = NULL;
    }
    if (s_mcp_status_label && s_mcp_app && s_mcp_app->mcp_bridge) {
        s_mcp_timer = lv_timer_create(mcp_status_timer_cb, 1000, NULL);
    }
    s_mcp_discover_button = lv_btn_create(body);
    lv_obj_remove_style_all(s_mcp_discover_button);
    lv_obj_set_size(s_mcp_discover_button, 112, 32);
    lv_obj_set_style_radius(s_mcp_discover_button, 6, 0);
    smart_home_lvgl_set_bg(s_mcp_discover_button, SMART_HOME_UI_COLOR_PRIMARY);
    lv_obj_add_event_cb(s_mcp_discover_button,
                        mcp_discover_event,
                        LV_EVENT_CLICKED,
                        ui);
    label = smart_home_lvgl_label_create(s_mcp_discover_button,
                                         "发现 MCP",
                                         lv_color_white(),
                                         12);
    lv_obj_center(label);
    ui->settings_mcp_discover_btn = s_mcp_discover_button;
#else
    create_status_row(body,
                      "MCP 服务",
                      mcp_bridge_status_text(app, buffer, sizeof(buffer)),
                      mcp_bridge_status_color(app));
#endif

    snprintf(value, sizeof(value), "%d devices", device_count);
    create_status_row(body,
                      "本地设备",
                      value,
                      SMART_HOME_UI_COLOR_TEXT_SECONDARY);

    if (status && status->last_error[0]) {
        create_status_row(body,
                          "最近错误",
                          status->last_error,
                          SMART_HOME_UI_COLOR_WARNING);
    }
}

#if defined(CONFIG_SMART_HOME_VOICE_TTS) || defined(CONFIG_SMART_HOME_VOICE_ASR) || \
    defined(CONFIG_SMART_HOME_KWS)
static void voice_announce_toggle_event(lv_event_t *event)
{
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)lv_event_get_user_data(event);
    lv_obj_t *body;

    if (!ui) {
        return;
    }

    body = ui->settings_voice_body;
    if (!body) {
        return;
    }

    if (lv_obj_has_flag(body, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(body, LV_OBJ_FLAG_HIDDEN);
        if (ui->settings_voice_toggle) {
            lv_label_set_text(ui->settings_voice_toggle, "-");
        }
    } else {
        lv_obj_add_flag(body, LV_OBJ_FLAG_HIDDEN);
        if (ui->settings_voice_toggle) {
            lv_label_set_text(ui->settings_voice_toggle, "+");
        }
    }
}
#endif

#ifdef CONFIG_SMART_HOME_VOICE_TTS
static void voice_announce_switch_event(lv_event_t *event)
{
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)lv_event_get_user_data(event);
    smart_home_tts_config_t config;

    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED ||
        !ui || !ui->settings_voice_switch) {
        return;
    }

    smart_home_tts_config_load(&config);
    config.enabled = lv_obj_has_state(ui->settings_voice_switch,
                                      LV_STATE_CHECKED) ? 1 : 0;
    if (smart_home_tts_config_save(&config) == AGENT_OK) {
        set_settings_status(ui,
                            config.enabled ? "语音播报已开启" : "语音播报已关闭",
                            SMART_HOME_UI_COLOR_SUCCESS);
    } else {
        set_settings_status(ui, "voice.json 保存失败", SMART_HOME_UI_COLOR_DANGER);
    }

    if (!config.enabled) {
        voice_play_stop();
    }
}

static void voice_announce_stop_event(lv_event_t *event)
{
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)lv_event_get_user_data(event);

    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !ui) {
        return;
    }

    voice_play_stop();
    set_settings_status(ui, "已请求停止播报", SMART_HOME_UI_COLOR_SUCCESS);
}
#endif

#ifdef CONFIG_SMART_HOME_VOICE_ASR
static void voice_asr_switch_event(lv_event_t *event)
{
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)lv_event_get_user_data(event);
    smart_home_asr_config_t config;

    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED ||
        !ui || !ui->settings_asr_switch) {
        return;
    }

    smart_home_asr_config_load(&config);
    config.enabled = lv_obj_has_state(ui->settings_asr_switch,
                                      LV_STATE_CHECKED) ? 1 : 0;
    if (smart_home_asr_config_save(&config) == AGENT_OK) {
        set_settings_status(ui,
                            config.enabled ? "语音输入已开启" : "语音输入已关闭",
                            SMART_HOME_UI_COLOR_SUCCESS);
    } else {
        set_settings_status(ui, "asr.json 保存失败", SMART_HOME_UI_COLOR_DANGER);
    }
}
#endif

#ifdef CONFIG_SMART_HOME_KWS
static void voice_kws_switch_event(lv_event_t *event)
{
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)lv_event_get_user_data(event);
    smart_home_kws_config_t config;

    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED ||
        !ui || !ui->settings_kws_switch) {
        return;
    }

    smart_home_kws_config_load(&config);
    config.enabled = lv_obj_has_state(ui->settings_kws_switch,
                                      LV_STATE_CHECKED) ? 1 : 0;
    if (smart_home_kws_config_save(&config) == AGENT_OK) {
        set_settings_status(ui,
                            config.enabled ? "语音唤醒已开启" : "语音唤醒已关闭",
                            SMART_HOME_UI_COLOR_SUCCESS);
        /* 运行期切换只做暂停/恢复；boot 时关闭的服务需重启后生效。 */
        if (kws_service_running()) {
            kws_service_set_paused(!config.enabled);
        } else if (config.enabled) {
            set_settings_status(ui, "唤醒服务未启动，重启后生效",
                                SMART_HOME_UI_COLOR_WARNING);
        }
    } else {
        set_settings_status(ui, "kws.json 保存失败", SMART_HOME_UI_COLOR_DANGER);
    }
}
#endif

#if defined(CONFIG_SMART_HOME_VOICE_TTS) || defined(CONFIG_SMART_HOME_VOICE_ASR) || \
    defined(CONFIG_SMART_HOME_KWS)
static void create_voice_announce_card(lv_obj_t *content,
                                       smart_home_lvgl_t *ui)
{
    lv_obj_t *card;
    lv_obj_t *header;
    lv_obj_t *body;
    lv_obj_t *row;
    lv_obj_t *label;
    lv_obj_t *button;
    char value[96];

    card = lv_obj_create(content);
    lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 6, 0);
    smart_home_lvgl_card_style(card);

    header = lv_obj_create(card);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, lv_pct(100), 28);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(header, 6, 0);
    lv_obj_add_flag(header, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(header,
                        voice_announce_toggle_event,
                        LV_EVENT_CLICKED,
                        ui);

    smart_home_lvgl_icon_create(header, ICON_STATUS_MICROPHONE, 18, 18);
    label = smart_home_lvgl_label_create(header,
                                         "语音",
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY,
                                         14);
    lv_obj_set_flex_grow(label, 1);
    ui->settings_voice_toggle =
        smart_home_lvgl_label_create(header,
                                     "+",
                                     SMART_HOME_UI_COLOR_TEXT_SECONDARY,
                                     16);
    lv_obj_set_width(ui->settings_voice_toggle, 18);
    lv_obj_set_style_text_align(ui->settings_voice_toggle,
                                LV_TEXT_ALIGN_CENTER,
                                0);

    body = lv_obj_create(card);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 8, 0);
    lv_obj_add_flag(body, LV_OBJ_FLAG_HIDDEN);
    ui->settings_voice_body = body;

#ifdef CONFIG_SMART_HOME_VOICE_TTS
    {
        smart_home_tts_config_t config;

        row = lv_obj_create(body);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row,
                              LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 8, 0);

        label = smart_home_lvgl_label_create(row,
                                             "自动播报 Agent 回复",
                                             SMART_HOME_UI_COLOR_TEXT_PRIMARY,
                                             12);
        lv_obj_set_flex_grow(label, 1);

        ui->settings_voice_switch = lv_switch_create(row);
        lv_obj_set_size(ui->settings_voice_switch, 44, 24);
        smart_home_tts_config_load(&config);
        if (config.enabled) {
            lv_obj_add_state(ui->settings_voice_switch, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(ui->settings_voice_switch,
                            voice_announce_switch_event,
                            LV_EVENT_VALUE_CHANGED,
                            ui);

        snprintf(value, sizeof(value), "%s | %s",
                 config.backend_id[0] ? config.backend_id : "custom",
                 config.model[0] ? config.model : "(未配置)");
        create_status_row(body,
                          "TTS 后端",
                          value,
                          SMART_HOME_UI_COLOR_TEXT_SECONDARY);

        snprintf(value, sizeof(value), "%s | %lu Hz",
                 config.voice[0] ? config.voice : "(默认音色)",
                 (unsigned long)config.sample_rate);
        create_status_row(body,
                          "音色 / 采样率",
                          value,
                          SMART_HOME_UI_COLOR_TEXT_SECONDARY);

        label = smart_home_lvgl_label_create(
            body,
            "端点与密钥在 /data/smart_home/voice.json 与 secrets.json 中配置；"
            "可用 tts_smoke speak 命令验证链路。",
            SMART_HOME_UI_COLOR_TEXT_MUTED,
            10);
        lv_obj_set_width(label, lv_pct(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);

        button = lv_btn_create(body);
        lv_obj_remove_style_all(button);
        lv_obj_set_size(button, 112, 32);
        lv_obj_set_style_radius(button, 6, 0);
        smart_home_lvgl_set_bg(button, SMART_HOME_UI_COLOR_PRIMARY);
        lv_obj_add_event_cb(button,
                            voice_announce_stop_event,
                            LV_EVENT_CLICKED,
                            ui);
        label = smart_home_lvgl_label_create(button,
                                             "停止播报",
                                             lv_color_white(),
                                             12);
        lv_obj_center(label);

        ui->settings_voice_status_label =
            smart_home_lvgl_label_create(body,
                                         "播报状态: 空闲",
                                         SMART_HOME_UI_COLOR_TEXT_MUTED,
                                         10);
    }
#endif

#ifdef CONFIG_SMART_HOME_VOICE_ASR
    {
        smart_home_asr_config_t config;

        row = lv_obj_create(body);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row,
                              LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 8, 0);

        label = smart_home_lvgl_label_create(row,
                                             "语音输入（MiMo ASR）",
                                             SMART_HOME_UI_COLOR_TEXT_PRIMARY,
                                             12);
        lv_obj_set_flex_grow(label, 1);

        ui->settings_asr_switch = lv_switch_create(row);
        lv_obj_set_size(ui->settings_asr_switch, 44, 24);
        smart_home_asr_config_load(&config);
        if (config.enabled) {
            lv_obj_add_state(ui->settings_asr_switch, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(ui->settings_asr_switch,
                            voice_asr_switch_event,
                            LV_EVENT_VALUE_CHANGED,
                            ui);

        snprintf(value, sizeof(value), "语言: %s（zh/en/auto）",
                 config.language);
        create_status_row(body,
                          "识别语言",
                          value,
                          SMART_HOME_UI_COLOR_TEXT_SECONDARY);
    }
#endif

#ifdef CONFIG_SMART_HOME_KWS
    {
        smart_home_kws_config_t config;

        row = lv_obj_create(body);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row,
                              LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 8, 0);

        label = smart_home_lvgl_label_create(row,
                                             "语音唤醒（KWS 常驻）",
                                             SMART_HOME_UI_COLOR_TEXT_PRIMARY,
                                             12);
        lv_obj_set_flex_grow(label, 1);

        ui->settings_kws_switch = lv_switch_create(row);
        lv_obj_set_size(ui->settings_kws_switch, 44, 24);
        smart_home_kws_config_load(&config);
        if (config.enabled) {
            lv_obj_add_state(ui->settings_kws_switch, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(ui->settings_kws_switch,
                            voice_kws_switch_event,
                            LV_EVENT_VALUE_CHANGED,
                            ui);

        create_status_row(body,
                          "唤醒服务",
                          kws_service_running() ? "运行中" : "未启动",
                          SMART_HOME_UI_COLOR_TEXT_SECONDARY);

        label = smart_home_lvgl_label_create(
            body,
            "唤醒词模型特征前端尚未与训练管线对拍，真机唤醒率数据"
            "仅供参考；可用 kws_smoke 命令验证模型。",
            SMART_HOME_UI_COLOR_TEXT_MUTED,
            10);
        lv_obj_set_width(label, lv_pct(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    }
#endif
}
#endif

/* 更多页卡片跳转入口：展开对应卡并滚动到可见位置。 */
void smart_home_lvgl_settings_focus(smart_home_lvgl_t *ui, int section)
{
    lv_obj_t *card;
    lv_obj_t *body;
    lv_obj_t **toggle_slot;

    if (!ui) {
        return;
    }

    switch (section) {
    case SMART_HOME_SETTINGS_FOCUS_MODEL:
        card = s_settings_model_card;
        body = ui->settings_model_api_body;
        toggle_slot = &ui->settings_model_api_toggle;
        break;
    case SMART_HOME_SETTINGS_FOCUS_TOOLS:
        card = s_settings_tool_card;
        body = ui->settings_tool_directory_body;
        toggle_slot = &ui->settings_tool_directory_toggle;
        break;
    case SMART_HOME_SETTINGS_FOCUS_SYSTEM:
    default:
        card = s_settings_system_card;
        body = ui->settings_system_status_body;
        toggle_slot = &ui->settings_system_status_toggle;
        break;
    }

    if (card == NULL) {
        return;
    }

    if (body && lv_obj_has_flag(body, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(body, LV_OBJ_FLAG_HIDDEN);
        if (*toggle_slot) {
            lv_label_set_text(*toggle_slot, "-");
        }
    }

    lv_obj_update_layout(card);
    lv_obj_scroll_to_view(card, LV_ANIM_OFF);
}

void smart_home_lvgl_build_settings_screen(smart_home_lvgl_t *ui)
{
    lv_obj_t *screen;
    lv_obj_t *content;
    lv_obj_t *card;
    lv_obj_t *model_body;
    lv_obj_t *button;
    lv_obj_t *label;
    char timeout_text[16];
    char max_tokens_text[16];
    char backend_options[SMART_HOME_BACKEND_OPTIONS_CAP];
    const smart_home_model_config_t *config;
    int selected_backend;
    int content_w = smart_home_lvgl_content_w();
    int content_y = SMART_HOME_TOPBAR_H + (smart_home_lvgl_compact() ? 8 : 12);
    int content_h = smart_home_lvgl_disp_h() - SMART_HOME_NAV_H -
                    SMART_HOME_NAV_BOTTOM_PAD - content_y - 8;

    screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(screen);
    smart_home_lvgl_set_bg(screen, SMART_HOME_UI_COLOR_BG);
    ui->screen_settings = screen;
    smart_home_lvgl_build_top_bar(screen, ui, "系统设置");

    content = lv_obj_create(screen);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, content_w, content_h);
    lv_obj_align(content,
                 LV_ALIGN_TOP_MID,
                 0,
                 content_y);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 8, 0);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);
    smart_home_lvgl_set_bg(content, SMART_HOME_UI_COLOR_BG);
    ui->settings_page = content;

    card = lv_obj_create(content);
    s_settings_model_card = card;
    lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, 0);
    smart_home_lvgl_card_style(card);

    /* "Model API" header: icon + label + collapse toggle */
    {
        lv_obj_t *header = lv_obj_create(card);
        lv_obj_remove_style_all(header);
        lv_obj_set_size(header, lv_pct(100), 28);
        lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(header, 0, 0);
        lv_obj_set_style_pad_all(header, 0, 0);
        lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(header,
                              LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(header, 6, 0);
        lv_obj_add_flag(header, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(header,
                            model_api_toggle_event,
                            LV_EVENT_CLICKED,
                            ui);

        smart_home_lvgl_icon_create(header, ICON_SETTING_AI, 18, 18);
        label = smart_home_lvgl_label_create(header,
                                             "模型服务",
                                             SMART_HOME_UI_COLOR_TEXT_PRIMARY,
                                             14);
        lv_obj_set_flex_grow(label, 1);

        ui->settings_model_api_toggle =
            smart_home_lvgl_label_create(header,
                                         "+",
                                         SMART_HOME_UI_COLOR_TEXT_SECONDARY,
                                         16);
        lv_obj_set_width(ui->settings_model_api_toggle, 18);
        lv_obj_set_style_text_align(ui->settings_model_api_toggle,
                                    LV_TEXT_ALIGN_CENTER,
                                    0);
    }

    model_body = lv_obj_create(card);
    lv_obj_remove_style_all(model_body);
    lv_obj_set_size(model_body, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(model_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(model_body, 8, 0);
    lv_obj_add_flag(model_body, LV_OBJ_FLAG_HIDDEN);
    ui->settings_model_api_body = model_body;

    config = ui->app ? &ui->app->model_config : NULL;
    snprintf(timeout_text,
             sizeof(timeout_text),
             "%d",
             config ? config->timeout_ms : 30000);
    snprintf(max_tokens_text,
             sizeof(max_tokens_text),
             "%lu",
             (unsigned long)(config ? config->max_output_tokens : 512u));
    selected_backend = smart_home_llm_backend_index_for_config(config);
    if (selected_backend < 0) {
        selected_backend = 0;
    }
    build_backend_dropdown_options(backend_options, sizeof(backend_options));

    {
        lv_obj_t *wrap;
        lv_obj_t *field_label;

        wrap = lv_obj_create(model_body);
        lv_obj_remove_style_all(wrap);
        lv_obj_set_size(wrap,
                        lv_pct(100),
                        smart_home_lvgl_compact() ? 54 : 58);
        lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(wrap, 4, 0);

        field_label = smart_home_lvgl_label_create(
            wrap,
                              "AI 后端",
            SMART_HOME_UI_COLOR_TEXT_SECONDARY,
            12);
        lv_obj_set_width(field_label, lv_pct(100));

        ui->settings_backend_dd = lv_dropdown_create(wrap);
        lv_dropdown_set_options(ui->settings_backend_dd,
                                backend_options[0] ? backend_options : "Custom");
        lv_dropdown_set_selected(ui->settings_backend_dd,
                                 (uint16_t)selected_backend);
        lv_obj_set_size(ui->settings_backend_dd, lv_pct(100), 32);
        lv_obj_set_style_text_font(ui->settings_backend_dd,
                                   smart_home_lvgl_font(12),
                                   0);
        lv_obj_add_event_cb(ui->settings_backend_dd,
                            backend_dropdown_event,
                            LV_EVENT_VALUE_CHANGED,
                            ui);
    }

    ui->settings_host_input =
        create_settings_input(model_body,
                              ui,
                              "服务地址",
                              config ? config->host : "",
                              sizeof(((smart_home_model_config_t *)0)->host) - 1u,
                              0);
    ui->settings_path_input =
        create_settings_input(model_body,
                              ui,
                              "请求路径",
                              config ? config->path : "",
                              sizeof(((smart_home_model_config_t *)0)->path) - 1u,
                              0);
    ui->settings_port_input =
        create_settings_input(model_body,
                              ui,
                              "端口",
                              config ? config->port : "443",
                              sizeof(((smart_home_model_config_t *)0)->port) - 1u,
                              0);
    ui->settings_model_input =
        create_settings_input(model_body,
                              ui,
                              "模型",
                              config ? config->model : "",
                              sizeof(((smart_home_model_config_t *)0)->model) - 1u,
                              0);
    ui->settings_api_key_input =
        create_settings_input(model_body,
                              ui,
                              "API 密钥（仅自定义后端）",
                              "由 secrets.json 安全管理",
                              sizeof(((smart_home_model_config_t *)0)->api_key) - 1u,
                              1);
    ui->settings_timeout_input =
        create_settings_input(model_body,
                              ui,
                              "请求超时（毫秒）",
                              timeout_text,
                              8,
                              0);
    ui->settings_max_tokens_input =
        create_settings_input(model_body,
                              ui,
                              "最大输出 Token",
                              max_tokens_text,
                              8,
                              0);

    button = lv_btn_create(model_body);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, 96, 34);
    lv_obj_set_style_radius(button, 8, 0);
    smart_home_lvgl_set_bg(button, SMART_HOME_UI_COLOR_PRIMARY);
    lv_obj_add_event_cb(button, apply_settings_event, LV_EVENT_CLICKED, ui);
    label = smart_home_lvgl_label_create(button, "应用", lv_color_white(), 12);
    lv_obj_center(label);

    ui->settings_status_label =
        smart_home_lvgl_label_create(model_body,
                                     "修改仅在当前运行会话内生效。",
                                     SMART_HOME_UI_COLOR_TEXT_MUTED,
                                     12);
    lv_label_set_long_mode(ui->settings_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ui->settings_status_label, lv_pct(100));

    {
        const smart_home_llm_backend_preset_t *preset =
            smart_home_llm_backend_get((size_t)selected_backend);
        update_api_key_input_for_backend(ui, preset);
        show_backend_key_status(ui, preset);
    }

    create_tool_directory_card(content, ui);

    create_system_status_card(content, ui);

#if defined(CONFIG_SMART_HOME_VOICE_TTS) || defined(CONFIG_SMART_HOME_VOICE_ASR) || \
    defined(CONFIG_SMART_HOME_KWS)
    create_voice_announce_card(content, ui);
#endif

    ui->settings_keyboard = lv_keyboard_create(screen);
    smart_home_lvgl_style_keyboard(ui->settings_keyboard);
    lv_obj_add_event_cb(ui->settings_keyboard,
                        settings_keyboard_event,
                        LV_EVENT_ALL,
                        ui);
    layout_settings_keyboard(ui, 0);

    card = lv_obj_create(content);
    lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
    smart_home_lvgl_card_style(card);
    label = smart_home_lvgl_label_create(
        card,
        "本地设备服务\n设备控制保留在原生服务，智能管家页展示工具调用轨迹。",
        SMART_HOME_UI_COLOR_TEXT_SECONDARY,
        12);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, content_w - 44);

    smart_home_lvgl_build_nav_bar(screen, ui);
}
