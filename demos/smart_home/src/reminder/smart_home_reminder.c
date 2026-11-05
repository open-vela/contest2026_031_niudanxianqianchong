/* SPDX-License-Identifier: Apache-2.0 */
#include "smart_home_reminder.h"
#include "../config/smart_home_config_store.h"
#include "../smart_home_memory.h"
#include <agent/error.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __NuttX__
#include <nuttx/irq.h>
#endif

#define REMINDER_GRACE_SECONDS 300
#define REMINDER_STACK_SIZE 16384

typedef struct {
    smart_home_reminder_t item;
    int64_t mono_due;
    int restored;
} reminder_record_t;

typedef struct {
    reminder_record_t records[SMART_HOME_REMINDER_MAX];
    int next_id;
    char path[SMART_HOME_CONFIG_PATH_MAX];
    smart_home_reminder_options_t clock;
} reminder_engine_t;

typedef struct {
    int kind; /* 1=create, 2=cancel */
    int delay;
    int64_t at;
    int id;
    char name[32];
    char message[128];
    int result;
    int pending;
    int busy;
} reminder_command_t;

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static pthread_t g_worker;
static reminder_engine_t *g_engine;
static void *g_stack_alloc;
static smart_home_reminder_snapshot_t g_cache;
static reminder_command_t g_command;
static int g_ack[SMART_HOME_REMINDER_MAX];
static int g_started;
static int g_stopping;

static int valid_clock(int64_t epoch)
{
    return epoch >= SMART_HOME_REMINDER_MIN_EPOCH &&
           epoch < SMART_HOME_REMINDER_MAX_EPOCH;
}

static int64_t wall_seconds(void *arg)
{
    (void)arg;
    return (int64_t)time(NULL);
}

static int64_t mono_seconds(void *arg)
{
    struct timespec ts;
    (void)arg;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
    return (int64_t)ts.tv_sec;
}

static int leap(int year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

int smart_home_reminder_parse_local(const char *text, int64_t *epoch)
{
    static const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int y, m, d, h, n, s, i;
    int64_t total = 0;
    if (!text || !epoch || strlen(text) != 19 || text[4] != '-' ||
        text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':')
        return AGENT_ERROR_INVALID;
    for (i = 0; i < 19; ++i) {
        if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16) continue;
        if (text[i] < '0' || text[i] > '9') return AGENT_ERROR_INVALID;
    }
    if (sscanf(text, "%4d-%2d-%2dT%2d:%2d:%2d", &y,&m,&d,&h,&n,&s) != 6 ||
        y < 2024 || y > 2037 || m < 1 || m > 12 || d < 1 ||
        d > days[m-1] + (m == 2 && leap(y)) || h > 23 || n > 59 || s > 59)
        return AGENT_ERROR_INVALID;
    for (i = 1970; i < y; ++i) total += 365 + leap(i);
    for (i = 1; i < m; ++i) total += days[i-1] + (i == 2 && leap(y));
    *epoch = (total + d - 1) * 86400 + h * 3600 + n * 60 + s - 8 * 3600;
    return valid_clock(*epoch) ? AGENT_OK : AGENT_ERROR_INVALID;
}

static int number(const cJSON *root, const char *key, int64_t low,
                  int64_t high, int64_t *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(v) || !(v->valuedouble >= (double)low &&
        v->valuedouble <= (double)high)) return AGENT_ERROR_PARSE;
    *out = (int64_t)v->valuedouble;
    return (double)*out == v->valuedouble ? AGENT_OK : AGENT_ERROR_PARSE;
}

