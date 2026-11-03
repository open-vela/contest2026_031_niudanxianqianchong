/* SPDX-License-Identifier: Apache-2.0 */

#include "smart_home_scene_catalog.h"

#include "../config/cjson_compat.h"

#include <ctype.h>
#include <string.h>

#define SCENE_CATALOG_START "<!-- scene_catalog:start -->"
#define SCENE_CATALOG_END "<!-- scene_catalog:end -->"

static int only_whitespace(const char *cursor, const char *end)
{
    while (cursor && cursor < end && isspace((unsigned char)*cursor)) {
        cursor++;
    }
    return cursor == end;
}

static int copy_string(const cJSON *item, char *output, size_t output_size)
{
    const char *value;
    size_t length;

    if (!item || !output || output_size < 2u || !cJSON_IsString(item)) {
        return AGENT_ERROR_PARSE;
    }
    value = cJSON_GetStringValue(item);
    if (!value || !value[0]) {
        return AGENT_ERROR_PARSE;
    }
    length = strlen(value);
    if (length >= output_size) {
        return AGENT_ERROR_LIMIT;
    }
    memcpy(output, value, length + 1u);
    return AGENT_OK;
}

static int json_int(const cJSON *item, int minimum, int maximum, int *output)
{
    if (!item || !output || !cJSON_IsNumber(item)
        || item->valuedouble != (double)item->valueint
        || item->valueint < minimum || item->valueint > maximum) {
        return AGENT_ERROR_PARSE;
    }
    *output = item->valueint;
    return AGENT_OK;
}

static int json_bool(const cJSON *item, int *output)
{
    if (!item || !output || !cJSON_IsBool(item)) {
        return AGENT_ERROR_PARSE;
    }
    *output = cJSON_IsTrue(item) ? 1 : 0;
    return AGENT_OK;
}

static int action_matches(const smart_home_scene_action_t *left,
                          const smart_home_scene_action_t *right)
{
    return left->device_type == right->device_type
        && strcmp(left->room, right->room) == 0;
}

static int parse_action(const cJSON *item, smart_home_scene_action_t *action)
{
    const cJSON *type;
    const cJSON *room;
    const cJSON *on;
    const char *type_name;
    int ret;

    if (!item || !action || !cJSON_IsObject(item)) {
        return AGENT_ERROR_PARSE;
    }
    memset(action, 0, sizeof(*action));
    type = cJSON_GetObjectItemCaseSensitive(item, "type");
    room = cJSON_GetObjectItemCaseSensitive(item, "room");
    on = cJSON_GetObjectItemCaseSensitive(item, "on");
    if (!cJSON_IsString(type)) {
        return AGENT_ERROR_PARSE;
    }
    type_name = cJSON_GetStringValue(type);
    ret = copy_string(room, action->room, sizeof(action->room));
    if (ret != AGENT_OK) {
        return ret;
    }
    ret = json_bool(on, &action->on);
    if (ret != AGENT_OK) {
        return ret;
    }

    if (strcmp(type_name, "light") == 0) {
        action->device_type = SMART_HOME_DEVICE_LIGHT;
        return json_int(cJSON_GetObjectItemCaseSensitive(item, "brightness"),
                        0, 100, &action->brightness);
    }
    if (strcmp(type_name, "ac") == 0) {
        const char *mode;
        const char *fan_speed;

        action->device_type = SMART_HOME_DEVICE_AC;
        mode = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(item, "mode"));
        fan_speed = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(item, "fanSpeed"));
        action->ac_mode = smart_home_ac_parse_mode(mode);
        action->ac_fan_speed = smart_home_ac_parse_fan_speed(fan_speed);
        if (action->ac_mode < 0 || action->ac_fan_speed < 0) {
            return AGENT_ERROR_PARSE;
        }
        return json_int(cJSON_GetObjectItemCaseSensitive(item, "temperature"),
                        16, 30, &action->temperature);
    }
    return AGENT_ERROR_PARSE;
}

