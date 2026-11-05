#include "smart_home_tools.h"
#include "smart_home_tool_io.h"
#include "../agent/smart_home_tool_metadata.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef CONFIG_SMART_HOME_REMINDERS
#include "../reminder/smart_home_reminder_tools.h"
#endif

#ifdef CONFIG_SMART_HOME_AHT30_TOOL
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <arch/board/board.h>
#include <nuttx/sensors/ioctl.h>
#endif

#define SMART_HOME_MAX_TIMERS 8

#define SCHEMA_GET_HOME_STATUS                                             \
    "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"

#define SCHEMA_GET_INDOOR_ENVIRONMENT                                      \
    "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"

#define SCHEMA_GET_WEATHER                                                 \
    "{\"type\":\"object\",\"properties\":{"                                \
    "\"location\":{\"type\":\"string\",\"description\":"                   \
        "\"City or location, e.g. Shanghai, Beijing, Shenzhen\"}},"        \
    "\"required\":[\"location\"],\"additionalProperties\":false}"

#define SCHEMA_SET_LIGHT                                                   \
    "{\"type\":\"object\",\"properties\":{\"room\":{\"type\":\"string\"},"  \
    "\"device_id\":{\"type\":\"integer\",\"description\":\"Exact demo device_id from get_device_inventory\"}," \
    "\"on\":{\"type\":\"boolean\"},\"brightness\":{\"type\":\"integer\"}}," \
    "\"required\":[\"device_id\",\"room\",\"on\",\"brightness\"],"                       \
    "\"additionalProperties\":false}"

#define SCHEMA_SET_AC                                                      \
    "{\"type\":\"object\",\"properties\":{"                                \
    "\"device_id\":{\"type\":\"integer\",\"description\":\"Exact demo device_id from get_device_inventory\"}," \
    "\"room\":{\"type\":\"string\",\"description\":\"Room name: bedroom\"}," \
    "\"on\":{\"type\":\"boolean\",\"description\":\"Whether AC is on\"},"   \
    "\"temperature\":{\"type\":\"integer\",\"description\":"               \
        "\"Target temperature for cool, heat, or auto mode\"},"             \
    "\"mode\":{\"type\":\"string\",\"enum\":"                              \
        "[\"cool\",\"heat\",\"dry\",\"fan\",\"auto\"],"                   \
        "\"description\":\"AC mode. fan and dry do not require temperature\"}," \
    "\"fan_speed\":{\"type\":\"string\",\"enum\":"                         \
        "[\"low\",\"medium\",\"high\",\"auto\"],"                         \
        "\"description\":\"Fan speed. Defaults to current value or auto\"}}," \
    "\"required\":[\"device_id\",\"room\",\"on\"],"                                      \
    "\"additionalProperties\":false}"

#define SCHEMA_RUN_SCENE                                                   \
    "{\"type\":\"object\",\"properties\":{\"scene\":{\"type\":\"string\"}}," \
    "\"required\":[\"scene\"],\"additionalProperties\":false}"

#define SCHEMA_SET_TIMER                                                   \
    "{\"type\":\"object\",\"properties\":{"                                \
    "\"delay_seconds\":{\"type\":\"integer\",\"description\":"            \
        "\"Timer delay in seconds\"},"                                      \
    "\"message\":{\"type\":\"string\",\"description\":"                   \
        "\"Reminder text or scheduled task description\"},"                \
    "\"name\":{\"type\":\"string\",\"description\":\"Optional timer name\"}}," \
    "\"required\":[\"delay_seconds\",\"message\"],"                      \
    "\"additionalProperties\":false}"

#define SCHEMA_LIST_TIMERS                                                 \
    "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"

#define SCHEMA_CANCEL_TIMER                                                \
    "{\"type\":\"object\",\"properties\":{"                                \
    "\"id\":{\"type\":\"integer\",\"description\":\"Timer id\"}},"        \
    "\"required\":[\"id\"],\"additionalProperties\":false}"