static int load_records(const cJSON *root, void *context)
{
    reminder_engine_t *e = context;
    const cJSON *array = cJSON_GetObjectItemCaseSensitive(root, "tasks");
    const cJSON *obj;
    int64_t n;
    int count, i, j;
    if (number(root,"version",1,1,&n) != AGENT_OK ||
        number(root,"next_id",1,INT_MAX,&n) != AGENT_OK ||
        !cJSON_IsArray(array)) return AGENT_ERROR_PARSE;
    e->next_id = (int)n;
    count = cJSON_GetArraySize(array);
    if (count > SMART_HOME_REMINDER_MAX) return AGENT_ERROR_PARSE;
    for (i = 0; i < count; ++i) {
        reminder_record_t *r = &e->records[i];
        smart_home_reminder_t *item = &r->item;
        const cJSON *name, *message, *countdown;
        obj = cJSON_GetArrayItem(array,i);
        if (!cJSON_IsObject(obj) || number(obj,"id",1,INT_MAX-1,&n) != AGENT_OK)
            return AGENT_ERROR_PARSE;
        item->id = (int)n;
        if (item->id >= e->next_id) return AGENT_ERROR_PARSE;
        for (j = 0; j < i; ++j)
            if (e->records[j].item.id == item->id) return AGENT_ERROR_PARSE;
        if (number(obj,"state",SMART_HOME_REMINDER_ACTIVE,
                   SMART_HOME_REMINDER_MISSED,&n) != AGENT_OK) return AGENT_ERROR_PARSE;
        item->state = (smart_home_reminder_state_t)n;
        if (number(obj,"due_epoch",SMART_HOME_REMINDER_MIN_EPOCH,
                   SMART_HOME_REMINDER_MAX_EPOCH-1,&item->due_epoch) != AGENT_OK)
            return AGENT_ERROR_PARSE;
        countdown = cJSON_GetObjectItemCaseSensitive(obj,"countdown");
        name = cJSON_GetObjectItemCaseSensitive(obj,"name");
        message = cJSON_GetObjectItemCaseSensitive(obj,"message");
        if (!cJSON_IsBool(countdown) || !cJSON_IsString(name) ||
            !cJSON_IsString(message) || !message->valuestring[0] ||
            strlen(name->valuestring) >= sizeof(item->name) ||
            strlen(message->valuestring) >= sizeof(item->message))
            return AGENT_ERROR_PARSE;
        item->countdown = cJSON_IsTrue(countdown);
        strcpy(item->name,name->valuestring);
        strcpy(item->message,message->valuestring);
        r->restored = 1;
        r->mono_due = -1;
    }
    return AGENT_OK;
}

static int save_records(const reminder_engine_t *e)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *array = cJSON_CreateArray();
    int i, ret = AGENT_ERROR_NOMEM;
    if (!root || !array) goto done;
    if (!cJSON_AddItemToObject(root,"tasks",array)) goto done;
    array = NULL;
    if (!cJSON_AddNumberToObject(root,"version",1) ||
        !cJSON_AddNumberToObject(root,"next_id",e->next_id)) goto done;
    for (i = 0; i < SMART_HOME_REMINDER_MAX; ++i) {
        const smart_home_reminder_t *r = &e->records[i].item;
        cJSON *o;
        if (!r->id) continue;
        o = cJSON_CreateObject();
        if (!o) goto done;
        if (!cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(root,"tasks"),o)) {
            cJSON_Delete(o);
            goto done;
        }
        if (!cJSON_AddNumberToObject(o,"id",r->id) ||
            !cJSON_AddNumberToObject(o,"state",r->state) ||
            !cJSON_AddNumberToObject(o,"due_epoch",(double)r->due_epoch) ||
            !cJSON_AddBoolToObject(o,"countdown",r->countdown) ||
            !cJSON_AddStringToObject(o,"name",r->name) ||
            !cJSON_AddStringToObject(o,"message",r->message)) goto done;
    }
    ret = smart_home_config_store_save(e->path,root);
 done:
    cJSON_Delete(array);
    cJSON_Delete(root);
    return ret;
}

static int engine_open(reminder_engine_t *e)
{
    cJSON *root = NULL;
    struct stat st;
    int ret;
    e->next_id = 1;
    if (stat(e->path,&st) != 0 && errno == ENOENT) return AGENT_OK;
    ret = smart_home_config_store_load(e->path,NULL,SMART_HOME_CONFIG_REJECT,
                                     load_records,e,&root,NULL,0);
    cJSON_Delete(root);
    return ret;
}

static int engine_remove(reminder_engine_t *e, int id)
{
    int i, ret;
    reminder_record_t previous;
    for (i = 0; i < SMART_HOME_REMINDER_MAX; ++i) {
        if (e->records[i].item.id != id) continue;
        previous = e->records[i];
        memset(&e->records[i],0,sizeof(e->records[i]));
        ret = save_records(e);
        if (ret != AGENT_OK) e->records[i] = previous;
        return ret;
    }
    return AGENT_ERROR_NOT_FOUND;
}

