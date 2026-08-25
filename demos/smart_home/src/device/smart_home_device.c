#include "smart_home_device.h"

#include <stdio.h>
#include <string.h>

static int room_is(const char *room, const char *name)
{
    return room && strcmp(room, name) == 0;
}

static int valid_room(const char *room)
{
    return room_is(room, "living_room") || room_is(room, "bedroom");
}

static int clamp_brightness(int brightness)
{
    if (brightness < 0) {
        return 0;
    }
    if (brightness > 100) {
        return 100;
    }
    return brightness;
}

static int clamp_temperature(int temperature)
{
    if (temperature < 16) {
        return 16;
    }
    if (temperature > 30) {
        return 30;
    }
    return temperature;
}

static int clamp_env_temperature(int temperature)
{
    if (temperature < 0) {
        return 0;
    }
    if (temperature > 45) {
        return 45;
    }
    return temperature;
}

static int clamp_humidity(int humidity)
{
    if (humidity < 0) {
        return 0;
    }
    if (humidity > 100) {
        return 100;
    }
    return humidity;
}

static int clamp_ambient_light(int ambient_light)
{
    if (ambient_light < 0) {
        return 0;
    }
    if (ambient_light > 1000) {
        return 1000;
    }
    return ambient_light;
}

static int clamp_ac_mode(int mode)
{
    return (mode < 0 || mode > 4) ? 0 : mode;
}

static int clamp_ac_fan_speed(int speed)
{
    return (speed < 0 || speed > 3) ? 3 : speed;
}

static void copy_json_safe_name(char *dest,
                                size_t dest_size,
                                const char *src,
                                const char *fallback)
{
    const char *input = (src && src[0]) ? src : fallback;
    size_t i;

    if (!dest || dest_size == 0u) {
        return;
    }

    for (i = 0; i + 1u < dest_size && input && input[i]; i++) {
        char ch = input[i];

        if (ch == '"' || ch == '\\' || (unsigned char)ch < 0x20u) {
            ch = ' ';
        }
        dest[i] = ch;
    }
    dest[i] = '\0';
}

const char *smart_home_device_type_name(smart_home_device_type_t type)
{
    return type == SMART_HOME_DEVICE_AC ? "ac" : "light";
}

smart_home_device_type_t smart_home_device_type_from_index(int index)
{
    return index == 1 ? SMART_HOME_DEVICE_AC : SMART_HOME_DEVICE_LIGHT;
}

int smart_home_device_type_to_index(smart_home_device_type_t type)
{
    return type == SMART_HOME_DEVICE_AC ? 1 : 0;
}

const char *smart_home_room_from_index(int index)
{
    return index == 1 ? "bedroom" : "living_room";
}

int smart_home_room_to_index(const char *room)
{
    return room_is(room, "bedroom") ? 1 : 0;
}

const char *smart_home_ac_mode_name(int mode)
{
    switch (mode) {
    case 0:
        return "cool";
    case 1:
        return "heat";
    case 2:
        return "dry";
    case 3:
        return "fan";
    case 4:
        return "auto";
    default:
        return "cool";
    }
}

const char *smart_home_ac_fan_speed_name(int speed)
{
    switch (speed) {
    case 0:
        return "low";
    case 1:
        return "medium";
    case 2:
        return "high";
    case 3:
        return "auto";
    default:
        return "auto";
    }
}

int smart_home_ac_parse_mode(const char *name)
{
    if (!name) {
        return -1;
    }
    if (strcmp(name, "cool") == 0) {
        return 0;
    }
    if (strcmp(name, "heat") == 0) {
        return 1;
    }
    if (strcmp(name, "dry") == 0) {
        return 2;
    }
    if (strcmp(name, "fan") == 0) {
        return 3;
    }
    if (strcmp(name, "auto") == 0) {
        return 4;
    }
    return -1;
}

int smart_home_ac_parse_fan_speed(const char *name)
{
    if (!name) {
        return -1;
    }
    if (strcmp(name, "low") == 0) {
        return 0;
    }
    if (strcmp(name, "medium") == 0) {
        return 1;
    }
    if (strcmp(name, "high") == 0) {
        return 2;
    }
    if (strcmp(name, "auto") == 0) {
        return 3;
    }
    return -1;
}

