/** V9 device view collection and async control state machine. */

#include "smart_home_v9_device_view.h"

#include <stdio.h>
#include <string.h>

#include "../../smart_home_memory.h"
static void copy_text(char *dst, size_t size, const char *src)
{
    if (!dst || size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, size - 1);
    dst[size - 1] = '\0';
}

static smart_home_v9_kind_t kind_from_local(smart_home_device_type_t type)
{
    switch (type) {
    case SMART_HOME_DEVICE_LIGHT:
        return SMART_HOME_V9_KIND_LIGHT;
    case SMART_HOME_DEVICE_AC:
        return SMART_HOME_V9_KIND_AC;
    default:
        return SMART_HOME_V9_KIND_UNKNOWN;
    }
}

#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
static bool control_has(const smart_home_miloco_control_t *control,
                        const char *needle)
{
    return (control->type_name[0] &&
            strstr(control->type_name, needle) != NULL) ||
           (control->desc[0] && strstr(control->desc, needle) != NULL);
}

static smart_home_v9_kind_t kind_from_miloco(
    const smart_home_miloco_device_t *device)
{
    switch (device->category) {
    case SMART_HOME_MILOCO_CATEGORY_LIGHT:
        return SMART_HOME_V9_KIND_LIGHT;
    case SMART_HOME_MILOCO_CATEGORY_AC:
        return SMART_HOME_V9_KIND_AC;
    case SMART_HOME_MILOCO_CATEGORY_CAMERA:
        return SMART_HOME_V9_KIND_CAMERA;
    case SMART_HOME_MILOCO_CATEGORY_FAN:
        return SMART_HOME_V9_KIND_FAN;
    default:
        break;
    }

    /* OTHER is common for washers and robot vacuums.  Use spec-provided
     * control type_name/description anchors; never infer from device name. */
    for (uint8_t i = 0; i < device->control_count; i++) {
        const smart_home_miloco_control_t *control = &device->controls[i];

        if (control_has(control, "wash") || control_has(control, "洗涤") ||
            control_has(control, "wash-mode") ||
            control_has(control, "remaining-time")) {
            return SMART_HOME_V9_KIND_WASHER;
        }
        if (control_has(control, "sweep") || control_has(control, "vacuum") ||
            control_has(control, "robot") || control_has(control, "mop") ||
            control_has(control, "dock") || control_has(control, "battery") ||
            control_has(control, "清扫") || control_has(control, "回充") ||
            control_has(control, "电量")) {
            return SMART_HOME_V9_KIND_ROBOT;
        }
    }
    return SMART_HOME_V9_KIND_UNKNOWN;
}

#endif

static smart_home_v9_device_view_t *append_view(
    smart_home_v9_device_view_t *out, size_t capacity, size_t *count)
{
    if (*count >= capacity) {
        return NULL;
    }
    memset(&out[*count], 0, sizeof(out[*count]));
    return &out[(*count)++];
}

#ifdef CONFIG_SMART_HOME_CAMERA_PREVIEW
#include "../../camera/smart_home_camera_service.h"
#include "../../camera/smart_home_camera_control.h"
#endif
int smart_home_v9_board_camera_view(const smart_home_state_t *local, smart_home_v9_device_view_t *out)
{
#ifdef CONFIG_SMART_HOME_CAMERA_PREVIEW
    if (!local || !out || local->room_count < 1) return -1;
    memset(out, 0, sizeof(*out));
    copy_text(out->key, sizeof out->key, SMART_HOME_BOARD_CAMERA_KEY);
    copy_text(out->name, sizeof out->name, "本机摄像头");
    const char *room = smart_home_device_room_override(local, SMART_HOME_BOARD_CAMERA_KEY);
    if (!room) room = smart_home_room_index(local, "living_room") >= 0 ? "living_room" : local->rooms[0];
    copy_text(out->room_id, sizeof out->room_id, room);
    copy_text(out->room_label, sizeof out->room_label, room);
    out->source = SMART_HOME_V9_SOURCE_BOARD;
    out->kind = SMART_HOME_V9_KIND_CAMERA;
    out->capabilities = SMART_HOME_V9_CAP_PREVIEW | SMART_HOME_V9_CAP_POWER;
    struct smart_home_camera_status_s status;
    smart_home_camera_control_status_t control;
    smart_home_camera_control_status(&control);
    if (smart_home_camera_get_status(&status)) return -1;
    out->state_valid = true;
    out->online = status.state != SMART_HOME_CAMERA_ERROR;
    out->on = status.state == SMART_HOME_CAMERA_RUNNING || status.state == SMART_HOME_CAMERA_STARTING;
    out->primary_value = status.preview_fps_x100 / 100;
    out->unit = "FPS";
    out->activity = SMART_HOME_V9_CAMERA_OFF;
    if (control.busy) {
        out->on = control.desired_on;
        out->activity = control.desired_on ? SMART_HOME_V9_CAMERA_STARTING : SMART_HOME_V9_CAMERA_STOPPING;
    } else if (control.result || status.state == SMART_HOME_CAMERA_ERROR) {
        out->activity = SMART_HOME_V9_CAMERA_ERROR;
    } else if (status.state == SMART_HOME_CAMERA_STARTING ||
               (status.state == SMART_HOME_CAMERA_RUNNING && !status.preview_sequence)) {
        out->activity = SMART_HOME_V9_CAMERA_STARTING;
    } else if (status.state == SMART_HOME_CAMERA_RUNNING) {
        out->activity = SMART_HOME_V9_CAMERA_PREVIEWING;
    }
    return 0;
#else
    (void)local; (void)out; return -1;
#endif
}

