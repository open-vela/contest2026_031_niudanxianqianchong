/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define SMART_HOME_REMINDER_MAX 8
#define SMART_HOME_REMINDER_MIN_EPOCH INT64_C(1704038400)
#define SMART_HOME_REMINDER_MAX_EPOCH INT64_C(2145888000)
#define SMART_HOME_REMINDER_MAX_DELAY (366 * 86400)

typedef enum {
    SMART_HOME_REMINDER_ACTIVE = 1,
    SMART_HOME_REMINDER_DUE,
    SMART_HOME_REMINDER_MISSED
} smart_home_reminder_state_t;

typedef struct {
    int id;
    int countdown;
    int audible; /* RAM only: restored notifications are silent. */
    smart_home_reminder_state_t state;
    int64_t due_epoch;
    int64_t remaining_seconds;
    char name[32];
    char message[128];
} smart_home_reminder_t;

typedef struct {
    smart_home_reminder_t items[SMART_HOME_REMINDER_MAX];
    int count;
    int ready;
    int clock_ready;
    int last_error;
    int64_t now_epoch;
} smart_home_reminder_snapshot_t;

typedef struct {
    const char *path;
    int64_t (*wall_seconds)(void *context);
    int64_t (*mono_seconds)(void *context);
    void *context;
} smart_home_reminder_options_t;

/* One service per application. Start/stop belong to the main thread. */
int smart_home_reminder_start(const smart_home_reminder_options_t *options);
void smart_home_reminder_stop(void);
/* Blocking durable operations: agent/console workers only. */
int smart_home_reminder_create(int delay_seconds, int64_t at_epoch,
                              const char *name, const char *message, int *id);
int smart_home_reminder_cancel(int id);
/* Cache-only calls, safe on the LVGL thread. Ack is processed asynchronously. */
void smart_home_reminder_snapshot(smart_home_reminder_snapshot_t *out);
int smart_home_reminder_ack(int id);
int smart_home_reminder_parse_local(const char *text, int64_t *epoch);
