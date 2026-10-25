#include "smart_home_device_service.h"
#include <agent/context.h>

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

static uint64_t device_service_now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return 0u;
    }

    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int append_json(char *buffer, size_t size, size_t *offset,
                       const char *format, ...)
{
    va_list args;
    int written;

    if (!buffer || !offset || *offset >= size) {
        return AGENT_ERROR_LIMIT;
    }

    va_start(args, format);
    written = vsnprintf(buffer + *offset, size - *offset, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= size - *offset) {
        return AGENT_ERROR_LIMIT;
    }

    *offset += (size_t)written;
    return AGENT_OK;
}

static int append_device_json(char *buffer, size_t size, size_t *offset,
                              const smart_home_device_t *device, int first)
{
    if (device->type == SMART_HOME_DEVICE_AC) {
        return append_json(buffer, size, offset,
                           "%s{\"id\":\"%d\",\"room\":\"%s\","
                           "\"name\":\"%s\",\"type\":\"ac\","
                           "\"online\":true,\"state\":{\"on\":%s,"
                           "\"temperature\":%d,\"mode\":\"%s\","
                           "\"fanSpeed\":\"%s\"}}",
                           first ? "" : ",", device->id, device->room,
                           device->name, device->on ? "true" : "false",
                           device->temperature,
                           smart_home_ac_mode_name(device->ac_mode),
                           smart_home_ac_fan_speed_name(device->ac_fan_speed));
    }

    return append_json(buffer, size, offset,
                       "%s{\"id\":\"%d\",\"room\":\"%s\","
                       "\"name\":\"%s\",\"type\":\"light\","
                       "\"online\":true,\"state\":{\"on\":%s,"
                       "\"brightness\":%d}}",
                       first ? "" : ",", device->id, device->room,
                       device->name, device->on ? "true" : "false",
                       device->brightness);
}

static int build_snapshot_locked(smart_home_device_service_t *service,
                                 char *buffer,
                                 size_t buffer_size)
{
    size_t offset = 0u;
    int first = 1;
    int i;
    int ret;

    ret = append_json(buffer, buffer_size, &offset,
                      "{\"revision\":%lu,\"devices\":[",
                      (unsigned long)service->revision);
    if (ret != AGENT_OK) {
        return ret;
    }

    for (i = 0; i < SMART_HOME_MAX_DEVICES; i++) {
        if (!service->state->devices[i].used) {
            continue;
        }
        ret = append_device_json(buffer, buffer_size, &offset,
                                 &service->state->devices[i], first);
        if (ret != AGENT_OK) {
            return ret;
        }
        first = 0;
    }

    return append_json(buffer, buffer_size, &offset,
                       "],\"environment\":{\"temperature\":%d,"
                       "\"humidity\":%d,\"ambientLight\":%d}}",
                       service->state->env_temperature,
                       service->state->env_humidity,
                       service->state->env_light);
}

static void publish_locked(smart_home_device_service_t *service,
                           const char *type,
                           const char *data_json,
                           smart_home_device_event_t *out_event)
{
    smart_home_device_event_t *event;

    service->revision++;
    event = &service->events[service->event_next];
    memset(event, 0, sizeof(*event));
    event->revision = service->revision;
    event->occurred_at_ms = device_service_now_ms();
    snprintf(event->event_id, sizeof(event->event_id), "evt-%lu",
             (unsigned long)service->next_event_id++);
    strncpy(event->type, type, sizeof(event->type) - 1u);
    strncpy(event->data_json, data_json, sizeof(event->data_json) - 1u);
    service->event_next = (service->event_next + 1u) %
                          SMART_HOME_DEVICE_EVENT_HISTORY;
    if (service->event_count < SMART_HOME_DEVICE_EVENT_HISTORY) {
        service->event_count++;
    }
    if (out_event) {
        *out_event = *event;
    }
}

static void notify_listener(smart_home_device_service_t *service,
                            const smart_home_device_event_t *event)
{
    smart_home_device_event_listener_t listener;
    void *listener_user_data;

    pthread_mutex_lock(&service->mutex);
    listener = service->listener;
    listener_user_data = service->listener_user_data;
    pthread_mutex_unlock(&service->mutex);
    if (listener) {
        listener(event, listener_user_data);
    }
}