static void init_device_defaults(smart_home_device_t *device,
                                 const char *room,
                                 const char *name,
                                 smart_home_device_type_t type)
{
    memset(device, 0, sizeof(*device));
    device->used = 1;
    strncpy(device->room, room, sizeof(device->room) - 1);
    copy_json_safe_name(device->name, sizeof(device->name), name, "");
    device->type = type;
    device->temperature = 26;
    device->ac_mode = 0;
    device->ac_fan_speed = 3;
}

int smart_home_device_add(smart_home_state_t *state,
                          const char *room,
                          const char *name,
                          smart_home_device_type_t type,
                          int *device_id)
{
    int i;
    const char *fallback_name;

    if (!state || !valid_room(room) ||
        (type != SMART_HOME_DEVICE_LIGHT && type != SMART_HOME_DEVICE_AC)) {
        return AGENT_ERROR_INVALID;
    }

    fallback_name = type == SMART_HOME_DEVICE_AC ? "New AC" : "New Light";
    for (i = 0; i < SMART_HOME_MAX_DEVICES; i++) {
        smart_home_device_t *device = &state->devices[i];

        if (device->used) {
            continue;
        }

        init_device_defaults(device,
                             room,
                             name && name[0] ? name : fallback_name,
                             type);
        device->id = state->next_device_id++;
        if (device_id) {
            *device_id = device->id;
        }
        return AGENT_OK;
    }

    return AGENT_ERROR_LIMIT;
}

int smart_home_device_update_meta(smart_home_state_t *state,
                                  int device_id,
                                  const char *room,
                                  const char *name)
{
    smart_home_device_t *device;

    if (!state || !valid_room(room) || !name || name[0] == '\0') {
        return AGENT_ERROR_INVALID;
    }

    device = smart_home_device_find_by_id(state, device_id);
    if (!device) {
        return AGENT_ERROR_NOTFOUND;
    }

    strncpy(device->room, room, sizeof(device->room) - 1);
    device->room[sizeof(device->room) - 1] = '\0';
    copy_json_safe_name(device->name, sizeof(device->name), name, "");
    return AGENT_OK;
}

int smart_home_device_remove(smart_home_state_t *state, int device_id)
{
    smart_home_device_t *device;

    if (!state) {
        return AGENT_ERROR_INVALID;
    }

    device = smart_home_device_find_by_id(state, device_id);
    if (!device) {
        return AGENT_ERROR_NOTFOUND;
    }

    memset(device, 0, sizeof(*device));
    return AGENT_OK;
}

int smart_home_device_count(const smart_home_state_t *state)
{
    int count = 0;
    int i;

    if (!state) {
        return 0;
    }

    for (i = 0; i < SMART_HOME_MAX_DEVICES; i++) {
        if (state->devices[i].used) {
            count++;
        }
    }
    return count;
}

smart_home_device_t *smart_home_device_get_by_slot(smart_home_state_t *state,
                                                   int slot)
{
    int count = 0;
    int i;

    if (!state || slot < 0) {
        return NULL;
    }

    for (i = 0; i < SMART_HOME_MAX_DEVICES; i++) {
        if (!state->devices[i].used) {
            continue;
        }
        if (count == slot) {
            return &state->devices[i];
        }
        count++;
    }
    return NULL;
}

const smart_home_device_t *smart_home_device_get_const_by_slot(
    const smart_home_state_t *state,
    int slot)
{
    return smart_home_device_get_by_slot((smart_home_state_t *)state, slot);
}

smart_home_device_t *smart_home_device_find_by_id(smart_home_state_t *state,
                                                  int id)
{
    int i;

    if (!state) {
        return NULL;
    }

    for (i = 0; i < SMART_HOME_MAX_DEVICES; i++) {
        if (state->devices[i].used && state->devices[i].id == id) {
            return &state->devices[i];
        }
    }
    return NULL;
}