static int engine_create(reminder_engine_t *e, reminder_command_t *cmd,
                         int64_t wall, int64_t mono)
{
    reminder_record_t *r;
    int i, ret;
    if (!valid_clock(wall) || mono < 0) return AGENT_ERROR_STATE;
    if (!cmd->delay && cmd->at <= wall) return AGENT_ERROR_INVALID;
    if (cmd->delay && wall + cmd->delay >= SMART_HOME_REMINDER_MAX_EPOCH)
        return AGENT_ERROR_INVALID;
    if (e->next_id == INT_MAX) return AGENT_ERROR_CAPACITY;
    for (i = 0; i < SMART_HOME_REMINDER_MAX && e->records[i].item.id; ++i) {}
    if (i == SMART_HOME_REMINDER_MAX) return AGENT_ERROR_CAPACITY;
    r = &e->records[i];
    memset(r,0,sizeof(*r));
    r->item.id = e->next_id++;
    r->item.countdown = cmd->delay > 0;
    r->item.state = SMART_HOME_REMINDER_ACTIVE;
    r->item.due_epoch = cmd->delay ? wall + cmd->delay : cmd->at;
    r->mono_due = cmd->delay ? mono + cmd->delay : -1;
    strcpy(r->item.name,cmd->name);
    strcpy(r->item.message,cmd->message);
    ret = save_records(e);
    if (ret != AGENT_OK) {
        --e->next_id;
        memset(r,0,sizeof(*r));
    } else {
        cmd->id = r->item.id;
        printf("[reminder] created id=%d due=%lld kind=%s\n",cmd->id,
               (long long)r->item.due_epoch,r->item.countdown ? "countdown" : "alarm");
    }
    return ret;
}

static int engine_tick(reminder_engine_t *e, int64_t wall, int64_t mono)
{
    int i, ret = AGENT_OK;
    for (i = 0; i < SMART_HOME_REMINDER_MAX; ++i) {
        reminder_record_t *r = &e->records[i];
        reminder_record_t previous;
        int64_t remaining;
        if (!r->item.id || r->item.state != SMART_HOME_REMINDER_ACTIVE) continue;
        /* Re-anchor the persisted wall deadline without changing the live
         * monotonic countdown. Required after NTP moves CLOCK_REALTIME. */
        if (r->item.countdown && r->mono_due >= 0 && mono >= 0) {
            remaining = r->mono_due - mono;
            if (valid_clock(wall) && remaining > 0 &&
                llabs(r->item.due_epoch - wall - remaining) > 2 &&
                wall + remaining < SMART_HOME_REMINDER_MAX_EPOCH) {
                previous = *r;
                r->item.due_epoch = wall + remaining;
                ret = save_records(e);
                if (ret != AGENT_OK) *r = previous;
            }
        } else {
            if (!valid_clock(wall)) continue;
            remaining = r->item.due_epoch - wall;
            if (r->item.countdown && remaining > 0 && mono >= 0)
                r->mono_due = mono + remaining;
        }
        if (remaining > 0) { r->restored = 0; continue; }
        previous = *r;
        r->item.state = r->restored && -remaining > REMINDER_GRACE_SECONDS ?
                       SMART_HOME_REMINDER_MISSED : SMART_HOME_REMINDER_DUE;
        r->item.audible = r->item.state == SMART_HOME_REMINDER_DUE;
        ret = save_records(e);
        if (ret != AGENT_OK) *r = previous;
        else printf("[reminder] due id=%d state=%d\n",r->item.id,r->item.state);
    }
    return ret;
}

