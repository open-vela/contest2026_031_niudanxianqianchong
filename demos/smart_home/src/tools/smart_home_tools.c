#include "smart_home_tools.h"
#include "../agent/smart_home_tool_metadata.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    "\"on\":{\"type\":\"boolean\"},\"brightness\":{\"type\":\"integer\"}}," \
    "\"required\":[\"room\",\"on\",\"brightness\"],"                       \
    "\"additionalProperties\":false}"

#define SCHEMA_SET_AC                                                      \
    "{\"type\":\"object\",\"properties\":{"                                \
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
    "\"required\":[\"room\",\"on\"],"                                      \
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

typedef struct {
    int active;
    int id;
    int delay_seconds;
    long long created_epoch;
    long long due_epoch;
    char name[32];
    char message[128];
} smart_home_timer_t;

typedef struct {
    const char *condition;
    int temperature;
    int humidity;
    const char *wind;
} smart_home_weather_t;

static smart_home_timer_t g_timers[SMART_HOME_MAX_TIMERS];
static int g_next_timer_id = 1;

static const char *find_json_value(const char *json, const char *key)
{
    const char *p;

    if (!json || !key) {
        return NULL;
    }

    p = strstr(json, key);
    if (!p) {
        return NULL;
    }

    p = strchr(p + strlen(key), ':');
    if (!p) {
        return NULL;
    }

    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }

    return p;
}

static int json_get_int(const char *json, const char *key, int *out)
{
    const char *p = find_json_value(json, key);
    char *end = NULL;
    long value;

    if (!p || !out) {
        return AGENT_ERROR_INVALID;
    }

    value = strtol(p, &end, 10);
    if (end == p) {
        return AGENT_ERROR_INVALID;
    }

    *out = (int)value;
    return AGENT_OK;
}

static int json_get_bool(const char *json, const char *key, int *out)
{
    const char *p = find_json_value(json, key);

    if (!p || !out) {
        return AGENT_ERROR_INVALID;
    }

    if (strncmp(p, "true", 4) == 0) {
        *out = 1;
        return AGENT_OK;
    }

    if (strncmp(p, "false", 5) == 0) {
        *out = 0;
        return AGENT_OK;
    }

    return AGENT_ERROR_INVALID;
}

static int json_get_string(const char *json,
                           const char *key,
                           char *out,
                           size_t out_size)
{
    const char *p = find_json_value(json, key);
    size_t used = 0u;

    if (!p || !out || out_size == 0u || *p != '"') {
        return AGENT_ERROR_INVALID;
    }

    p++;
    while (*p && *p != '"' && used + 1u < out_size) {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'n') {
                out[used++] = '\n';
            } else if (*p == 'r') {
                out[used++] = '\r';
            } else if (*p == 't') {
                out[used++] = '\t';
            } else {
                out[used++] = *p;
            }
            p++;
            continue;
        }
        out[used++] = *p++;
    }
    out[used] = '\0';

    return *p == '"' ? AGENT_OK : AGENT_ERROR_LIMIT;
}

