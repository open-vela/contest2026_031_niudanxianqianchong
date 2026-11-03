#include "smart_home_quickapp_provider.h"

#include <string.h>

static int snapshot_from_service(smart_home_device_service_t *service,
                                 smart_home_quickapp_snapshot_t *out_snapshot)
{
    smart_home_state_t state;
    smart_home_device_t *light;
    int ret;

    if (!service || !out_snapshot) {
        return AGENT_ERROR_INVALID;
    }

    ret = smart_home_device_service_copy_state_with_revision(service, &state,
                                                              &out_snapshot->revision);
    if (ret != AGENT_OK) {
        return ret;
    }

    light = smart_home_device_find_first(&state, "living_room",
                                         SMART_HOME_DEVICE_LIGHT);
    if (!light) {
        return AGENT_ERROR_NOT_FOUND;
    }

    out_snapshot->online = 1;
    out_snapshot->power = light->on ? 1 : 0;
    out_snapshot->brightness = light->brightness;
    return AGENT_OK;
}

static int valid_request_id(const char *request_id)
{
    size_t length;

    if (!request_id || request_id[0] == '\0') {
        return 0;
    }

    length = strnlen(request_id, SMART_HOME_QUICKAPP_REQUEST_ID_SIZE);
    return length > 0u && length < SMART_HOME_QUICKAPP_REQUEST_ID_SIZE;
}

static smart_home_quickapp_request_cache_entry_t *find_request(
    smart_home_quickapp_provider_t *provider,
    const char *request_id)
{
    size_t index;

    for (index = 0u; index < SMART_HOME_QUICKAPP_REQUEST_CACHE_SIZE; index++) {
        smart_home_quickapp_request_cache_entry_t *entry = &provider->requests[index];

        if (entry->request_id[0] != '\0' && strcmp(entry->request_id, request_id) == 0) {
            return entry;
        }
    }

    return NULL;
}

static smart_home_quickapp_request_cache_entry_t *reserve_request(
    smart_home_quickapp_provider_t *provider,
    const char *request_id)
{
    smart_home_quickapp_request_cache_entry_t *entry;

    entry = &provider->requests[provider->next_request_slot];
    provider->next_request_slot = (provider->next_request_slot + 1u) %
                                  SMART_HOME_QUICKAPP_REQUEST_CACHE_SIZE;
    memset(entry, 0, sizeof(*entry));
    strncpy(entry->request_id, request_id, sizeof(entry->request_id) - 1u);
    entry->pending = 1;
    return entry;
}

int smart_home_quickapp_provider_init(
    smart_home_quickapp_provider_t *provider,
    smart_home_device_service_t *device_service)
{
    uint32_t revision;
    int ret;

    if (!provider || !device_service) {
        return AGENT_ERROR_INVALID;
    }

    memset(provider, 0, sizeof(*provider));
    if (pthread_mutex_init(&provider->mutex, NULL) != 0) {
        return AGENT_ERROR;
    }

    provider->mutex_initialized = 1;
    provider->device_service = device_service;
    revision = smart_home_device_service_revision(device_service);
    if (revision != 0u) {
        return AGENT_OK;
    }

    /* The P0 fixture starts with one known light and a non-zero revision. */
    ret = smart_home_device_service_set_light(device_service, "living_room", 0,
                                              SMART_HOME_QUICKAPP_DEFAULT_BRIGHTNESS);
    if (ret != AGENT_OK) {
        smart_home_quickapp_provider_deinit(provider);
    }
    return ret;
}

void smart_home_quickapp_provider_deinit(smart_home_quickapp_provider_t *provider)
{
    if (!provider) {
        return;
    }

    if (provider->mutex_initialized) {
        pthread_mutex_destroy(&provider->mutex);
    }
    memset(provider, 0, sizeof(*provider));
}

int smart_home_quickapp_provider_get_snapshot(
    smart_home_quickapp_provider_t *provider,
    smart_home_quickapp_snapshot_t *out_snapshot)
{
    if (!provider || !provider->mutex_initialized) {
        return AGENT_ERROR_INVALID;
    }

    return snapshot_from_service(provider->device_service, out_snapshot);
}

int smart_home_quickapp_provider_control(
    smart_home_quickapp_provider_t *provider,
    const char *request_id,
    const char *device_id,
    const char *command,
    int value,
    uint32_t expected_revision,
    smart_home_quickapp_snapshot_t *out_snapshot)
{
    smart_home_quickapp_request_cache_entry_t *entry;
    smart_home_quickapp_snapshot_t current;
    int brightness;
    int ret;

    if (!provider || !provider->mutex_initialized || !out_snapshot ||
        !valid_request_id(request_id) ||
        !device_id || strcmp(device_id, SMART_HOME_QUICKAPP_DEVICE_ID) != 0 ||
        !command || strcmp(command, SMART_HOME_QUICKAPP_COMMAND_SET_POWER) != 0 ||
        (value != 0 && value != 1)) {
        return AGENT_ERROR_INVALID;
    }

    pthread_mutex_lock(&provider->mutex);
    entry = find_request(provider, request_id);
    if (entry) {
        if (entry->pending) {
            pthread_mutex_unlock(&provider->mutex);
            return AGENT_ERROR_BUSY;
        }
        *out_snapshot = entry->snapshot;
        ret = entry->result;
        pthread_mutex_unlock(&provider->mutex);
        return ret;
    }
    entry = reserve_request(provider, request_id);
    pthread_mutex_unlock(&provider->mutex);

    ret = snapshot_from_service(provider->device_service, &current);
    if (ret == AGENT_OK) {
        brightness = current.brightness;
        if (value && brightness == 0) {
            brightness = SMART_HOME_QUICKAPP_DEFAULT_BRIGHTNESS;
        }
        ret = smart_home_device_service_set_light_if_revision(
            provider->device_service, "living_room", expected_revision, value,
            brightness);
    }

    if (snapshot_from_service(provider->device_service, &current) != AGENT_OK) {
        ret = AGENT_ERROR;
        memset(&current, 0, sizeof(current));
    }

    pthread_mutex_lock(&provider->mutex);
    entry->result = ret;
    entry->snapshot = current;
    entry->pending = 0;
    *out_snapshot = current;
    pthread_mutex_unlock(&provider->mutex);
    return ret;
}