static void publish(reminder_engine_t *e, int ready, int error,
                    int64_t wall, int64_t mono)
{
    int i, previous_error;
    pthread_mutex_lock(&g_mutex);
    previous_error = g_cache.last_error;
    memset(&g_cache,0,sizeof(g_cache));
    g_cache.ready = ready;
    g_cache.clock_ready = valid_clock(wall);
    g_cache.now_epoch = wall;
    g_cache.last_error = error;
    if (ready) for (i = 0; i < SMART_HOME_REMINDER_MAX; ++i) {
        reminder_record_t *r = &e->records[i];
        smart_home_reminder_t *copy;
        if (!r->item.id) continue;
        copy = &g_cache.items[g_cache.count++];
        *copy = r->item;
        copy->remaining_seconds = r->item.countdown && r->mono_due >= 0 ?
                                 r->mono_due - mono : r->item.due_epoch - wall;
        if (copy->remaining_seconds < 0) copy->remaining_seconds = 0;
    }
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_mutex);
    if (error != AGENT_OK && error != previous_error)
        fprintf(stderr,"[reminder] operation failed code=%d\n",error);
}

static void *reminder_worker(void *arg)
{
    reminder_engine_t *e = arg;
    reminder_command_t cmd;
    int ready = engine_open(e) == AGENT_OK;
    int error = ready ? AGENT_OK : AGENT_ERROR_PARSE;
    printf("[reminder] start ready=%d path=%s\n",ready,e->path);
    for (;;) {
        int i, ack[SMART_HOME_REMINDER_MAX];
        int64_t wall = e->clock.wall_seconds(e->clock.context);
        int64_t mono = e->clock.mono_seconds(e->clock.context);
        pthread_mutex_lock(&g_mutex);
        if (g_stopping) { pthread_mutex_unlock(&g_mutex); break; }
        cmd = g_command;
        memcpy(ack,g_ack,sizeof(ack));
        memset(g_ack,0,sizeof(g_ack));
        pthread_mutex_unlock(&g_mutex);
        if (ready) {
            for (i = 0; i < SMART_HOME_REMINDER_MAX; ++i) if (ack[i]) {
                int ret = engine_remove(e,ack[i]);
                if (ret != AGENT_OK && ret != AGENT_ERROR_NOT_FOUND) error = ret;
            }
        }
        if (cmd.pending) {
            cmd.result = !ready ? AGENT_ERROR_STATE : cmd.kind == 1 ?
                         engine_create(e,&cmd,wall,mono) : engine_remove(e,cmd.id);
            error = cmd.result;
        }
        if (ready) {
            int tick_result = engine_tick(e,wall,mono);
            if (tick_result != AGENT_OK) error = tick_result;
        }
        publish(e,ready,error,wall,mono);
        pthread_mutex_lock(&g_mutex);
        if (cmd.pending) {
            g_command.result = cmd.result;
            g_command.id = cmd.id;
            g_command.pending = 0;
            pthread_cond_broadcast(&g_cond);
        }
        pthread_mutex_unlock(&g_mutex);
        usleep(200000);
    }
    return NULL;
}

int smart_home_reminder_start(const smart_home_reminder_options_t *options)
{
    pthread_attr_t attr;
    reminder_engine_t *e;
    int ret;
    pthread_mutex_lock(&g_mutex);
    if (g_started) { pthread_mutex_unlock(&g_mutex); return AGENT_ERROR_BUSY; }
    e = calloc(1,sizeof(*e));
    if (!e) { pthread_mutex_unlock(&g_mutex); return AGENT_ERROR_NOMEM; }
    if (options) e->clock = *options;
    if (!e->clock.wall_seconds) e->clock.wall_seconds = wall_seconds;
    if (!e->clock.mono_seconds) e->clock.mono_seconds = mono_seconds;
    if (snprintf(e->path,sizeof(e->path),"%s", options && options->path ?
                 options->path : SMART_HOME_CONFIG_DIR "/reminders.json") >= (int)sizeof(e->path)) {
        free(e); pthread_mutex_unlock(&g_mutex); return AGENT_ERROR_CAPACITY;
    }
    g_stopping = 0;
    memset(&g_cache,0,sizeof(g_cache));
    memset(&g_command,0,sizeof(g_command));
    memset(g_ack,0,sizeof(g_ack));
    ret = pthread_attr_init(&attr);
    if (ret != 0) { free(e); pthread_mutex_unlock(&g_mutex); return AGENT_ERROR; }
#ifdef __NuttX__
    g_stack_alloc = smart_home_bulk_alloc(REMINDER_STACK_SIZE + STACK_ALIGNMENT - 1u);
    if (!g_stack_alloc) ret = ENOMEM;
    else ret = pthread_attr_setstack(&attr,(void *)STACK_ALIGN_UP((uintptr_t)g_stack_alloc),
                                    REMINDER_STACK_SIZE);
    if (!ret) ret = pthread_attr_setschedparam(&attr,
                         &(struct sched_param){.sched_priority = 90});
#endif
    if (!ret) ret = pthread_create(&g_worker,&attr,reminder_worker,e);
    pthread_attr_destroy(&attr);
    if (ret) {
        smart_home_bulk_free(g_stack_alloc); g_stack_alloc = NULL; free(e);
    } else { g_started = 1; g_engine = e; }
    pthread_mutex_unlock(&g_mutex);
    return ret ? AGENT_ERROR : AGENT_OK;
}