int smart_home_device_service_init(smart_home_device_service_t *service,
                                   smart_home_state_t *state)
{
    if (!service || !state) {
        return AGENT_ERROR_INVALID;
    }

    memset(service, 0, sizeof(*service));
    if (pthread_mutex_init(&service->mutex, NULL) != 0) {
        return AGENT_ERROR;
    }
    service->mutex_initialized = 1;
    service->state = state;
    service->next_event_id = 1u;
    return AGENT_OK;
}

void smart_home_device_service_deinit(smart_home_device_service_t *service)
{
    if (!service) {
        return;
    }
    if (service->mutex_initialized) {
        pthread_mutex_destroy(&service->mutex);
    }
    memset(service, 0, sizeof(*service));
}

void smart_home_device_service_set_event_listener(
    smart_home_device_service_t *service,
    smart_home_device_event_listener_t listener,
    void *user_data)
{
    if (!service || !service->mutex_initialized) {
        return;
    }
    pthread_mutex_lock(&service->mutex);
    service->listener = listener;
    service->listener_user_data = user_data;
    pthread_mutex_unlock(&service->mutex);
}

int smart_home_device_service_set_scene_catalog(
    smart_home_device_service_t *service,
    const smart_home_scene_catalog_t *catalog)
{
    if (!service || !catalog || !service->mutex_initialized
        || catalog->version != SMART_HOME_SCENE_CATALOG_VERSION
        || catalog->scene_count == 0u
        || catalog->scene_count > SMART_HOME_SCENE_CATALOG_MAX_SCENES) {
        return AGENT_ERROR_INVALID;
    }

    pthread_mutex_lock(&service->mutex);
    service->scene_catalog = *catalog;
    pthread_mutex_unlock(&service->mutex);
    return AGENT_OK;
}

uint32_t smart_home_device_service_revision(smart_home_device_service_t *service)
{
    uint32_t revision = 0u;

    if (!service || !service->mutex_initialized) {
        return 0u;
    }
    pthread_mutex_lock(&service->mutex);
    revision = service->revision;
    pthread_mutex_unlock(&service->mutex);
    return revision;
}

int smart_home_device_service_copy_state(smart_home_device_service_t *service,
                                         smart_home_state_t *out_state)
{
    if (!service || !out_state || !service->mutex_initialized) {
        return AGENT_ERROR_INVALID;
    }
    pthread_mutex_lock(&service->mutex);
    *out_state = *service->state;
    pthread_mutex_unlock(&service->mutex);
    return AGENT_OK;
}

int smart_home_device_service_copy_state_with_revision(
    smart_home_device_service_t *service,
    smart_home_state_t *out_state,
    uint32_t *out_revision)
{
    if (!service || !out_state || !out_revision || !service->mutex_initialized) {
        return AGENT_ERROR_INVALID;
    }

    pthread_mutex_lock(&service->mutex);
    *out_state = *service->state;
    *out_revision = service->revision;
    pthread_mutex_unlock(&service->mutex);
    return AGENT_OK;
}

int smart_home_device_service_find_first(smart_home_device_service_t *service,
                                         const char *room,
                                         smart_home_device_type_t type,
                                         smart_home_device_t *out_device)
{
    smart_home_device_t *device;

    if (!service || !room || !out_device || !service->mutex_initialized) {
        return AGENT_ERROR_INVALID;
    }
    pthread_mutex_lock(&service->mutex);
    device = smart_home_device_find_first(service->state, room, type);
    if (device) {
        *out_device = *device;
    }
    pthread_mutex_unlock(&service->mutex);
    return device ? AGENT_OK : AGENT_ERROR_NOT_FOUND;
}

int smart_home_device_service_build_snapshot_json(
    smart_home_device_service_t *service, char *buffer, size_t buffer_size)
{
    int ret;

    if (!service || !buffer || buffer_size == 0u || !service->mutex_initialized) {
        return AGENT_ERROR_INVALID;
    }
    pthread_mutex_lock(&service->mutex);
    ret = build_snapshot_locked(service, buffer, buffer_size);
    pthread_mutex_unlock(&service->mutex);
    return ret;
}