#ifndef CONFIG_SMART_HOME_REMINDERS
typedef struct {
    int active;
    int id;
    int delay_seconds;
    long long created_epoch;
    long long due_epoch;
    char name[32];
    char message[128];
} smart_home_timer_t;
#endif

typedef struct {
    const char *condition;
    int temperature;
    int humidity;
    const char *wind;
} smart_home_weather_t;

#ifndef CONFIG_SMART_HOME_REMINDERS
static smart_home_timer_t g_timers[SMART_HOME_MAX_TIMERS];
static int g_next_timer_id = 1;
#endif

static void json_escape_string(const char *in, char *out, size_t out_size)
{
    size_t used = 0u;

    if (!out || out_size == 0u) {
        return;
    }

    while (in && *in && used + 1u < out_size) {
        if ((*in == '"' || *in == '\\') && used + 2u < out_size) {
            out[used++] = '\\';
            if ((unsigned char)*in < 0x20u) {
            if (used + 6u >= out_size) break;
            snprintf(out + used, out_size - used, "\\u%04x", (unsigned char)*in++);
            used += 6u;
            continue;
        }
        out[used++] = *in++;
            continue;
        }
        if (*in == '\n' && used + 2u < out_size) {
            out[used++] = '\\';
            out[used++] = 'n';
            in++;
            continue;
        }
        if (*in == '\r' && used + 2u < out_size) {
            out[used++] = '\\';
            out[used++] = 'r';
            in++;
            continue;
        }
        if (*in == '\t' && used + 2u < out_size) {
            out[used++] = '\\';
            out[used++] = 't';
            in++;
            continue;
        }
        if ((unsigned char)*in < 0x20u) {
            if (used + 6u >= out_size) break;
            snprintf(out + used, out_size - used, "\\u%04x", (unsigned char)*in++);
            used += 6u;
            continue;
        }
        out[used++] = *in++;
    }
    out[used] = '\0';
}

static unsigned int weather_hash_location(const char *location)
{
    unsigned int hash = 5381u;

    while (location && *location) {
        hash = ((hash << 5) + hash) + (unsigned char)*location++;
    }

    return hash;
}

static smart_home_weather_t weather_for_location(const char *location)
{
    smart_home_weather_t weather;
    unsigned int hash;
    static const char *conditions[] = {
        "clear",
        "partly_cloudy",
        "cloudy",
        "light_rain",
        "hot",
    };
    static const char *winds[] = {
        "calm",
        "light",
        "moderate",
    };

    if (location && strcmp(location, "Shanghai") == 0) {
        weather.condition = "light_rain";
        weather.temperature = 27;
        weather.humidity = 78;
        weather.wind = "moderate";
        return weather;
    }

    if (location && strcmp(location, "Beijing") == 0) {
        weather.condition = "clear";
        weather.temperature = 30;
        weather.humidity = 36;
        weather.wind = "light";
        return weather;
    }

    if (location && strcmp(location, "Shenzhen") == 0) {
        weather.condition = "cloudy";
        weather.temperature = 29;
        weather.humidity = 82;
        weather.wind = "moderate";
        return weather;
    }

    hash = weather_hash_location(location);
    weather.condition = conditions[hash % 5u];
    weather.temperature = 18 + (int)(hash % 16u);
    weather.humidity = 35 + (int)((hash / 7u) % 55u);
    weather.wind = winds[(hash / 17u) % 3u];
    return weather;
}

#ifndef CONFIG_SMART_HOME_REMINDERS
static int timer_find_slot(void)
{
    int i;

    for (i = 0; i < SMART_HOME_MAX_TIMERS; i++) {
        if (!g_timers[i].active) {
            return i;
        }
    }

    return -1;
}

static int timer_find_by_id(int id)
{
    int i;

    for (i = 0; i < SMART_HOME_MAX_TIMERS; i++) {
        if (g_timers[i].active && g_timers[i].id == id) {
            return i;
        }
    }

    return -1;
}