smart_home_device_t *smart_home_device_find_first(smart_home_state_t *state,
                                                  const char *room,
                                                  smart_home_device_type_t type)
{
    int i;

    if (!state || !room) {
        return NULL;
    }

    for (i = 0; i < SMART_HOME_MAX_DEVICES; i++) {
        smart_home_device_t *device = &state->devices[i];

        if (device->used && device->type == type &&
            strcmp(device->room, room) == 0) {
            return device;
        }
    }
    return NULL;
}

void smart_home_device_init(smart_home_state_t *state)
{
    if (!state) {
        return;
    }

    memset(state, 0, sizeof(*state));
    state->next_device_id = 1;
    state->env_temperature = 26;
    state->env_humidity = 45;
    state->env_light = 300;
    smart_home_device_add(state,
                          "living_room",
                          "Living Light",
                          SMART_HOME_DEVICE_LIGHT,
                          NULL);
    smart_home_device_add(state,
                          "bedroom",
                          "Bedroom Light",
                          SMART_HOME_DEVICE_LIGHT,
                          NULL);
    smart_home_device_add(state,
                          "bedroom",
                          "Bedroom AC",
                          SMART_HOME_DEVICE_AC,
                          NULL);
}

int smart_home_device_set_light(smart_home_state_t *state,
                                const char *room,
                                int on,
                                int brightness)
{
    smart_home_device_t *device;
    int level;

    if (!state || !room) {
        return AGENT_ERROR_INVALID;
    }

    device = smart_home_device_find_first(state,
                                          room,
                                          SMART_HOME_DEVICE_LIGHT);
    if (!device) {
        return AGENT_ERROR_NOTFOUND;
    }

    level = clamp_brightness(brightness);
    device->on = on ? 1 : 0;
    device->brightness = device->on ? level : 0;
    return AGENT_OK;
}

int smart_home_device_set_ac(smart_home_state_t *state,
                             const char *room,
                             int on,
                             int mode,
                             int fan_speed,
                             int temperature)
{
    smart_home_device_t *device; /* 不在核心 Guard 中做关键词判断 */

    if (!state || !room) {
        return AGENT_ERROR_INVALID;
    }

    device = smart_home_device_find_first(state, room, SMART_HOME_DEVICE_AC);
    if (!device) {
        return AGENT_ERROR_NOTFOUND;
    }

    device->on = on ? 1 : 0;
    if (!device->on) {
        return AGENT_OK;
    }

    device->ac_mode = clamp_ac_mode(mode);
    device->ac_fan_speed = clamp_ac_fan_speed(fan_speed);
    if (device->ac_mode != 2 && device->ac_mode != 3) {
        device->temperature = clamp_temperature(temperature);
    }
    return AGENT_OK;
}

int smart_home_device_set_environment(smart_home_state_t *state,
                                      int temperature,
                                      int humidity,
                                      int ambient_light)
{
    if (!state) {
        return AGENT_ERROR_INVALID;
    }

    state->env_temperature = clamp_env_temperature(temperature);
    state->env_humidity = clamp_humidity(humidity);
    state->env_light = clamp_ambient_light(ambient_light);
    return AGENT_OK;
}

static int append_text(char *buffer, size_t size, size_t *off, const char *text)
{
    int n;

    if (*off >= size) {
        return AGENT_ERROR_LIMIT;
    }

    n = snprintf(buffer + *off, size - *off, "%s", text);
    if (n < 0 || (size_t)n >= size - *off) {
        return AGENT_ERROR_LIMIT;
    }

    *off += (size_t)n;
    return AGENT_OK;
}

