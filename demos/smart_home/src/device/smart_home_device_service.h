#pragma once

#include "smart_home_device.h"
#include "../skills/smart_home_scene_catalog.h"

#include <pthread.h>
#include <stdint.h>

#define SMART_HOME_DEVICE_EVENT_HISTORY 16
#define SMART_HOME_DEVICE_EVENT_TYPE_SIZE 32
#define SMART_HOME_DEVICE_EVENT_ID_SIZE 24
#define SMART_HOME_DEVICE_EVENT_DATA_SIZE 384

/* A mutation was rejected because its caller used an obsolete revision. */
#define SMART_HOME_DEVICE_SERVICE_ERROR_STALE_REVISION (-1001)

typedef struct {
    uint32_t revision;
    uint64_t occurred_at_ms;
    char event_id[SMART_HOME_DEVICE_EVENT_ID_SIZE];
    char type[SMART_HOME_DEVICE_EVENT_TYPE_SIZE];
    char data_json[SMART_HOME_DEVICE_EVENT_DATA_SIZE];
} smart_home_device_event_t;

typedef void (*smart_home_device_event_listener_t)(
    const smart_home_device_event_t *event,
    void *user_data);

typedef struct {
    smart_home_state_t *state;
    pthread_mutex_t mutex;
    int mutex_initialized;
    uint32_t revision;
    uint32_t next_event_id;
    smart_home_scene_catalog_t scene_catalog;
    smart_home_device_event_t events[SMART_HOME_DEVICE_EVENT_HISTORY];
    size_t event_count;
    size_t event_next;
    smart_home_device_event_listener_t listener;
    void *listener_user_data;
} smart_home_device_service_t;

int smart_home_device_service_init(smart_home_device_service_t *service,
                                   smart_home_state_t *state);
void smart_home_device_service_deinit(smart_home_device_service_t *service);

void smart_home_device_service_set_event_listener(
    smart_home_device_service_t *service,
    smart_home_device_event_listener_t listener,
    void *user_data);

int smart_home_device_service_set_scene_catalog(
    smart_home_device_service_t *service,
    const smart_home_scene_catalog_t *catalog);

uint32_t smart_home_device_service_revision(smart_home_device_service_t *service);
int smart_home_device_service_copy_state(smart_home_device_service_t *service,
                                         smart_home_state_t *out_state);
int smart_home_device_service_copy_state_with_revision(
    smart_home_device_service_t *service,
    smart_home_state_t *out_state,
    uint32_t *out_revision);
int smart_home_device_service_find_first(smart_home_device_service_t *service,
                                         const char *room,
                                         smart_home_device_type_t type,
                                         smart_home_device_t *out_device);
int smart_home_device_service_build_snapshot_json(
    smart_home_device_service_t *service,
    char *buffer,
    size_t buffer_size);
int smart_home_device_service_build_events_json(smart_home_device_service_t *service,
                                                uint32_t after_revision,
                                                char *buffer,
                                                size_t buffer_size);

int smart_home_device_service_add(smart_home_device_service_t *service,
                                  const char *room,
                                  const char *name,
                                  smart_home_device_type_t type,
                                  int *device_id);
int smart_home_device_service_add_room(smart_home_device_service_t *service,
                                       const char *room);
int smart_home_device_service_update_meta(smart_home_device_service_t *service,
                                          int device_id,
                                          const char *room,
                                          const char *name);
int smart_home_device_service_remove(smart_home_device_service_t *service,
                                     int device_id);
int smart_home_device_service_set_light(smart_home_device_service_t *service,
                                        const char *room,
                                        int on,
                                        int brightness);
int smart_home_device_service_set_light_if_revision(
    smart_home_device_service_t *service,
    const char *room,
    uint32_t expected_revision,
    int on,
    int brightness);
int smart_home_device_service_set_ac(smart_home_device_service_t *service,
                                     const char *room,
                                     int on,
                                     int mode,
                                     int fan_speed,
                                     int temperature);
int smart_home_device_service_set_device_control(
    smart_home_device_service_t *service,
    int device_id,
    int on,
    int value,
    int mode,
    int fan_speed);
int smart_home_device_service_set_environment(smart_home_device_service_t *service,
                                              int temperature,
                                              int humidity,
                                              int ambient_light);
int smart_home_device_service_run_scene(smart_home_device_service_t *service,
                                        const char *scene);

int smart_home_device_service_register_context(agent_t *agent,
                                                smart_home_device_service_t *service);