int smart_home_device_service_build_events_json(smart_home_device_service_t *service,
                                                uint32_t after_revision,
                                                char *buffer,
                                                size_t buffer_size)
{
    size_t offset = 0u;
    size_t first_index;
    size_t i;
    int first = 1;
    int ret;

    if (!service || !buffer || buffer_size == 0u || !service->mutex_initialized) {
        return AGENT_ERROR_INVALID;
    }

    pthread_mutex_lock(&service->mutex);
    ret = append_json(buffer, buffer_size, &offset, "{\"events\":[");
    first_index = (service->event_next + SMART_HOME_DEVICE_EVENT_HISTORY -
                   service->event_count) % SMART_HOME_DEVICE_EVENT_HISTORY;
    for (i = 0; ret == AGENT_OK && i < service->event_count; i++) {
        const smart_home_device_event_t *event =
            &service->events[(first_index + i) % SMART_HOME_DEVICE_EVENT_HISTORY];
        if (event->revision <= after_revision) {
            continue;
        }
        ret = append_json(buffer, buffer_size, &offset,
                          "%s{\"eventId\":\"%s\",\"type\":\"%s\","
                          "\"revision\":%lu,\"occurredAt\":%llu,"
                          "\"data\":%s}",
                          first ? "" : ",", event->event_id, event->type,
                          (unsigned long)event->revision,
                          (unsigned long long)event->occurred_at_ms,
                          event->data_json);
        first = 0;
    }
    if (ret == AGENT_OK) {
        ret = append_json(buffer, buffer_size, &offset,
                          "],\"latestRevision\":%lu}",
                          (unsigned long)service->revision);
    }
    pthread_mutex_unlock(&service->mutex);
    return ret;
}

static int commit_mutation(smart_home_device_service_t *service,
                           int ret,
                           const char *event_type,
                           const char *event_data)
{
    smart_home_device_event_t event;

    if (ret != AGENT_OK) {
        pthread_mutex_unlock(&service->mutex);
        return ret;
    }
    publish_locked(service, event_type, event_data, &event);
    pthread_mutex_unlock(&service->mutex);
    notify_listener(service, &event);
    return AGENT_OK;
}

int smart_home_device_service_add(smart_home_device_service_t *service,
                                  const char *room, const char *name,
                                  smart_home_device_type_t type, int *device_id)
{
    char data[SMART_HOME_DEVICE_EVENT_DATA_SIZE];
    int id = 0;
    int ret;

    if (!service || !service->mutex_initialized) {
        return AGENT_ERROR_INVALID;
    }
    pthread_mutex_lock(&service->mutex);
    ret = smart_home_device_add(service->state, room, name, type, &id);
    snprintf(data, sizeof(data), "{\"deviceId\":\"%d\",\"operation\":\"added\"}", id);
    ret = commit_mutation(service, ret, "device_state_changed", data);
    if (ret == AGENT_OK && device_id) {
        *device_id = id;
    }
    return ret;
}

int smart_home_device_service_add_room(smart_home_device_service_t *service,
                                       const char *room)
{
    char data[SMART_HOME_DEVICE_EVENT_DATA_SIZE];
    int ret;

    if (!service || !room || !service->mutex_initialized) {
        return AGENT_ERROR_INVALID;
    }

    pthread_mutex_lock(&service->mutex);
    ret = smart_home_room_add(service->state, room);
    snprintf(data, sizeof(data),
             "{\"room\":\"%s\",\"operation\":\"room_added\"}", room);
    /* Room changes are state changes as well.  Reuse the established event
     * category so existing state-change consumers observe the update. */
    return commit_mutation(service, ret, "device_state_changed", data);
}

int smart_home_device_service_update_meta(smart_home_device_service_t *service,
                                          int device_id, const char *room,
                                          const char *name)
{
    char data[SMART_HOME_DEVICE_EVENT_DATA_SIZE];
    int ret;

    if (!service || !service->mutex_initialized) {
        return AGENT_ERROR_INVALID;
    }
    pthread_mutex_lock(&service->mutex);
    ret = smart_home_device_update_meta(service->state, device_id, room, name);
    snprintf(data, sizeof(data), "{\"deviceId\":\"%d\",\"operation\":\"metadata_updated\"}", device_id);
    return commit_mutation(service, ret, "device_state_changed", data);
}

int smart_home_device_service_remove(smart_home_device_service_t *service,
                                     int device_id)
{
    char data[SMART_HOME_DEVICE_EVENT_DATA_SIZE];
    int ret;

    if (!service || !service->mutex_initialized) {
        return AGENT_ERROR_INVALID;
    }
    pthread_mutex_lock(&service->mutex);
    ret = smart_home_device_remove(service->state, device_id);
    snprintf(data, sizeof(data), "{\"deviceId\":\"%d\",\"operation\":\"removed\"}", device_id);
    return commit_mutation(service, ret, "device_state_changed", data);
}