static int append_device_json(char *buffer,
                              size_t size,
                              size_t *off,
                              const smart_home_device_t *device,
                              int first)
{
    int n;

    if (*off >= size) {
        return AGENT_ERROR_LIMIT;
    }

    if (device->type == SMART_HOME_DEVICE_AC) {
        n = snprintf(buffer + *off,
                     size - *off,
                     "%s{\"id\":%d,\"room\":\"%s\",\"name\":\"%s\","
                     "\"type\":\"ac\",\"on\":%s,\"temperature\":%d,"
                     "\"mode\":\"%s\",\"fan_speed\":\"%s\"}",
                     first ? "" : ",",
                     device->id,
                     device->room,
                     device->name,
                     device->on ? "true" : "false",
                     device->temperature,
                     smart_home_ac_mode_name(device->ac_mode),
                     smart_home_ac_fan_speed_name(device->ac_fan_speed));
    } else {
        n = snprintf(buffer + *off,
                     size - *off,
                     "%s{\"id\":%d,\"room\":\"%s\",\"name\":\"%s\","
                     "\"type\":\"light\",\"on\":%s,\"brightness\":%d}",
                     first ? "" : ",",
                     device->id,
                     device->room,
                     device->name,
                     device->on ? "true" : "false",
                     device->brightness);
    }

    if (n < 0 || (size_t)n >= size - *off) {
        return AGENT_ERROR_LIMIT;
    }

    *off += (size_t)n;
    return AGENT_OK;
}

int smart_home_device_build_status_json(const smart_home_state_t *state,
                                        char *buffer,
                                        size_t buffer_size)
{
    size_t off = 0u;
    int first = 1;
    int i;
    int ret;
    int n;

    if (!state || !buffer || buffer_size == 0u) {
        return AGENT_ERROR_INVALID;
    }

    ret = append_text(buffer, buffer_size, &off, "{\"devices\":[");
    if (ret != AGENT_OK) {
        return ret;
    }

    for (i = 0; i < SMART_HOME_MAX_DEVICES; i++) {
        const smart_home_device_t *device = &state->devices[i];

        if (!device->used) {
            continue;
        }
        ret = append_device_json(buffer, buffer_size, &off, device, first);
        if (ret != AGENT_OK) {
            return ret;
        }
        first = 0;
    }

    n = snprintf(buffer + off,
                 buffer_size - off,
                 "],\"environment\":{\"temperature\":%d,"
                 "\"humidity\":%d,\"ambient_light\":%d}}",
                 state->env_temperature,
                 state->env_humidity,
                 state->env_light);
    if (n < 0 || (size_t)n >= buffer_size - off) {
        return AGENT_ERROR_LIMIT;
    }

    return AGENT_OK;
}

static int build_device_context(char *buffer,
                                size_t buffer_size,
                                size_t *written,
                                void *user_data)
{
    smart_home_state_t *state = (smart_home_state_t *)user_data;
    char status[1024];
    int ret;
    int n;

    if (!buffer || !written || !state) {
        return AGENT_ERROR_INVALID;
    }

    ret = smart_home_device_build_status_json(state, status, sizeof(status));
    if (ret != AGENT_OK) {
        return ret;
    }

    n = snprintf(buffer,
                 buffer_size,
                 "Current virtual home state JSON:\n%s\n",
                 status);
    if (n < 0 || (size_t)n >= buffer_size) {
        return AGENT_ERROR_CONTEXT_OVERFLOW;
    }

    *written = (size_t)n;
    return AGENT_OK;
}

int smart_home_device_register_context(agent_t *agent,
                                       smart_home_state_t *state)
{
    agent_context_provider_t provider;

    if (!agent || !state) {
        return AGENT_ERROR_INVALID;
    }

    memset(&provider, 0, sizeof(provider));
    provider.name = "smart_home_state";
    provider.priority = 80u;
    provider.flags = AGENT_CONTEXT_FLAG_OPTIONAL;
    provider.build = build_device_context;
    provider.user_data = state;

    return agent_register_context_provider(agent, &provider);
}

/* ── 序列化（state.json 持久化用） ── */

