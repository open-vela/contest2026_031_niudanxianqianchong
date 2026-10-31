/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
typedef struct {
    bool desired_on;
    bool busy;
    int result;
} smart_home_camera_control_status_t;
/* Nonblocking, coalesced commands. No caller/UI pointer is retained. */
int smart_home_camera_request(bool on);
void smart_home_camera_control_status(smart_home_camera_control_status_t *out);
/* Call outside the display lock when the application exits. */
int smart_home_camera_control_shutdown(void);
