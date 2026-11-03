#include "smart_home_agent_run_service.h"
#include "smart_home_agent.h"
#include "../smart_home_cpu_debug.h"
#include "../smart_home_memory.h"

#include <nuttx/irq.h>

#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define RUN_ID_SIZE 24
#define RUN_TEXT_SIZE 512

#ifndef CONFIG_SMART_HOME_APP_BRIDGE_RUN_STACKSIZE
#define CONFIG_SMART_HOME_APP_BRIDGE_RUN_STACKSIZE 65536
#endif

struct smart_home_agent_run_service {
    smart_home_agent_app_t *app;
    pthread_t worker;
    void *worker_stack_alloc;
    void *worker_stack;
    pthread_mutex_t mutex;
    sem_t pending;
    int stopping;
    int queued;
    unsigned int next_id;
    char run_id[RUN_ID_SIZE];
    char conversation_id[48];
    char message[RUN_TEXT_SIZE];
    char output[RUN_TEXT_SIZE];
    char status[16];
    smart_home_agent_run_listener_t listener;
    void *listener_user_data;
};

static void notify(smart_home_agent_run_service_t *service, const char *type,
                   const char *data)
{
    smart_home_agent_run_listener_t listener;
    void *user_data;
    char run_id[RUN_ID_SIZE];

    pthread_mutex_lock(&service->mutex);
    listener = service->listener;
    user_data = service->listener_user_data;
    strncpy(run_id, service->run_id, sizeof(run_id) - 1u);
    run_id[sizeof(run_id) - 1u] = '\0';
    pthread_mutex_unlock(&service->mutex);
    if (listener) listener(type, run_id, data, user_data);
}

static void *run_worker(void *argument)
{
    char stack_marker;
    smart_home_agent_run_service_t *service = argument;
    char input[RUN_TEXT_SIZE];
    char output[RUN_TEXT_SIZE];
    int ret;

    smart_home_cpu_debug_log("app-run-worker-start");
    ov_mem_region_log("app-run-worker-stack", &stack_marker);
    while (sem_wait(&service->pending) == 0) {
        pthread_mutex_lock(&service->mutex);
        if (service->stopping) { pthread_mutex_unlock(&service->mutex); break; }
        strncpy(input, service->message, sizeof(input) - 1u);
        input[sizeof(input) - 1u] = '\0';
        strcpy(service->status, "running");
        service->queued = 0;
        pthread_mutex_unlock(&service->mutex);
        notify(service, "agent_run_started", "{\"status\":\"running\"}");
        smart_home_cpu_debug_log("app-run-before-agent");
        ret = smart_home_agent_run(service->app, input, output, sizeof(output));
        pthread_mutex_lock(&service->mutex);
        strncpy(service->output, output, sizeof(service->output) - 1u);
        service->output[sizeof(service->output) - 1u] = '\0';
        strcpy(service->status, ret == AGENT_OK ? "succeeded" : "failed");
        pthread_mutex_unlock(&service->mutex);
        if (ret == AGENT_OK) {
            char data[RUN_TEXT_SIZE + 32];
            snprintf(data, sizeof(data), "{\"content\":\"%s\"}", output);
            notify(service, "agent_message", data);
            notify(service, "agent_done", "{\"status\":\"succeeded\"}");
        } else {
            char data[64];
            snprintf(data, sizeof(data), "{\"status\":\"failed\",\"code\":%d}", ret);
            notify(service, "agent_failed", data);
        }
    }
    return NULL;
}