cJSON *smart_home_device_state_to_json(const smart_home_state_t *state)
{
    cJSON *root;
    cJSON *devices;
    int count;
    int i;

    if (!state) {
        return NULL;
    }

    root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddNumberToObject(root, "version", 1);

    devices = cJSON_AddArrayToObject(root, "devices");
    if (!devices) {
        cJSON_Delete(root);
        return NULL;
    }

    count = smart_home_device_count(state);
    for (i = 0; i < count; i++) {
        const smart_home_device_t *device =
            smart_home_device_get_const_by_slot(state, i);
        cJSON *item;

        if (!device) {
            continue;
        }
        item = cJSON_CreateObject();
        if (!item) {
            cJSON_Delete(root);
            return NULL;
        }
        cJSON_AddNumberToObject(item, "id", device->id);
        cJSON_AddStringToObject(item, "room", device->room);
        cJSON_AddStringToObject(item, "name", device->name);
        cJSON_AddStringToObject(item, "type",
                                smart_home_device_type_name(device->type));
        cJSON_AddBoolToObject(item, "on", device->on ? 1 : 0);
        if (device->type == SMART_HOME_DEVICE_LIGHT) {
            cJSON_AddNumberToObject(item, "brightness", device->brightness);
        } else if (device->type == SMART_HOME_DEVICE_AC) {
            cJSON_AddNumberToObject(item, "temperature", device->temperature);
            cJSON_AddStringToObject(item, "mode",
                                    smart_home_ac_mode_name(device->ac_mode));
            cJSON_AddStringToObject(item, "fan_speed",
                                    smart_home_ac_fan_speed_name(
                                        device->ac_fan_speed));
        }
        cJSON_AddItemToArray(devices, item);
    }

    {
        cJSON *env = cJSON_CreateObject();
        if (!env) {
            cJSON_Delete(root);
            return NULL;
        }
        cJSON_AddNumberToObject(env, "temperature", state->env_temperature);
        cJSON_AddNumberToObject(env, "humidity", state->env_humidity);
        cJSON_AddNumberToObject(env, "ambient_light", state->env_light);
        cJSON_AddItemToObject(root, "environment", env);
    }

    return root;
}