#endif

static agent_error_t status_tool(void *user_data,
                                   const agent_tool_context_t *context,
                                   const agent_text_sink_t *sink)
{
    static char output[1024];
    smart_home_device_service_t *service =
        (smart_home_device_service_t *)user_data;

    int ret;

    (void)context;
    ret = smart_home_device_service_build_snapshot_json(service,
                                                            output, sizeof(output));
    if (ret != AGENT_OK) return smart_home_tool_reply(sink, "{\"ok\":false,\"source\":\"demo\"}", (agent_error_t)ret);
    cJSON *root = cJSON_Parse(output);
    if (!root || !cJSON_AddStringToObject(root, "source", "demo")) {
        cJSON_Delete(root);
        return AGENT_ERROR_NOMEM;
    }
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) return AGENT_ERROR_NOMEM;
    ret = smart_home_tool_reply(sink, text, AGENT_OK);
    cJSON_free(text);
    return (agent_error_t)ret;
}

#ifdef CONFIG_SMART_HOME_AHT30_TOOL
static agent_error_t indoor_environment_tool(void *user_data,
                                   const agent_tool_context_t *context,
                                   const agent_text_sink_t *sink)
{
    static char output[256];
    struct box3_aht30_data_s data;
    int temperature_abs;
    int saved_errno;
    int fd;
    int ret;

    (void)context;
    (void)user_data;

    fd = open("/dev/aht0", O_RDONLY);
    if (fd < 0) {
        saved_errno = errno;
        snprintf(output, sizeof(output),
                 "{\"ok\":false,\"source\":\"box3_sensor_01_aht30\","
                 "\"error\":\"sensor_unavailable\",\"errno\":%d}",
                 saved_errno);
        return smart_home_tool_reply(sink, output, AGENT_ERROR);
    }

    ret = ioctl(fd, SNIOC_READ_CONVERT_DATA,
                (unsigned long)(uintptr_t)&data);
    if (ret < 0) {
        saved_errno = errno;
        close(fd);
        snprintf(output, sizeof(output),
                 "{\"ok\":false,\"source\":\"box3_sensor_01_aht30\","
                 "\"error\":\"sensor_read_failed\",\"errno\":%d}",
                 saved_errno);
        return smart_home_tool_reply(sink, output, AGENT_ERROR);
    }

    close(fd);
    temperature_abs = data.temperature < 0 ? -data.temperature :
                                              data.temperature;
    snprintf(output, sizeof(output),
             "{\"ok\":true,\"source\":\"box3_sensor_01_aht30\","
             "\"device\":\"/dev/aht0\",\"fresh\":true,"
             "\"temperature_c\":%s%d.%03d,\"humidity_percent\":%d.%03d}",
             data.temperature < 0 ? "-" : "", temperature_abs / 1000,
             temperature_abs % 1000, data.humidity / 1000,
             data.humidity % 1000);
    return smart_home_tool_reply(sink, output, AGENT_OK);
}
#endif

#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
#include "../miloco/smart_home_miloco.h"
/* 由 smart_home_agent init 设置——tools 层拿不到 agent_app 指针，
 * 用全局单例桥接。 */
smart_home_miloco_t *g_weather_miloco_service;
#endif