static void json_escape_string(const char *in, char *out, size_t out_size)
{
    size_t used = 0u;

    if (!out || out_size == 0u) {
        return;
    }

    while (in && *in && used + 1u < out_size) {
        if ((*in == '"' || *in == '\\') && used + 2u < out_size) {
            out[used++] = '\\';
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

static int status_tool(const agent_tool_call_t *call,
                       agent_tool_result_t *result,
                       void *user_data)
{
    static char output[1024];
    smart_home_device_service_t *service =
        (smart_home_device_service_t *)user_data;

    (void)call;

    result->status =
        smart_home_device_service_build_snapshot_json(service,
                                                      output,
                                                      sizeof(output));
    result->content_json = output;
    result->error_message = result->status == AGENT_OK ? NULL : "status failed";
    return result->status;
}

#ifdef CONFIG_SMART_HOME_AHT30_TOOL
static int indoor_environment_tool(const agent_tool_call_t *call,
                                   agent_tool_result_t *result,
                                   void *user_data)
{
    static char output[256];
    struct box3_aht30_data_s data;
    int temperature_abs;
    int saved_errno;
    int fd;
    int ret;

    (void)call;
    (void)user_data;

    fd = open("/dev/aht0", O_RDONLY);
    if (fd < 0) {
        saved_errno = errno;
        snprintf(output, sizeof(output),
                 "{\"ok\":false,\"source\":\"box3_sensor_01_aht30\","
                 "\"error\":\"sensor_unavailable\",\"errno\":%d}",
                 saved_errno);
        result->status = AGENT_ERROR;
        result->content_json = output;
        result->error_message = "AHT30 sensor unavailable";
        return result->status;
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
        result->status = AGENT_ERROR;
        result->content_json = output;
        result->error_message = "AHT30 sensor read failed";
        return result->status;
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
    result->status = AGENT_OK;
    result->content_json = output;
    result->error_message = NULL;
    return AGENT_OK;
}
#endif

#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
#include "../miloco/smart_home_miloco.h"
/* 由 smart_home_agent init 设置——tools 层拿不到 agent_app 指针，
 * 用全局单例桥接。 */
smart_home_miloco_t *g_weather_miloco_service;
#endif

static int weather_tool(const agent_tool_call_t *call,
                        agent_tool_result_t *result,
                        void *user_data)
{
    static char output[320];
    char location[64] = "";
    char escaped_location[128];
    int ret;

    ret = json_get_string(call->arguments_json,
                          "\"location\"",
                          location,
                          sizeof(location));
    if (ret != AGENT_OK) {
        snprintf(output, sizeof(output), "{\"ok\":false}");
        result->status = ret;
        result->content_json = output;
        result->error_message = "get_weather failed";
        return ret;
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
            result->status = AGENT_OK;
            result->content_json = output;
            result->error_message = NULL;
            return AGENT_OK;
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
        result->status = AGENT_OK;
        result->content_json = output;
        result->error_message = NULL;
        return AGENT_OK;
    }
}

static int set_light_tool(const agent_tool_call_t *call,
                          agent_tool_result_t *result,
                          void *user_data)
{
    static char output[192];
    smart_home_device_service_t *service =
        (smart_home_device_service_t *)user_data;
    char room[32] = "";
    int brightness = 0;
    int on = 0;
    int ret;

    ret = json_get_string(call->arguments_json, "\"room\"", room, sizeof(room));
    if (ret == AGENT_OK) {
        ret = json_get_bool(call->arguments_json, "\"on\"", &on);
    }
    if (ret == AGENT_OK) {
        ret = json_get_int(call->arguments_json, "\"brightness\"", &brightness);
    }
    if (ret == AGENT_OK) {
        ret = smart_home_device_service_set_light(service, room, on, brightness);
    }

    snprintf(output,
             sizeof(output),
             "{\"ok\":%s,\"room\":\"%s\",\"on\":%s,\"brightness\":%d}",
             ret == AGENT_OK ? "true" : "false",
             room[0] ? room : "",
             on ? "true" : "false",
             brightness);

    result->status = ret;
    result->content_json = output;
    result->error_message = ret == AGENT_OK ? NULL : "set_light failed";
    return ret;
}

static int set_ac_tool(const agent_tool_call_t *call,
                       agent_tool_result_t *result,
                       void *user_data)
{
    static char output[256];
    smart_home_device_service_t *service =
        (smart_home_device_service_t *)user_data;
    smart_home_device_t ac = {0};
    char room[32] = "";
    char mode_str[16] = "";
    char fan_speed_str[16] = "";
    int temperature;
    int mode;
    int fan_speed;
    int on = 0;
    int ret;

    if (!service) {
        result->status = AGENT_ERROR_INVALID;
        result->content_json = "{\"ok\":false}";
        result->error_message = "set_ac failed";
        return result->status;
    }

    ret = json_get_string(call->arguments_json, "\"room\"", room, sizeof(room));
    if (ret == AGENT_OK) {
        ret = smart_home_device_service_find_first(service,
                                                   room,
                                                   SMART_HOME_DEVICE_AC,
                                                   &ac);
    }
    temperature = ret == AGENT_OK ? ac.temperature : 26;
    mode = ret == AGENT_OK ? ac.ac_mode : 0;
    fan_speed = ret == AGENT_OK ? ac.ac_fan_speed : 3;

    if (ret == AGENT_OK) {
        ret = json_get_bool(call->arguments_json, "\"on\"", &on);
    }
    if (ret == AGENT_OK) {
        if (json_get_int(call->arguments_json,
                         "\"temperature\"",
                         &temperature) != AGENT_OK) {
            temperature = ac.temperature;
        }
        if (json_get_string(call->arguments_json,
                            "\"mode\"",
                            mode_str,
                            sizeof(mode_str)) == AGENT_OK) {
            mode = smart_home_ac_parse_mode(mode_str);
        }
        if (json_get_string(call->arguments_json,
                            "\"fan_speed\"",
                            fan_speed_str,
                            sizeof(fan_speed_str)) == AGENT_OK) {
            fan_speed = smart_home_ac_parse_fan_speed(fan_speed_str);
        }
        ret = smart_home_device_service_set_ac(service,
                                               room,
                                               on,
                                               mode,
                                               fan_speed,
                                               temperature);
        if (ret == AGENT_OK) {
            (void)smart_home_device_service_find_first(service,
                                                       room,
                                                       SMART_HOME_DEVICE_AC,
                                                       &ac);
        }
    }

    snprintf(output,
             sizeof(output),
             "{\"ok\":%s,\"room\":\"%s\",\"on\":%s,"
             "\"temperature\":%d,\"mode\":\"%s\",\"fan_speed\":\"%s\"}",
             ret == AGENT_OK ? "true" : "false",
             room[0] ? room : "",
             on ? "true" : "false",
             ac.temperature,
             smart_home_ac_mode_name(ac.ac_mode),
             smart_home_ac_fan_speed_name(ac.ac_fan_speed));

    result->status = ret;
    result->content_json = output;
    result->error_message = ret == AGENT_OK ? NULL : "set_ac failed";
    return ret;
}

static int run_scene_tool(const agent_tool_call_t *call,
                          agent_tool_result_t *result,
                          void *user_data)
{
    static char output[1024];
    smart_home_device_service_t *service =
        (smart_home_device_service_t *)user_data;
    char scene[32] = "";
    int ret;

    ret = json_get_string(call->arguments_json, "\"scene\"", scene, sizeof(scene));
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
                 "{\"ok\":%s,\"scene\":\"%s\",\"report\":\"%s\"}",
                 scene_ret == 0 ? "true" : "false", scene, escaped_report);
        result->status = scene_ret == 0 ? AGENT_OK : AGENT_ERROR;
        result->content_json = output;
        result->error_message = scene_ret == 0 ? NULL :
                                "run_scene failed on real devices";
        return scene_ret == 0 ? AGENT_OK : AGENT_ERROR;
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
                 scene[0] ? scene : "");
    }

    result->status = ret;
    result->content_json = output;
    result->error_message = ret == AGENT_OK ? NULL : "run_scene failed";
    return ret;
}

static int set_timer_tool(const agent_tool_call_t *call,
                          agent_tool_result_t *result,
                          void *user_data)
{
    static char output[512];
    smart_home_timer_t *timer;
    char message[128] = "";
    char escaped_message[256];
    char name[32] = "";
    char escaped_name[64];
    int delay_seconds = 0;
    int slot;
    int ret;
    time_t now;

    (void)user_data;

    ret = json_get_int(call->arguments_json,
                       "\"delay_seconds\"",
                       &delay_seconds);
    if (ret == AGENT_OK) {
        ret = json_get_string(call->arguments_json,
                              "\"message\"",
                              message,
                              sizeof(message));
    }
    if (ret == AGENT_OK && delay_seconds <= 0) {
        ret = AGENT_ERROR_INVALID;
    }
    if (ret == AGENT_OK) {
        if (json_get_string(call->arguments_json,
                            "\"name\"",
                            name,
                            sizeof(name)) != AGENT_OK ||
            name[0] == '\0') {
            snprintf(name, sizeof(name), "timer_%d", g_next_timer_id);
        }
    }

    slot = ret == AGENT_OK ? timer_find_slot() : -1;
    if (ret == AGENT_OK && slot < 0) {
        ret = AGENT_ERROR_LIMIT;
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

    result->status = ret;
    result->content_json = output;
    result->error_message = ret == AGENT_OK ? NULL : "set_timer failed";
    return ret;
}

static int list_timers_tool(const agent_tool_call_t *call,
                            agent_tool_result_t *result,
                            void *user_data)
{
    static char output[768];
    size_t off = 0u;
    int count = 0;
    int i;
    time_t now;

    (void)call;
    (void)user_data;

    now = time(NULL);
    off += snprintf(output + off, sizeof(output) - off, "{\"timers\":[");
    for (i = 0; i < SMART_HOME_MAX_TIMERS && off < sizeof(output); i++) {
        const smart_home_timer_t *timer = &g_timers[i];
        char escaped_name[64];
        char escaped_message[256];
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

    result->status = off < sizeof(output) ? AGENT_OK : AGENT_ERROR_LIMIT;
    result->content_json = output;
    result->error_message =
        result->status == AGENT_OK ? NULL : "list_timers failed";
    return result->status;
}

static int cancel_timer_tool(const agent_tool_call_t *call,
                             agent_tool_result_t *result,
                             void *user_data)
{
    static char output[96];
    int id = 0;
    int slot;
    int ret;

    (void)user_data;

    ret = json_get_int(call->arguments_json, "\"id\"", &id);
    slot = ret == AGENT_OK ? timer_find_by_id(id) : -1;
    if (ret == AGENT_OK && slot < 0) {
        ret = AGENT_ERROR_NOTFOUND;
    }
    if (ret == AGENT_OK) {
        memset(&g_timers[slot], 0, sizeof(g_timers[slot]));
    }

    snprintf(output,
             sizeof(output),
             "{\"ok\":%s,\"id\":%d}",
             ret == AGENT_OK ? "true" : "false",
             id);

    result->status = ret;
    result->content_json = output;
    result->error_message = ret == AGENT_OK ? NULL : "cancel_timer failed";
    return ret;
}

static int register_local_tool(agent_t *agent,
                               const char *name,
                               const char *description,
                               const char *input_schema_json,
                               smart_home_tool_category_t category,
                               agent_tool_fn execute,
                               void *user_data,
                               uint32_t flags)
{
    agent_tool_t tool = {0};

    tool.name = name;
    tool.group_id = SMART_HOME_TOOL_GROUP_LOCAL;
    tool.category_id = (uint16_t)category;
    tool.description = description;
    tool.input_schema_json = input_schema_json;
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

    ret = register_local_tool(agent,
                              "get_home_status",
                              "Get current virtual smart home status.",
                              SCHEMA_GET_HOME_STATUS,
                              SMART_HOME_TOOL_CATEGORY_QUERY,
                              status_tool,
                              device_service,
                              AGENT_TOOL_FLAG_LLM_VISIBLE |
                                  AGENT_TOOL_FLAG_READ_ONLY);
    if (ret != AGENT_OK) {
        return ret;
    }

#ifdef CONFIG_SMART_HOME_AHT30_TOOL
    ret = register_local_tool(agent,
                              "get_indoor_environment",
                              "Read current indoor temperature and humidity from the Box3 AHT30 sensor.",
                              SCHEMA_GET_INDOOR_ENVIRONMENT,
                              SMART_HOME_TOOL_CATEGORY_QUERY,
                              indoor_environment_tool,
                              NULL,
                              AGENT_TOOL_FLAG_LLM_VISIBLE |
                                  AGENT_TOOL_FLAG_READ_ONLY);
    if (ret != AGENT_OK) {
        return ret;
    }
#endif

    ret = register_local_tool(agent,
                              "get_weather",
                              "Get simulated outdoor weather for a location.",
                              SCHEMA_GET_WEATHER,
                              SMART_HOME_TOOL_CATEGORY_QUERY,
                              weather_tool,
                              device_service,
                              AGENT_TOOL_FLAG_LLM_VISIBLE |
                                  AGENT_TOOL_FLAG_READ_ONLY);
    if (ret != AGENT_OK) {
        return ret;
    }

    ret = register_local_tool(agent,
                              "set_light",
                              "Set a room light. Rooms: living_room, bedroom.",
                              SCHEMA_SET_LIGHT,
                              SMART_HOME_TOOL_CATEGORY_CONTROL,
                              set_light_tool,
                              device_service,
                              AGENT_TOOL_FLAG_LLM_VISIBLE |
                                  AGENT_TOOL_FLAG_SIDE_EFFECT);
    if (ret != AGENT_OK) {
        return ret;
    }

    ret = register_local_tool(agent,
                              "set_ac",
                              "Set bedroom air conditioner.",
                              SCHEMA_SET_AC,
                              SMART_HOME_TOOL_CATEGORY_CONTROL,
                              set_ac_tool,
                              device_service,
                              AGENT_TOOL_FLAG_LLM_VISIBLE |
                                  AGENT_TOOL_FLAG_SIDE_EFFECT);
    if (ret != AGENT_OK) {
        return ret;
    }

    ret = register_local_tool(agent,
                              "run_scene",
                              "Run a smart home scene: sleep, movie, away, home.",
                              SCHEMA_RUN_SCENE,
                              SMART_HOME_TOOL_CATEGORY_CONTROL,
                              run_scene_tool,
                              device_service,
                              AGENT_TOOL_FLAG_LLM_VISIBLE |
                                  AGENT_TOOL_FLAG_SIDE_EFFECT);
    if (ret != AGENT_OK) {
        return ret;
    }

    ret = register_local_tool(agent,
                              "set_timer",
                              "Create a demo timer or reminder.",
                              SCHEMA_SET_TIMER,
                              SMART_HOME_TOOL_CATEGORY_CONTROL,
                              set_timer_tool,
                              device_service,
                              AGENT_TOOL_FLAG_LLM_VISIBLE |
                                  AGENT_TOOL_FLAG_SIDE_EFFECT);
    if (ret != AGENT_OK) {
        return ret;
    }

    ret = register_local_tool(agent,
                              "list_timers",
                              "List active demo timers.",
                              SCHEMA_LIST_TIMERS,
                              SMART_HOME_TOOL_CATEGORY_QUERY,
                              list_timers_tool,
                              device_service,
                              AGENT_TOOL_FLAG_LLM_VISIBLE |
                                  AGENT_TOOL_FLAG_READ_ONLY);
    if (ret != AGENT_OK) {
        return ret;
    }

    return register_local_tool(agent,
                               "cancel_timer",
                               "Cancel a demo timer by id.",
                               SCHEMA_CANCEL_TIMER,
                               SMART_HOME_TOOL_CATEGORY_CONTROL,
                               cancel_timer_tool,
                               device_service,
                               AGENT_TOOL_FLAG_LLM_VISIBLE |
                                   AGENT_TOOL_FLAG_SIDE_EFFECT);
}