int smart_home_device_service_set_light(smart_home_device_service_t *service,
                                        const char *room, int on, int brightness)
{
    char data[SMART_HOME_DEVICE_EVENT_DATA_SIZE];
    smart_home_device_t *device;
    int ret;

    if (!service || !service->mutex_initialized) {
        return AGENT_ERROR_INVALID;
    }
    pthread_mutex_lock(&service->mutex);
    ret = smart_home_device_set_light(service->state, room, on, brightness);
    device = ret == AGENT_OK ? smart_home_device_find_first(service->state, room,
                                                              SMART_HOME_DEVICE_LIGHT) : NULL;
    snprintf(data, sizeof(data), "{\"deviceId\":\"%d\",\"state\":{\"on\":%s,\"brightness\":%d}}",
             device ? device->id : 0, on ? "true" : "false",
             device ? device->brightness : brightness);
    return commit_mutation(service, ret, "device_state_changed", data);
}

int smart_home_device_service_set_light_if_revision(
    smart_home_device_service_t *service,
    const char *room,
    uint32_t expected_revision,
    int on,
    int brightness)
{
    char data[SMART_HOME_DEVICE_EVENT_DATA_SIZE];
    smart_home_device_t *device;
    int ret;

    if (!service || !service->mutex_initialized) {
        return AGENT_ERROR_INVALID;
    }

    pthread_mutex_lock(&service->mutex);
    if (service->revision != expected_revision) {
        pthread_mutex_unlock(&service->mutex);
        return SMART_HOME_DEVICE_SERVICE_ERROR_STALE_REVISION;
    }

    ret = smart_home_device_set_light(service->state, room, on, brightness);
    device = ret == AGENT_OK ? smart_home_device_find_first(service->state, room,
                                                              SMART_HOME_DEVICE_LIGHT) : NULL;
    snprintf(data, sizeof(data),
             "{\"deviceId\":\"%d\",\"state\":{\"on\":%s,\"brightness\":%d}}",
             device ? device->id : 0, on ? "true" : "false",
             device ? device->brightness : brightness);
    return commit_mutation(service, ret, "device_state_changed", data);
}

int smart_home_device_service_set_ac(smart_home_device_service_t *service,
                                     const char *room, int on, int mode,
                                     int fan_speed, int temperature)
{
    char data[SMART_HOME_DEVICE_EVENT_DATA_SIZE];
    smart_home_device_t *device;
    int ret;

    if (!service || !service->mutex_initialized) {
        return AGENT_ERROR_INVALID;
    }
    pthread_mutex_lock(&service->mutex);
    ret = smart_home_device_set_ac(service->state, room, on, mode, fan_speed,
                                   temperature);
    device = ret == AGENT_OK ? smart_home_device_find_first(service->state, room,
                                                              SMART_HOME_DEVICE_AC) : NULL;
    snprintf(data, sizeof(data), "{\"deviceId\":\"%d\",\"state\":{\"on\":%s,\"temperature\":%d,\"mode\":\"%s\",\"fanSpeed\":\"%s\"}}",
             device ? device->id : 0, on ? "true" : "false",
             device ? device->temperature : temperature,
             smart_home_ac_mode_name(device ? device->ac_mode : mode),
             smart_home_ac_fan_speed_name(device ? device->ac_fan_speed : fan_speed));
    return commit_mutation(service, ret, "device_state_changed", data);
}

int smart_home_device_service_set_device_control(
    smart_home_device_service_t *service, int device_id, int on, int value,
    int mode, int fan_speed)
{
    smart_home_device_t *device;
    char room[SMART_HOME_ROOM_NAME_SIZE];
    smart_home_device_type_t type;

    if (!service || !service->mutex_initialized) {
        return AGENT_ERROR_INVALID;
    }
    pthread_mutex_lock(&service->mutex);
    device = smart_home_device_find_by_id(service->state, device_id);
    if (!device) {
        pthread_mutex_unlock(&service->mutex);
        return AGENT_ERROR_NOT_FOUND;
    }
    type = device->type;
    strncpy(room, device->room, sizeof(room) - 1u);
    room[sizeof(room) - 1u] = '\0';
    pthread_mutex_unlock(&service->mutex);
    if (type == SMART_HOME_DEVICE_AC) {
        return smart_home_device_service_set_ac(service, room, on, mode,
                                                fan_speed, value);
    }
    return smart_home_device_service_set_light(service, room, on, value);
}