static agent_error_t weather_tool(void *user_data,
                                   const agent_tool_context_t *context,
                                   const agent_text_sink_t *sink)
{
    static char output[640];
    char location[64] = "";
    char escaped_location[384];
    int ret;

    (void)user_data;
    ret = smart_home_tool_get_string(context->call.arguments_json,
                          "location",
                          location,
                          sizeof(location));
    if (ret != AGENT_OK) {
        snprintf(output, sizeof(output), "{\"ok\":false}");
        return smart_home_tool_reply(sink, output, ret);
    }
    json_escape_string(location, escaped_location, sizeof(escaped_location));

#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
    /* 真实天气：通过 smart_home_agent.h 的 app 单例取 miloco 服务。 */
    {
        smart_home_miloco_weather_t wx;

        if (g_weather_miloco_service &&
            smart_home_miloco_get_weather(g_weather_miloco_service, &wx)) {
            snprintf(output, sizeof(output),
                     "{\"ok\":true,\"source\":\"wttr.in\""
                     ",\"location\":\"%s\",\"condition\":\"%s\""
                     ",\"condition_cn\":\"%s\",\"temperature\":%d"
                     ",\"humidity\":%d,\"wind_kmph\":%d}",
                     wx.city, wx.condition_en, wx.condition_cn,
                     wx.temperature, wx.humidity, wx.wind_kmph);
            return smart_home_tool_reply(sink, output, AGENT_OK);
        }
    }
#endif

    /* 无网关或天气尚未就绪：回落模拟数据。 */
    {
        smart_home_weather_t weather = weather_for_location(location);

        snprintf(output, sizeof(output),
                 "{\"ok\":true,\"source\":\"demo_simulated\""
                 ",\"location\":\"%s\",\"condition\":\"%s\""
                 ",\"temperature\":%d,\"humidity\":%d"
                 ",\"wind\":\"%s\"}",
                 escaped_location, weather.condition,
                 weather.temperature, weather.humidity, weather.wind);
        return smart_home_tool_reply(sink, output, AGENT_OK);
    }
}

static agent_error_t set_light_tool(void *user_data,
                                   const agent_tool_context_t *context,
                                   const agent_text_sink_t *sink)
{
    static char output[320];
    smart_home_device_service_t *service =
        (smart_home_device_service_t *)user_data;
    char room[32] = "";
    char escaped_room[192];
    int brightness = 0;
    int on = 0;
    int device_id;
    smart_home_device_t target;
    int ret;

    ret = smart_home_tool_get_string(context->call.arguments_json, "room", room, sizeof(room));
    if (ret == AGENT_OK)
        ret = smart_home_tool_get_int(context->call.arguments_json, "device_id", &device_id);
    if (ret == AGENT_OK)
        ret = smart_home_device_service_get_device(service, device_id, &target);
    if (ret == AGENT_OK && (target.type != SMART_HOME_DEVICE_LIGHT || strcmp(target.room, room)))
        ret = AGENT_ERROR_TOOL_ARGUMENT;
    if (ret == AGENT_OK) {
        ret = smart_home_tool_get_bool(context->call.arguments_json, "on", &on);
    }
    if (ret == AGENT_OK) {
        ret = smart_home_tool_get_int(context->call.arguments_json, "brightness", &brightness);
    }
    if (ret == AGENT_OK) {
        ret = smart_home_device_service_set_device_control(service, device_id, on, brightness,
                                                           target.ac_mode, target.ac_fan_speed);
    }

    json_escape_string(room, escaped_room, sizeof(escaped_room));
    snprintf(output,
             sizeof(output),
             "{\"ok\":%s,\"source\":\"demo\",\"room\":\"%s\",\"on\":%s,\"brightness\":%d}",
             ret == AGENT_OK ? "true" : "false",
             escaped_room,
             on ? "true" : "false",
             brightness);

    return smart_home_tool_reply(sink, output, ret);
}

