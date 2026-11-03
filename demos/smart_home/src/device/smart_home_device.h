#pragma once

#include <stddef.h>

#include <agent.h>
#include "../config/cjson_compat.h"

#define SMART_HOME_MAX_DEVICES 8
#define SMART_HOME_MAX_ROOMS 6
#define SMART_HOME_ROOM_NAME_SIZE 24
#define SMART_HOME_ROOM_BINDINGS 17
#define SMART_HOME_BOARD_CAMERA_KEY "board:camera:0"
#define SMART_HOME_ROOM_KEY_SIZE 48
#define SMART_HOME_DEVICE_NAME_SIZE 32

typedef enum {
    SMART_HOME_DEVICE_LIGHT = 0,
    SMART_HOME_DEVICE_AC = 1,
} smart_home_device_type_t;

typedef struct {
    int used;
    int id;
    char room[SMART_HOME_ROOM_NAME_SIZE];
    char name[SMART_HOME_DEVICE_NAME_SIZE];
    smart_home_device_type_t type;
    int on;
    int brightness;
    int temperature;
    int ac_mode;
    int ac_fan_speed;
} smart_home_device_t;

typedef struct {
    smart_home_device_t devices[SMART_HOME_MAX_DEVICES];
    struct { char key[SMART_HOME_ROOM_KEY_SIZE]; char room[SMART_HOME_ROOM_NAME_SIZE]; }
        room_bindings[SMART_HOME_ROOM_BINDINGS];
    char rooms[SMART_HOME_MAX_ROOMS][SMART_HOME_ROOM_NAME_SIZE];
    int room_count;
    int next_device_id;
    int env_temperature;
    int env_humidity;
    int env_light;
} smart_home_state_t;

void smart_home_device_init(smart_home_state_t *state);
/** Remove only the two retired factory examples when loading older state. */
void smart_home_device_remove_legacy_demos(smart_home_state_t *state);

/* ── 序列化（state.json 持久化用） ── */

/**
 * 将 typed state 序列化为 cJSON（version 1 + rooms + devices + environment）。
 * 返回堆 cJSON *，调用者 cJSON_Delete 释放；失败返回 NULL。
 */
cJSON *smart_home_device_state_to_json(const smart_home_state_t *state);

/**
 * 从 cJSON 恢复 typed state（覆盖式）。
 * @return AGENT_OK 或 AGENT_ERROR_PARSE / LIMIT / INVALID
 */
int smart_home_device_state_from_json(smart_home_state_t *state,
                                      const cJSON *root);

const char *smart_home_device_type_name(smart_home_device_type_t type);
smart_home_device_type_t smart_home_device_type_from_index(int index);
int smart_home_device_type_to_index(smart_home_device_type_t type);
const char *smart_home_room_from_index(int index);
int smart_home_room_to_index(const char *room);

/* Rooms are persisted independently from devices, so an empty room can be
 * created before a device is assigned to it. */
int smart_home_room_count(const smart_home_state_t *state);
const char *smart_home_room_get(const smart_home_state_t *state, int index);
int smart_home_room_index(const smart_home_state_t *state, const char *room);
int smart_home_room_add(smart_home_state_t *state, const char *room);
int smart_home_room_delete_and_reassign(smart_home_state_t *state,
                                        const char *source_room,
                                        const char *target_room);

int smart_home_device_count(const smart_home_state_t *state);
smart_home_device_t *smart_home_device_get_by_slot(smart_home_state_t *state,
                                                   int slot);
const smart_home_device_t *smart_home_device_get_const_by_slot(
    const smart_home_state_t *state,
    int slot);
smart_home_device_t *smart_home_device_find_by_id(smart_home_state_t *state,
                                                  int id);
smart_home_device_t *smart_home_device_find_first(smart_home_state_t *state,
                                                  const char *room,
                                                  smart_home_device_type_t type);

int smart_home_device_add(smart_home_state_t *state,
                          const char *room,
                          const char *name,
                          smart_home_device_type_t type,
                          int *device_id);
int smart_home_device_update_meta(smart_home_state_t *state,
                                  int device_id,
                                  const char *room,
                                  const char *name);
int smart_home_device_remove(smart_home_state_t *state, int device_id);

int smart_home_device_set_light(smart_home_state_t *state,
                                const char *room,
                                int on,
                                int brightness);

int smart_home_device_set_ac(smart_home_state_t *state,
                             const char *room,
                             int on,
                             int mode,
                             int fan_speed,
                             int temperature);
int smart_home_device_set_control_by_id(smart_home_state_t *state, int id,
                                        int on, int value, int mode,
                                        int fan_speed);

int smart_home_device_set_environment(smart_home_state_t *state,
                                      int temperature,
                                      int humidity,
                                      int ambient_light);

const char *smart_home_ac_mode_name(int mode);
const char *smart_home_ac_fan_speed_name(int speed);
int smart_home_ac_parse_mode(const char *name);
int smart_home_ac_parse_fan_speed(const char *name);

int smart_home_device_build_status_json(const smart_home_state_t *state,
                                        char *buffer,
                                        size_t buffer_size);

/* Stable remote keys are panel-local room overrides, independent of cloud rooms. */
const char *smart_home_device_room_override(const smart_home_state_t *state, const char *key);
int smart_home_device_assign_room(smart_home_state_t *state, const char *key,
                                   const char *room);