int smart_home_device_state_from_json(smart_home_state_t *state,
                                      const cJSON *root)
{
    const cJSON *devices;
    const cJSON *env;
    const cJSON *item;
    smart_home_state_t tmp;

    if (!state || !root || !cJSON_IsObject(root)) {
        return AGENT_ERROR_INVALID;
    }

    devices = cJSON_GetObjectItemCaseSensitive(root, "devices");
    if (!devices || !cJSON_IsArray(devices)) {
        return AGENT_ERROR_PARSE;
    }

    /* 先解析到临时 state，全部合法才提交，避免半恢复 */
    memset(&tmp, 0, sizeof(tmp));
    tmp.next_device_id = 1;

    cJSON_ArrayForEach(item, devices) {
        const cJSON *id;
        const cJSON *room;
        const cJSON *name;
        const cJSON *type;
        const cJSON *on;
        const char *type_name;
        int device_id;
        int slot;

        if (!cJSON_IsObject(item)) {
            return AGENT_ERROR_PARSE;
        }
        id = cJSON_GetObjectItemCaseSensitive(item, "id");
        room = cJSON_GetObjectItemCaseSensitive(item, "room");
        name = cJSON_GetObjectItemCaseSensitive(item, "name");
        type = cJSON_GetObjectItemCaseSensitive(item, "type");
        on = cJSON_GetObjectItemCaseSensitive(item, "on");
        if (!cJSON_IsNumber(id) || id->valueint <= 0 ||
            !cJSON_IsString(room) || !cJSON_IsString(name) ||
            !cJSON_IsString(type) || !cJSON_IsBool(on)) {
            return AGENT_ERROR_PARSE;
        }

        /* 校验 JSON id：正数且唯一 */
        {
            int j;
            for (j = 0; j < SMART_HOME_MAX_DEVICES; j++) {
                if (tmp.devices[j].used && tmp.devices[j].id == id->valueint) {
                    return AGENT_ERROR_PARSE; /* 重复 id */
                }
            }
        }

        /* 保留 JSON id：device_add 用自增 id，这里覆盖为 JSON 中的稳定 id，
         * 保证重启后设备 id 不变（工具/UI 按 id 操作依赖此稳定性） */
        type_name = type->valuestring;
        if (strcmp(type_name, "light") == 0) {
            slot = smart_home_device_add(&tmp,
                                         room->valuestring,
                                         name->valuestring,
                                         SMART_HOME_DEVICE_LIGHT,
                                         &device_id);
            if (slot < 0) {
                return AGENT_ERROR_LIMIT;
            }
            {
                smart_home_device_t *dev =
                    smart_home_device_find_by_id(&tmp, device_id);
                if (!dev) {
                    return AGENT_ERROR_PARSE;
                }
                dev->id = id->valueint; /* 保留 JSON 稳定 id */
                if (tmp.next_device_id <= id->valueint) {
                    tmp.next_device_id = id->valueint + 1;
                }
            }

            /* 校验/应用亮度 */
            {
                const cJSON *brightness =
                    cJSON_GetObjectItemCaseSensitive(item, "brightness");
                smart_home_device_t *dev =
                    smart_home_device_find_by_id(&tmp, device_id);
                if (!dev) {
                    return AGENT_ERROR_PARSE;
                }
                if (brightness) {
                    if (!cJSON_IsNumber(brightness) ||
                        brightness->valueint < 0 ||
                        brightness->valueint > 100) {
                        return AGENT_ERROR_PARSE;
                    }
                    dev->brightness = brightness->valueint;
                }
                dev->on = on->valueint ? 1 : 0;
            }
        } else if (strcmp(type_name, "ac") == 0) {
            const cJSON *temperature;
            const cJSON *mode;
            const cJSON *fan_speed;
            smart_home_device_t *dev;

            slot = smart_home_device_add(&tmp,
                                         room->valuestring,
                                         name->valuestring,
                                         SMART_HOME_DEVICE_AC,
                                         &device_id);
            if (slot < 0) {
                return AGENT_ERROR_LIMIT;
            }
            dev = smart_home_device_find_by_id(&tmp, device_id);
            if (!dev) {
                return AGENT_ERROR_PARSE;
            }
            dev->id = id->valueint; /* 保留 JSON 稳定 id */
            if (tmp.next_device_id <= id->valueint) {
                tmp.next_device_id = id->valueint + 1;
            }
            dev->on = on->valueint ? 1 : 0;

            temperature = cJSON_GetObjectItemCaseSensitive(item, "temperature");
            mode = cJSON_GetObjectItemCaseSensitive(item, "mode");
            fan_speed = cJSON_GetObjectItemCaseSensitive(item, "fan_speed");
            if (temperature) {
                if (!cJSON_IsNumber(temperature) ||
                    temperature->valueint < 16 ||
                    temperature->valueint > 30) {
                    return AGENT_ERROR_PARSE;
                }
                dev->temperature = temperature->valueint;
            }
            if (mode) {
                int m;
                if (!cJSON_IsString(mode)) {
                    return AGENT_ERROR_PARSE;
                }
                m = smart_home_ac_parse_mode(mode->valuestring);
                if (m < 0) {
                    return AGENT_ERROR_PARSE;
                }
                dev->ac_mode = m;
            }
            if (fan_speed) {
                int f;
                if (!cJSON_IsString(fan_speed)) {
                    return AGENT_ERROR_PARSE;
                }
                f = smart_home_ac_parse_fan_speed(fan_speed->valuestring);
                if (f < 0) {
                    return AGENT_ERROR_PARSE;
                }
                dev->ac_fan_speed = f;
            }
        } else {
            return AGENT_ERROR_PARSE;
        }
    }

    /* environment */
    env = cJSON_GetObjectItemCaseSensitive(root, "environment");
    if (env && cJSON_IsObject(env)) {
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(env, "temperature");
        const cJSON *h = cJSON_GetObjectItemCaseSensitive(env, "humidity");
        const cJSON *l = cJSON_GetObjectItemCaseSensitive(env, "ambient_light");
        if (t) {
            if (!cJSON_IsNumber(t) || t->valueint < 0 || t->valueint > 45) {
                return AGENT_ERROR_PARSE;
            }
            tmp.env_temperature = t->valueint;
        }
        if (h) {
            if (!cJSON_IsNumber(h) || h->valueint < 0 || h->valueint > 100) {
                return AGENT_ERROR_PARSE;
            }
            tmp.env_humidity = h->valueint;
        }
        if (l) {
            if (!cJSON_IsNumber(l) || l->valueint < 0 || l->valueint > 1000) {
                return AGENT_ERROR_PARSE;
            }
            tmp.env_light = l->valueint;
        }
    }

    /* 全部合法，提交 */
    *state = tmp;
    return AGENT_OK;
}