static agent_error_t set_ac_tool(void *user_data,
                                   const agent_tool_context_t *context,
                                   const agent_text_sink_t *sink)
{
    static char output[448];
    smart_home_device_service_t *service =
        (smart_home_device_service_t *)user_data;
    smart_home_device_t ac = {0};
    char room[32] = "";
    char escaped_room[192];
    char mode_str[16] = "";
    char fan_speed_str[16] = "";
    int temperature;
    int mode;
    int fan_speed;
    int on = 0;
    int device_id;
    int ret;

    if (!service) {
        return smart_home_tool_reply(sink, "{\"ok\":false}", AGENT_ERROR_INVALID);
    }

    ret = smart_home_tool_get_string(context->call.arguments_json, "room", room, sizeof(room));
    if (ret == AGENT_OK) {
        ret = smart_home_tool_get_int(context->call.arguments_json, "device_id", &device_id);
    }
    if (ret == AGENT_OK)
        ret = smart_home_device_service_get_device(service, device_id, &ac);
    if (ret == AGENT_OK && (ac.type != SMART_HOME_DEVICE_AC || strcmp(ac.room, room)))
        ret = AGENT_ERROR_TOOL_ARGUMENT;
    temperature = ret == AGENT_OK ? ac.temperature : 26;
    mode = ret == AGENT_OK ? ac.ac_mode : 0;
    fan_speed = ret == AGENT_OK ? ac.ac_fan_speed : 3;

    if (ret == AGENT_OK) {
        ret = smart_home_tool_get_bool(context->call.arguments_json, "on", &on);
    }
    if (ret == AGENT_OK) {
        int optional;
        optional = smart_home_tool_get_int(context->call.arguments_json,
                                           "temperature", &temperature);
        if (optional != AGENT_OK && optional != AGENT_ERROR_NOT_FOUND) ret = optional;
        optional = smart_home_tool_get_string(context->call.arguments_json,
                                               "mode", mode_str, sizeof(mode_str));
        if (optional == AGENT_OK) {
            mode = smart_home_ac_parse_mode(mode_str);
            if (mode < 0) ret = AGENT_ERROR_TOOL_ARGUMENT;
        } else if (optional != AGENT_ERROR_NOT_FOUND) ret = optional;
        optional = smart_home_tool_get_string(context->call.arguments_json,
                                               "fan_speed", fan_speed_str, sizeof(fan_speed_str));
        if (optional == AGENT_OK) {
            fan_speed = smart_home_ac_parse_fan_speed(fan_speed_str);
            if (fan_speed < 0) ret = AGENT_ERROR_TOOL_ARGUMENT;
        } else if (optional != AGENT_ERROR_NOT_FOUND) ret = optional;
        if (ret == AGENT_OK)
            ret = smart_home_device_service_set_device_control(service, device_id, on,
                                                                 temperature, mode, fan_speed);
        if (ret == AGENT_OK) {
            (void)smart_home_device_service_get_device(service, device_id, &ac);
        }
    }

    json_escape_string(room, escaped_room, sizeof(escaped_room));
    snprintf(output,
             sizeof(output),
             "{\"ok\":%s,\"source\":\"demo\",\"room\":\"%s\",\"on\":%s,"
             "\"temperature\":%d,\"mode\":\"%s\",\"fan_speed\":\"%s\"}",
             ret == AGENT_OK ? "true" : "false",
             escaped_room,
             on ? "true" : "false",
             ac.temperature,
             smart_home_ac_mode_name(ac.ac_mode),
             smart_home_ac_fan_speed_name(ac.ac_fan_speed));

    return smart_home_tool_reply(sink, output, ret);
}