static int parse_scene(const cJSON *item, smart_home_scene_t *scene)
{
    const cJSON *actions;
    int count;
    int i;
    int ret;

    if (!item || !scene || !cJSON_IsObject(item)) {
        return AGENT_ERROR_PARSE;
    }
    memset(scene, 0, sizeof(*scene));
    ret = copy_string(cJSON_GetObjectItemCaseSensitive(item, "id"),
                      scene->id, sizeof(scene->id));
    if (ret != AGENT_OK) {
        return ret;
    }
    actions = cJSON_GetObjectItemCaseSensitive(item, "actions");
    if (!cJSON_IsArray(actions)) {
        return AGENT_ERROR_PARSE;
    }
    count = cJSON_GetArraySize(actions);
    if (count <= 0 || count > (int)SMART_HOME_SCENE_CATALOG_MAX_ACTIONS) {
        return AGENT_ERROR_LIMIT;
    }
    for (i = 0; i < count; i++) {
        int j;

        ret = parse_action(cJSON_GetArrayItem(actions, i), &scene->actions[i]);
        if (ret != AGENT_OK) {
            return ret;
        }
        for (j = 0; j < i; j++) {
            if (action_matches(&scene->actions[i], &scene->actions[j])) {
                return AGENT_ERROR_PARSE;
            }
        }
    }
    scene->action_count = (size_t)count;
    return AGENT_OK;
}

int smart_home_scene_catalog_load(const char *skill_text,
                                  smart_home_scene_catalog_t *catalog)
{
    const char *block_start;
    const char *json_start;
    const char *block_end;
    const char *parse_end = NULL;
    cJSON *root = NULL;
    const cJSON *version;
    const cJSON *scenes;
    int scene_count;
    int i;
    int ret = AGENT_ERROR_PARSE;

    if (!skill_text || !catalog) {
        return AGENT_ERROR_INVALID;
    }
    memset(catalog, 0, sizeof(*catalog));
    block_start = strstr(skill_text, SCENE_CATALOG_START);
    if (!block_start) {
        return AGENT_ERROR_NOT_FOUND;
    }
    block_start += strlen(SCENE_CATALOG_START);
    block_end = strstr(block_start, SCENE_CATALOG_END);
    json_start = strchr(block_start, '{');
    if (!block_end || !json_start || json_start >= block_end) {
        return AGENT_ERROR_PARSE;
    }
    root = cJSON_ParseWithLengthOpts(json_start,
                                     (size_t)(block_end - json_start),
                                     &parse_end, 0);
    if (!root || !parse_end || !only_whitespace(parse_end, block_end)
        || !cJSON_IsObject(root)) {
        goto out;
    }
    version = cJSON_GetObjectItemCaseSensitive(root, "version");
    scenes = cJSON_GetObjectItemCaseSensitive(root, "scenes");
    if (!cJSON_IsNumber(version) || version->valueint != 1
        || version->valuedouble != 1.0 || !cJSON_IsArray(scenes)) {
        goto out;
    }
    scene_count = cJSON_GetArraySize(scenes);
    if (scene_count <= 0 || scene_count > (int)SMART_HOME_SCENE_CATALOG_MAX_SCENES) {
        ret = AGENT_ERROR_LIMIT;
        goto out;
    }
    for (i = 0; i < scene_count; i++) {
        int j;

        ret = parse_scene(cJSON_GetArrayItem(scenes, i), &catalog->scenes[i]);
        if (ret != AGENT_OK) {
            goto out;
        }
        for (j = 0; j < i; j++) {
            if (strcmp(catalog->scenes[i].id, catalog->scenes[j].id) == 0) {
                ret = AGENT_ERROR_PARSE;
                goto out;
            }
        }
    }
    catalog->version = SMART_HOME_SCENE_CATALOG_VERSION;
    catalog->scene_count = (size_t)scene_count;
    ret = AGENT_OK;

out:
    cJSON_Delete(root);
    if (ret != AGENT_OK) {
        memset(catalog, 0, sizeof(*catalog));
    }
    return ret;
}