size_t smart_home_v9_collect_views(const smart_home_state_t *local,
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
                                   const smart_home_miloco_t *miloco,
#endif
                                   smart_home_v9_device_view_t *out,
                                   size_t capacity)
{
    size_t count = 0;

    if (!out || capacity == 0) {
        return 0;
    }

    if (local) {
#ifdef CONFIG_SMART_HOME_CAMERA_PREVIEW
        if (count < capacity && !smart_home_v9_board_camera_view(local, &out[count])) count++;
#endif
        for (int i = 0; i < SMART_HOME_MAX_DEVICES; i++) {
            const smart_home_device_t *device = &local->devices[i];
            smart_home_v9_device_view_t *view;

            if (!device->used) {
                continue;
            }
            view = append_view(out, capacity, &count);
            if (!view) {
                break;
            }
            snprintf(view->key, sizeof(view->key), "local:%d", device->id);
            view->source = SMART_HOME_V9_SOURCE_LOCAL;
            view->kind = kind_from_local(device->type);
            copy_text(view->name, sizeof(view->name), device->name);
            copy_text(view->room_id, sizeof(view->room_id), device->room);
            copy_text(view->room_label, sizeof(view->room_label), device->room);
            view->online = true;
            view->state_valid = true;
            view->on = device->on != 0;
            view->activity = view->on ? 1 : 0;
            if (device->type == SMART_HOME_DEVICE_LIGHT) {
                view->primary_value = device->brightness;
                view->unit = "%";
                view->capabilities = SMART_HOME_V9_CAP_POWER |
                                     SMART_HOME_V9_CAP_BRIGHTNESS;
            } else if (device->type == SMART_HOME_DEVICE_AC) {
                view->primary_value = device->temperature;
                view->unit = "°C";
                view->capabilities = SMART_HOME_V9_CAP_POWER |
                                     SMART_HOME_V9_CAP_TEMPERATURE |
                                     SMART_HOME_V9_CAP_FAN_SPEED;
            } else {
                view->capabilities = SMART_HOME_V9_CAP_POWER;
            }
        }
    }

#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
    if (miloco) {
        /* 整表快照约 19 KB，放 PSRAM 而不是 LVGL 线程栈。 */
        smart_home_miloco_device_t *devices = smart_home_bulk_alloc(
            sizeof(*devices) * SMART_HOME_MILOCO_MAX_DEVICES);
        size_t n = 0;

        if (devices != NULL) {
            n = smart_home_miloco_list(miloco, devices,
                                       SMART_HOME_MILOCO_MAX_DEVICES, NULL);
        }

        for (size_t i = 0; i < n; i++) {
            const smart_home_miloco_device_t *device = &devices[i];
            smart_home_v9_device_view_t *view = append_view(out, capacity, &count);

            if (!view) {
                break;
            }
            snprintf(view->key, sizeof(view->key), "milo:%s", device->did);
            view->source = SMART_HOME_V9_SOURCE_MILOCO;
            view->kind = kind_from_miloco(device);
            copy_text(view->name, sizeof(view->name), device->name);
            copy_text(view->room_id, sizeof(view->room_id), device->room);
            copy_text(view->room_label, sizeof(view->room_label), device->room);
            view->online = device->online;
            view->state_valid = device->online;
            view->on = device->power_on;
            view->activity = view->on ? 1 : 0;
            view->capabilities = device->controllable ?
                                 SMART_HOME_V9_CAP_POWER : 0;
            if (view->kind == SMART_HOME_V9_KIND_CAMERA) {
                view->capabilities |= SMART_HOME_V9_CAP_PREVIEW;
            }
            if (view->kind == SMART_HOME_V9_KIND_WASHER ||
                view->kind == SMART_HOME_V9_KIND_ROBOT) {
                view->capabilities |= SMART_HOME_V9_CAP_PAUSE;
            }
        }
        smart_home_bulk_free(devices);
    }
#endif
    return count;
}