static agent_error_t run_scene_tool(void *user_data,
                                   const agent_tool_context_t *context,
                                   const agent_text_sink_t *sink)
{
    static char output[1280];
    smart_home_device_service_t *service =
        (smart_home_device_service_t *)user_data;
    char scene[32] = "";
    char escaped_scene[192];
    int ret;

    ret = smart_home_tool_get_string(context->call.arguments_json, "scene", scene, sizeof(scene));
    json_escape_string(scene, escaped_scene, sizeof(escaped_scene));
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
    if (ret == AGENT_OK) {
        /* 场景只面向真实米家设备：动作以 type_name 语义锚点在各
         * 在线设备的 controls 中匹配提交（如睡眠模式开夜视/人形
         * 追踪）。不再回落虚拟设备。 */
        extern smart_home_miloco_t *g_weather_miloco_service;
        char report[512];
        char escaped_report[896];
        int scene_ret = -1;

        report[0] = '\0';
        if (g_weather_miloco_service) {
            scene_ret = smart_home_miloco_run_scene(
                g_weather_miloco_service, scene, report, sizeof(report));
        }
        json_escape_string(report[0] ? report : "miloco gateway unavailable",
                           escaped_report, sizeof(escaped_report));
        snprintf(output, sizeof(output),
                 "{\"ok\":%s,\"source\":\"mijia\",\"scene\":\"%s\",\"report\":\"%s\"}",
                 scene_ret == 0 ? "true" : "false", escaped_scene, escaped_report);
        return smart_home_tool_reply(sink, output,
                                     scene_ret == 0 ? AGENT_OK : AGENT_ERROR_TOOL_FAILED);
    }
#endif
    if (ret == AGENT_OK) {
        ret = smart_home_device_service_run_scene(service, scene);
    }
    if (ret == AGENT_OK) {
        ret = smart_home_device_service_build_snapshot_json(service,
                                                            output,
                                                            sizeof(output));
    } else {
        snprintf(output,
                 sizeof(output),
                 "{\"ok\":false,\"scene\":\"%s\"}",
                 escaped_scene);
    }

    return smart_home_tool_reply(sink, output, ret);
}

#ifndef CONFIG_SMART_HOME_REMINDERS
static agent_error_t set_timer_tool(void *user_data,
                                   const agent_tool_context_t *context,
                                   const agent_text_sink_t *sink)
{
    static char output[1280];
    smart_home_timer_t *timer;
    char message[128] = "";
    char escaped_message[768];
    char name[32] = "";
    char escaped_name[192];
    int delay_seconds = 0;
    int slot;
    int ret;
    time_t now;

    (void)user_data;

    ret = smart_home_tool_get_int(context->call.arguments_json,
                       "delay_seconds",
                       &delay_seconds);
    if (ret == AGENT_OK) {
        ret = smart_home_tool_get_string(context->call.arguments_json,
                              "message",
                              message,
                              sizeof(message));
    }
    if (ret == AGENT_OK && delay_seconds <= 0) {
        ret = AGENT_ERROR_INVALID;
    }
    if (ret == AGENT_OK) {
        if (smart_home_tool_get_string(context->call.arguments_json,
                            "name",
                            name,
                            sizeof(name)) != AGENT_OK ||
            name[0] == '\0') {
            snprintf(name, sizeof(name), "timer_%d", g_next_timer_id);
        }
    }

    slot = ret == AGENT_OK ? timer_find_slot() : -1;
    if (ret == AGENT_OK && slot < 0) {
        ret = AGENT_ERROR_CAPACITY;
    }

    if (ret == AGENT_OK) {
        now = time(NULL);
        timer = &g_timers[slot];
        memset(timer, 0, sizeof(*timer));
        timer->active = 1;
        timer->id = g_next_timer_id++;
        timer->delay_seconds = delay_seconds;
        timer->created_epoch = (long long)now;
        timer->due_epoch = (long long)now + delay_seconds;
        strncpy(timer->name, name, sizeof(timer->name) - 1);
        strncpy(timer->message, message, sizeof(timer->message) - 1);
        json_escape_string(timer->name, escaped_name, sizeof(escaped_name));
        json_escape_string(timer->message,
                           escaped_message,
                           sizeof(escaped_message));

        snprintf(output,
                 sizeof(output),
                 "{\"ok\":true,\"id\":%d,\"name\":\"%s\","
                 "\"delay_seconds\":%d,\"due_epoch\":%lld,"
                 "\"message\":\"%s\"}",
                 timer->id,
                 escaped_name,
                 timer->delay_seconds,
                 timer->due_epoch,
                 escaped_message);
    } else {
        snprintf(output, sizeof(output), "{\"ok\":false}");
    }

    return smart_home_tool_reply(sink, output, ret);
}