void smart_home_reminder_stop(void)
{
    pthread_mutex_lock(&g_mutex);
    if (!g_started) { pthread_mutex_unlock(&g_mutex); return; }
    g_stopping = 1;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_mutex);
    pthread_join(g_worker,NULL);
    pthread_mutex_lock(&g_mutex);
    free(g_engine); g_engine = NULL;
    smart_home_bulk_free(g_stack_alloc); g_stack_alloc = NULL;
    g_started = 0;
    memset(&g_cache,0,sizeof(g_cache));
    pthread_mutex_unlock(&g_mutex);
}

static int submit(reminder_command_t *cmd)
{
    int result;
    pthread_mutex_lock(&g_mutex);
    if (!g_started || g_stopping) result = AGENT_ERROR_STATE;
    else if (g_command.busy) result = AGENT_ERROR_BUSY;
    else {
        g_command = *cmd;
        g_command.pending = g_command.busy = 1;
        while (g_command.pending && !g_stopping)
            pthread_cond_wait(&g_cond,&g_mutex);
        result = g_stopping ? AGENT_ERROR_CANCELLED : g_command.result;
        cmd->id = g_command.id;
        g_command.busy = 0;
    }
    pthread_mutex_unlock(&g_mutex);
    return result;
}

int smart_home_reminder_create(int delay, int64_t at, const char *name,
                              const char *message, int *id)
{
    reminder_command_t cmd = {0};
    if (!message || !message[0] || !id ||
        strlen(message) >= sizeof(cmd.message) ||
        (name && strlen(name) >= sizeof(cmd.name)) ||
        delay < 0 || delay > SMART_HOME_REMINDER_MAX_DELAY ||
        (delay > 0 ? at != 0 : !valid_clock(at))) return AGENT_ERROR_INVALID;
    cmd.kind = 1; cmd.delay = delay; cmd.at = at;
    strcpy(cmd.message,message);
    strcpy(cmd.name,name && name[0] ? name : "提醒");
    {
        int ret = submit(&cmd);
        if (ret == AGENT_OK) *id = cmd.id;
        return ret;
    }
}

int smart_home_reminder_cancel(int id)
{
    reminder_command_t cmd = {0};
    if (id <= 0) return AGENT_ERROR_INVALID;
    cmd.kind = 2; cmd.id = id;
    return submit(&cmd);
}

void smart_home_reminder_snapshot(smart_home_reminder_snapshot_t *out)
{
    if (!out) return;
    pthread_mutex_lock(&g_mutex);
    *out = g_cache;
    pthread_mutex_unlock(&g_mutex);
}

int smart_home_reminder_ack(int id)
{
    int i, result = AGENT_ERROR_CAPACITY;
    pthread_mutex_lock(&g_mutex);
    if (!g_started || g_stopping) result = AGENT_ERROR_STATE;
    else {
        int found = 0;
        for (i = 0; i < g_cache.count; ++i)
            if (g_cache.items[i].id == id &&
                g_cache.items[i].state != SMART_HOME_REMINDER_ACTIVE) found = 1;
        if (!found) { pthread_mutex_unlock(&g_mutex); return AGENT_ERROR_NOT_FOUND; }
        for (i = 0; i < SMART_HOME_REMINDER_MAX; ++i) {
            if (g_ack[i] == id) { result = AGENT_OK; break; }
            if (!g_ack[i]) { g_ack[i] = id; result = AGENT_OK; break; }
        }
    }
    pthread_mutex_unlock(&g_mutex);
    return result;
}
