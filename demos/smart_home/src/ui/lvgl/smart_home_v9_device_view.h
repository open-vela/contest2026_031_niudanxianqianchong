/** V9 read-only device view and asynchronous control state machine. */

#pragma once

#include "../../device/smart_home_device.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
#include "../../miloco/smart_home_miloco.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SMART_HOME_V9_MAX_VIEWS 16
#define SMART_HOME_V9_KEY_SIZE 48
#define SMART_HOME_V9_NAME_SIZE 40
#define SMART_HOME_V9_ROOM_SIZE 24

typedef enum {
    SMART_HOME_V9_SOURCE_LOCAL = 0,
    SMART_HOME_V9_SOURCE_MILOCO,
    SMART_HOME_V9_SOURCE_NODE,
    SMART_HOME_V9_SOURCE_DEMO,
} smart_home_v9_source_t;

typedef enum {
    SMART_HOME_V9_KIND_LIGHT = 0,
    SMART_HOME_V9_KIND_AC,
    SMART_HOME_V9_KIND_WASHER,
    SMART_HOME_V9_KIND_ROBOT,
    SMART_HOME_V9_KIND_CAMERA,
    SMART_HOME_V9_KIND_FAN,
    SMART_HOME_V9_KIND_UNKNOWN,
} smart_home_v9_kind_t;

enum {
    SMART_HOME_V9_CAP_POWER = 1u << 0,
    SMART_HOME_V9_CAP_BRIGHTNESS = 1u << 1,
    SMART_HOME_V9_CAP_TEMPERATURE = 1u << 2,
    SMART_HOME_V9_CAP_FAN_SPEED = 1u << 3,
    SMART_HOME_V9_CAP_PREVIEW = 1u << 4,
    SMART_HOME_V9_CAP_PAUSE = 1u << 5,
    SMART_HOME_V9_CAP_DOCK = 1u << 6,
};

typedef struct {
    char key[SMART_HOME_V9_KEY_SIZE];
    smart_home_v9_source_t source;
    smart_home_v9_kind_t kind;
    char name[SMART_HOME_V9_NAME_SIZE];
    char room_id[SMART_HOME_V9_ROOM_SIZE];
    char room_label[SMART_HOME_V9_ROOM_SIZE];
    bool online;
    bool state_valid;
    bool on;
    int primary_value;
    const char *unit;
    int activity; /* closed/running/paused/standby/error, source-defined */
    unsigned capabilities;
} smart_home_v9_device_view_t;

typedef enum {
    SMART_HOME_V9_CONTROL_IDLE = 0,
    SMART_HOME_V9_CONTROL_QUEUED,
    SMART_HOME_V9_CONTROL_EXECUTING,
    SMART_HOME_V9_CONTROL_WAIT_READBACK,
    SMART_HOME_V9_CONTROL_SUCCEEDED,
    SMART_HOME_V9_CONTROL_FAILED,
    SMART_HOME_V9_CONTROL_TIMED_OUT,
} smart_home_v9_control_state_t;

typedef struct {
    smart_home_v9_control_state_t state;
    char key[SMART_HOME_V9_KEY_SIZE];
    int desired_on;
    unsigned request_id;
    uint32_t started_ms;
} smart_home_v9_control_t;

/* Collect local and Miloco snapshots into a presentation-only list.  The
 * result never owns device state; callers must re-read it after a revision. */
size_t smart_home_v9_collect_views(const smart_home_state_t *local,
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
                                   const smart_home_miloco_t *miloco,
#endif
                                   smart_home_v9_device_view_t *out,
                                   size_t capacity);

/* Six design-template views for the M2/M3 demo fallback. */
size_t smart_home_v9_demo_views(smart_home_v9_device_view_t *out,
                                size_t capacity);

void smart_home_v9_control_begin(smart_home_v9_control_t *control,
                                 const smart_home_v9_device_view_t *view,
                                 int desired_on,
                                 unsigned request_id,
                                 uint32_t now_ms);
void smart_home_v9_control_result(smart_home_v9_control_t *control, int result);
/* Confirm when a fresh snapshot matches the desired value; timeout otherwise. */
void smart_home_v9_control_snapshot(smart_home_v9_control_t *control,
                                    const smart_home_v9_device_view_t *view,
                                    uint32_t now_ms,
                                    uint32_t timeout_ms);

const char *smart_home_v9_control_state_name(smart_home_v9_control_state_t state);

#ifdef __cplusplus
}
#endif