static agent_error_t list_timers_tool(void *user_data,
                                   const agent_tool_context_t *context,
                                   const agent_text_sink_t *sink)
{
    static char output[4096];
    size_t off = 0u;
    int count = 0;
    int i;
    time_t now;

    (void)context;
    (void)user_data;

    now = time(NULL);
    off += snprintf(output + off, sizeof(output) - off, "{\"timers\":[");
    for (i = 0; i < SMART_HOME_MAX_TIMERS && off < sizeof(output); i++) {
        const smart_home_timer_t *timer = &g_timers[i];
        char escaped_name[192];
        char escaped_message[768];
        long long remaining;

        if (!timer->active) {
            continue;
        }

        remaining = timer->due_epoch - (long long)now;
        json_escape_string(timer->name, escaped_name, sizeof(escaped_name));
        json_escape_string(timer->message,
                           escaped_message,
                           sizeof(escaped_message));
        if (count > 0) {
            off += snprintf(output + off, sizeof(output) - off, ",");
        }
        off += snprintf(output + off,
                        sizeof(output) - off,
                        "{\"id\":%d,\"name\":\"%s\","
                        "\"message\":\"%s\",\"remaining_seconds\":%lld,"
                        "\"expired\":%s}",
                        timer->id,
                        escaped_name,
                        escaped_message,
                        remaining > 0 ? remaining : 0,
                        remaining <= 0 ? "true" : "false");
        count++;
    }
    if (off < sizeof(output)) {
        off += snprintf(output + off,
                        sizeof(output) - off,
                        "],\"count\":%d}",
                        count);
    }

    if (off >= sizeof(output))
        return smart_home_tool_reply(sink, "{\"ok\":false,\"error\":\"timer_list_capacity\"}", AGENT_ERROR_CAPACITY);
    return smart_home_tool_reply(sink, output, AGENT_OK);
}

static agent_error_t cancel_timer_tool(void *user_data,
                                   const agent_tool_context_t *context,
                                   const agent_text_sink_t *sink)
{
    static char output[96];
    int id = 0;
    int slot;
    int ret;

    (void)user_data;

    ret = smart_home_tool_get_int(context->call.arguments_json, "id", &id);
    slot = ret == AGENT_OK ? timer_find_by_id(id) : -1;
    if (ret == AGENT_OK && slot < 0) {
        ret = AGENT_ERROR_NOT_FOUND;
    }
    if (ret == AGENT_OK) {
        memset(&g_timers[slot], 0, sizeof(g_timers[slot]));
    }

    snprintf(output,
             sizeof(output),
             "{\"ok\":%s,\"id\":%d}",
             ret == AGENT_OK ? "true" : "false",
             id);

    return smart_home_tool_reply(sink, output, ret);
}

#endif

int smart_home_tool_register_local(agent_t *agent,
                               const char *name,
                               const char *description,
                               const char *input_schema_json,
                               smart_home_tool_category_t category,
                               agent_tool_execute_fn execute,
                               void *user_data,
                               uint32_t flags)
{
    agent_tool_t tool = {0};

    tool.name = agent_string_view(name, strlen(name));
    tool.group = agent_string_view("local", 5u);
    tool.category = category == SMART_HOME_TOOL_CATEGORY_QUERY ?
        agent_string_view("query", 5u) : agent_string_view("control", 7u);
    tool.description = agent_string_view(description, strlen(description));
    tool.input_schema_json = agent_string_view(input_schema_json, strlen(input_schema_json));
    tool.validate = smart_home_tool_validate_object;
    tool.execute = execute;
    tool.user_data = user_data;
    tool.flags = flags;
    return agent_register_tool(agent, &tool);
}

