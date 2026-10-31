/* SPDX-License-Identifier: Apache-2.0 */
#include "smart_home_camera_control.h"
#include "smart_home_camera_service.h"
#include <pthread.h>
#include <errno.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t settled = PTHREAD_COND_INITIALIZER;
static smart_home_camera_control_status_t control;
static unsigned generation;

static void *run(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&lock);
        bool on = control.desired_on;
        unsigned current = generation;
        pthread_mutex_unlock(&lock);
        struct smart_home_camera_status_s status;
        int result = smart_home_camera_get_status(&status);
        if (!result) {
            if (!on)
                result = smart_home_camera_stop();
            else if (status.state != SMART_HOME_CAMERA_RUNNING &&
                     status.state != SMART_HOME_CAMERA_STARTING) {
                /* Reap an exited/error capture thread before retrying. */
                result = smart_home_camera_stop();
                if (!result)
                    result = smart_home_camera_start();
            }
        }
        pthread_mutex_lock(&lock);
        if (current != generation) {
            pthread_mutex_unlock(&lock);
            continue;
        }
        control.result = result;
        control.busy = false;
        pthread_cond_broadcast(&settled);
        pthread_mutex_unlock(&lock);
        return NULL;
    }
}
int smart_home_camera_request(bool on) {
    pthread_t thread;
    pthread_attr_t attr;
    pthread_mutex_lock(&lock);
    control.desired_on = on;
    generation++;
    if (control.busy) {
        pthread_mutex_unlock(&lock);
        return 0;
    }
    control.busy = true;
    control.result = 0;
    int ret = pthread_attr_init(&attr);
    if (!ret) {
        ret = pthread_attr_setstacksize(&attr, 16384);
        if (!ret)
            ret = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (!ret)
            ret = pthread_create(&thread, &attr, run, NULL);
        pthread_attr_destroy(&attr);
    }
    if (ret) {
        control.busy = false;
        control.result = -ret;
        pthread_cond_broadcast(&settled);
    }
    pthread_mutex_unlock(&lock);
    return ret ? -ret : 0;
}
void smart_home_camera_control_status(smart_home_camera_control_status_t *out) {
    if (!out)
        return;
    pthread_mutex_lock(&lock);
    *out = control;
    pthread_mutex_unlock(&lock);
}
int smart_home_camera_control_shutdown(void) {
    int ret = smart_home_camera_request(false);
    if (ret)
        return smart_home_camera_stop();
    pthread_mutex_lock(&lock);
    while (control.busy)
        pthread_cond_wait(&settled, &lock);
    ret = control.result;
    pthread_mutex_unlock(&lock);
    return ret;
}