size_t smart_home_v9_demo_views(smart_home_v9_device_view_t *out,
                                size_t capacity)
{
    static const struct {
        const char *key;
        smart_home_v9_kind_t kind;
        const char *name;
        const char *room;
        int on;
        int value;
        const char *unit;
        unsigned caps;
    } demo[] = {
        { "demo:light",  SMART_HOME_V9_KIND_LIGHT,  "吊灯",       "客厅", 1, 80, "%",  SMART_HOME_V9_CAP_POWER | SMART_HOME_V9_CAP_BRIGHTNESS },
    };

    size_t count = 0;

    if (!out) {
        return 0;
    }
    for (size_t i = 0; i < sizeof(demo) / sizeof(demo[0]) && count < capacity;
         i++) {
        smart_home_v9_device_view_t *view = &out[count++];

        memset(view, 0, sizeof(*view));
        copy_text(view->key, sizeof(view->key), demo[i].key);
        view->source = SMART_HOME_V9_SOURCE_DEMO;
        view->kind = demo[i].kind;
        copy_text(view->name, sizeof(view->name), demo[i].name);
        copy_text(view->room_id, sizeof(view->room_id), demo[i].room);
        copy_text(view->room_label, sizeof(view->room_label), demo[i].room);
        view->online = true;
        view->state_valid = true;
        view->on = demo[i].on;
        view->primary_value = demo[i].value;
        view->unit = demo[i].unit;
        view->activity = view->on ? 1 : 0;
        view->capabilities = demo[i].caps;
    }
    return count;
}

void smart_home_v9_control_begin(smart_home_v9_control_t *control,
                                 const smart_home_v9_device_view_t *view,
                                 int desired_on,
                                 unsigned request_id,
                                 uint32_t now_ms)
{
    if (!control) {
        return;
    }
    memset(control, 0, sizeof(*control));
    control->state = SMART_HOME_V9_CONTROL_QUEUED;
    control->desired_on = desired_on ? 1 : 0;
    control->request_id = request_id;
    control->started_ms = now_ms;
    if (view) {
        copy_text(control->key, sizeof(control->key), view->key);
    }
}

void smart_home_v9_control_result(smart_home_v9_control_t *control, int result)
{
    if (!control) {
        return;
    }
    if (control->state != SMART_HOME_V9_CONTROL_QUEUED &&
        control->state != SMART_HOME_V9_CONTROL_EXECUTING) {
        return;
    }
    control->state = result == AGENT_OK ?
                     SMART_HOME_V9_CONTROL_WAIT_READBACK :
                     SMART_HOME_V9_CONTROL_FAILED;
}

void smart_home_v9_control_snapshot(smart_home_v9_control_t *control,
                                    const smart_home_v9_device_view_t *view,
                                    uint32_t now_ms,
                                    uint32_t timeout_ms)
{
    if (!control || !view ||
        control->state != SMART_HOME_V9_CONTROL_WAIT_READBACK) {
        return;
    }
    if (strcmp(control->key, view->key) != 0) {
        return;
    }
    if (view->state_valid && view->on == control->desired_on) {
        control->state = SMART_HOME_V9_CONTROL_SUCCEEDED;
    } else if ((uint32_t)(now_ms - control->started_ms) >= timeout_ms) {
        control->state = SMART_HOME_V9_CONTROL_TIMED_OUT;
    }
}

const char *smart_home_v9_control_state_name(smart_home_v9_control_state_t state)
{
    switch (state) {
    case SMART_HOME_V9_CONTROL_IDLE:          return "idle";
    case SMART_HOME_V9_CONTROL_QUEUED:        return "queued";
    case SMART_HOME_V9_CONTROL_EXECUTING:     return "executing";
    case SMART_HOME_V9_CONTROL_WAIT_READBACK: return "wait_readback";
    case SMART_HOME_V9_CONTROL_SUCCEEDED:     return "succeeded";
    case SMART_HOME_V9_CONTROL_FAILED:        return "failed";
    case SMART_HOME_V9_CONTROL_TIMED_OUT:     return "timed_out";
    default:                                  return "unknown";
    }
}