int smart_home_tools_register(agent_t *agent,
                              smart_home_device_service_t *device_service)
{
    int ret;

    if (!agent || !device_service) {
        return AGENT_ERROR_INVALID;
    }

    ret = smart_home_tool_register_local(agent,
                              "get_home_status",
                              "[demo] Get virtual device state only; not a whole-home hardware inventory. Use get_device_inventory to discover origins.",
                              SCHEMA_GET_HOME_STATUS,
                              SMART_HOME_TOOL_CATEGORY_QUERY,
                              status_tool,
                              device_service,
                              AGENT_TOOL_READ_ONLY);
    if (ret != AGENT_OK) {
        return ret;
    }

#ifdef CONFIG_SMART_HOME_AHT30_TOOL
    ret = smart_home_tool_register_local(agent,
                              "get_indoor_environment",
                              "Read current indoor temperature and humidity from the Box3 AHT30 sensor.",
                              SCHEMA_GET_INDOOR_ENVIRONMENT,
                              SMART_HOME_TOOL_CATEGORY_QUERY,
                              indoor_environment_tool,
                              NULL,
                              AGENT_TOOL_READ_ONLY);
    if (ret != AGENT_OK) {
        return ret;
    }
#endif

    ret = smart_home_tool_register_local(agent,
                              "get_weather",
                              "Get simulated outdoor weather for a location.",
                              SCHEMA_GET_WEATHER,
                              SMART_HOME_TOOL_CATEGORY_QUERY,
                              weather_tool,
                              device_service,
                              AGENT_TOOL_READ_ONLY);
    if (ret != AGENT_OK) {
        return ret;
    }

    ret = smart_home_tool_register_local(agent,
                              "set_light",
                              "[demo] Change only a virtual light by its discovered device_id and room. No physical Mi Home, board or Node control.",
                              SCHEMA_SET_LIGHT,
                              SMART_HOME_TOOL_CATEGORY_CONTROL,
                              set_light_tool,
                              device_service,
                              AGENT_TOOL_SIDE_EFFECT);
    if (ret != AGENT_OK) {
        return ret;
    }

    ret = smart_home_tool_register_local(agent,
                              "set_ac",
                              "[demo] Change only a virtual AC by its discovered device_id and room. No physical Mi Home, board or Node control.",
                              SCHEMA_SET_AC,
                              SMART_HOME_TOOL_CATEGORY_CONTROL,
                              set_ac_tool,
                              device_service,
                              AGENT_TOOL_SIDE_EFFECT);
    if (ret != AGENT_OK) {
        return ret;
    }

    ret = smart_home_tool_register_local(agent,
                              "run_scene",
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
                              "[mijia] Submit an explicit scene to real Mi Home devices only: sleep, movie, away, home. Never use to probe an ambiguous request.",
#else
                              "[demo] Change only virtual scene state: sleep, movie, away, home.",
#endif
                              SCHEMA_RUN_SCENE,
                              SMART_HOME_TOOL_CATEGORY_CONTROL,
                              run_scene_tool,
                              device_service,
                              AGENT_TOOL_SIDE_EFFECT);
    if (ret != AGENT_OK) {
        return ret;
    }

#ifdef CONFIG_SMART_HOME_REMINDERS
    return smart_home_reminder_tools_register(agent);
#else
    ret = smart_home_tool_register_local(agent,
                              "set_timer",
                              "Create a demo timer or reminder.",
                              SCHEMA_SET_TIMER,
                              SMART_HOME_TOOL_CATEGORY_CONTROL,
                              set_timer_tool,
                              device_service,
                              AGENT_TOOL_SIDE_EFFECT);
    if (ret != AGENT_OK) {
        return ret;
    }

    ret = smart_home_tool_register_local(agent,
                              "list_timers",
                              "List active demo timers.",
                              SCHEMA_LIST_TIMERS,
                              SMART_HOME_TOOL_CATEGORY_QUERY,
                              list_timers_tool,
                              device_service,
                              AGENT_TOOL_READ_ONLY);
    if (ret != AGENT_OK) {
        return ret;
    }

    return smart_home_tool_register_local(agent,
                               "cancel_timer",
                               "Cancel a demo timer by id.",
                               SCHEMA_CANCEL_TIMER,
                               SMART_HOME_TOOL_CATEGORY_CONTROL,
                               cancel_timer_tool,
                               device_service,
                               AGENT_TOOL_SIDE_EFFECT);
#endif
}