int smart_home_agent_run_service_start(smart_home_agent_run_service_t **out,
                                       smart_home_agent_app_t *app)
{
    smart_home_agent_run_service_t *service;
    pthread_attr_t attributes;
    int mutex_ready = 0;
    int pending_ready = 0;
    int attributes_ready = 0;
    int ret = AGENT_ERROR;

    if (!out || !app) return AGENT_ERROR_INVALID;
    service = calloc(1u, sizeof(*service));
    if (!service) return AGENT_ERROR_NOMEM;
    ov_mem_region_log("app-run-service", service);
    service->app = app; service->next_id = 1u; strcpy(service->status, "idle");
    if (pthread_mutex_init(&service->mutex, NULL) != 0) {
        goto fail;
    }
    mutex_ready = 1;
    if (sem_init(&service->pending, 0, 0) != 0) {
        goto fail;
    }
    pending_ready = 1;
    service->worker_stack_alloc = smart_home_bulk_alloc(
        CONFIG_SMART_HOME_APP_BRIDGE_RUN_STACKSIZE + STACK_ALIGNMENT - 1u);
    if (!service->worker_stack_alloc) {
        ret = AGENT_ERROR_NOMEM;
        goto fail;
    }
    service->worker_stack = (void *)STACK_ALIGN_UP(
        (uintptr_t)service->worker_stack_alloc);
    ov_mem_region_log("app-run-worker-stack-base", service->worker_stack);
    smart_home_bulk_diag("app-run-worker-stack-reserved");
    if (pthread_attr_init(&attributes) != 0) {
        goto fail;
    }
    attributes_ready = 1;
    if (pthread_attr_setstack(&attributes, service->worker_stack,
                              CONFIG_SMART_HOME_APP_BRIDGE_RUN_STACKSIZE) != 0) {
        ret = AGENT_ERROR_INVALID;
        goto fail;
    }
    if (pthread_create(&service->worker, &attributes, run_worker, service) != 0) {
        goto fail;
    }
    pthread_attr_destroy(&attributes);
    *out = service; return AGENT_OK;

fail:
    if (attributes_ready) pthread_attr_destroy(&attributes);
    smart_home_bulk_free(service->worker_stack_alloc);
    if (pending_ready) sem_destroy(&service->pending);
    if (mutex_ready) pthread_mutex_destroy(&service->mutex);
    free(service);
    return ret;
}

void smart_home_agent_run_service_stop(smart_home_agent_run_service_t **ptr)
{
    smart_home_agent_run_service_t *service;
    if (!ptr || !(service = *ptr)) return;
    pthread_mutex_lock(&service->mutex); service->stopping = 1; pthread_mutex_unlock(&service->mutex);
    sem_post(&service->pending); pthread_join(service->worker, NULL);
    smart_home_bulk_free(service->worker_stack_alloc);
    sem_destroy(&service->pending); pthread_mutex_destroy(&service->mutex); free(service); *ptr = NULL;
}

void smart_home_agent_run_service_set_listener(smart_home_agent_run_service_t *service,
                                               smart_home_agent_run_listener_t listener,
                                               void *user_data)
{
    if (!service) return;
    pthread_mutex_lock(&service->mutex); service->listener = listener;
    service->listener_user_data = user_data; pthread_mutex_unlock(&service->mutex);
}

int smart_home_agent_run_service_submit(smart_home_agent_run_service_t *service,
                                        const char *conversation_id, const char *message,
                                        char *run_id, size_t run_id_size)
{
    if (!service || !message || !message[0] || !run_id || run_id_size < RUN_ID_SIZE) return AGENT_ERROR_INVALID;
    pthread_mutex_lock(&service->mutex);
    if (service->queued || strcmp(service->status, "running") == 0) { pthread_mutex_unlock(&service->mutex); return AGENT_ERROR_BUSY; }
    snprintf(service->run_id, sizeof(service->run_id), "run-%u", service->next_id++);
    strncpy(service->conversation_id, conversation_id ? conversation_id : "default", sizeof(service->conversation_id) - 1u);
    strncpy(service->message, message, sizeof(service->message) - 1u);
    service->output[0] = '\0'; strcpy(service->status, "queued"); service->queued = 1;
    strcpy(run_id, service->run_id); pthread_mutex_unlock(&service->mutex); sem_post(&service->pending); return AGENT_OK;
}

int smart_home_agent_run_service_get(smart_home_agent_run_service_t *service,
                                     const char *run_id, char *buffer, size_t buffer_size)
{
    int n;
    if (!service || !run_id || !buffer || buffer_size == 0u) return AGENT_ERROR_INVALID;
    pthread_mutex_lock(&service->mutex);
    if (strcmp(run_id, service->run_id) != 0) { pthread_mutex_unlock(&service->mutex); return AGENT_ERROR_NOTFOUND; }
    n = snprintf(buffer, buffer_size, "{\"runId\":\"%s\",\"status\":\"%s\",\"output\":\"%s\"}", service->run_id, service->status, service->output);
    pthread_mutex_unlock(&service->mutex);
    return n < 0 || (size_t)n >= buffer_size ? AGENT_ERROR_LIMIT : AGENT_OK;
}