int smart_home_device_service_set_environment(smart_home_device_service_t *service,
                                              int temperature, int humidity,
                                              int ambient_light)
{
    char data[SMART_HOME_DEVICE_EVENT_DATA_SIZE];
    int ret;

    if (!service || !service->mutex_initialized) {
        return AGENT_ERROR_INVALID;
    }
    pthread_mutex_lock(&service->mutex);
    ret = smart_home_device_set_environment(service->state, temperature, humidity,
                                            ambient_light);
    snprintf(data, sizeof(data), "{\"environment\":{\"temperature\":%d,\"humidity\":%d,\"ambientLight\":%d}}",
             service->state->env_temperature, service->state->env_humidity,
             service->state->env_light);
    return commit_mutation(service, ret, "environment_changed", data);
}

int smart_home_device_service_run_scene(smart_home_device_service_t *service,
                                        const char *scene)
{
    char data[SMART_HOME_DEVICE_EVENT_DATA_SIZE];
    const smart_home_scene_t *matched = NULL;
    size_t i;

    if (!service || !scene || !scene[0] || !service->mutex_initialized) {
        return AGENT_ERROR_INVALID;
    }
    pthread_mutex_lock(&service->mutex);
    if (service->scene_catalog.version != SMART_HOME_SCENE_CATALOG_VERSION) {
        pthread_mutex_unlock(&service->mutex);
        return AGENT_ERROR_NOT_FOUND;
    }
    for (i = 0u; i < service->scene_catalog.scene_count; i++) {
        if (strcmp(service->scene_catalog.scenes[i].id, scene) == 0) {
            matched = &service->scene_catalog.scenes[i];
            break;
        }
    }
    if (!matched) {
        pthread_mutex_unlock(&service->mutex);
        return AGENT_ERROR_NOT_FOUND;
    }

    /* Validate every target before changing state, so a scene is atomic. */
    for (i = 0u; i < matched->action_count; i++) {
        const smart_home_scene_action_t *action = &matched->actions[i];

        if (!smart_home_device_find_first(service->state, action->room,
                                          action->device_type)) {
            pthread_mutex_unlock(&service->mutex);
            return AGENT_ERROR_NOT_FOUND;
        }
    }
    for (i = 0u; i < matched->action_count; i++) {
        const smart_home_scene_action_t *action = &matched->actions[i];
        int ret;

        if (action->device_type == SMART_HOME_DEVICE_LIGHT) {
            ret = smart_home_device_set_light(service->state, action->room,
                                               action->on, action->brightness);
        } else {
            ret = smart_home_device_set_ac(service->state, action->room,
                                            action->on, action->ac_mode,
                                            action->ac_fan_speed,
                                            action->temperature);
        }
        if (ret != AGENT_OK) {
            pthread_mutex_unlock(&service->mutex);
            return ret;
        }
    }
    snprintf(data, sizeof(data), "{\"sceneId\":\"%s\"}", scene);
    return commit_mutation(service, AGENT_OK, "scene_progress", data);
}

static agent_error_t build_device_context(void *user_data,
                                          const agent_context_request_t *request,
                                          const agent_text_sink_t *output)
{
    char snapshot[2048];
    int ret;
    (void)request;
    ret = smart_home_device_service_build_snapshot_json(user_data, snapshot, sizeof(snapshot));
    if (ret != AGENT_OK) return (agent_error_t)ret;
    return output->write(output->context, agent_string_view(snapshot, strlen(snapshot)));
}

int smart_home_device_service_register_context(agent_t *agent,
                                                smart_home_device_service_t *service)
{
    const agent_context_provider_t provider = {
        .name = AGENT_SV_LITERAL("smart_home_state"),
        .priority = 80, .required = false,
        .build = build_device_context, .user_data = service,
        .max_bytes = AGENT_MAX_INPUT_BYTES < 2048u ? AGENT_MAX_INPUT_BYTES : 2048u,
        .placement = AGENT_CONTEXT_REFERENCE
    };
    if (!agent || !service) return AGENT_ERROR_INVALID;
    return agent_register_context(agent, &provider);
}
