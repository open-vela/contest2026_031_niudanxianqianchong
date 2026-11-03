/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "smart_home_miloco.h"

/* These helpers perform no network I/O. Caller serializes snapshot updates. */
int smart_home_miloco_power_index(const smart_home_miloco_device_t *device);
bool smart_home_miloco_value_allowed(const smart_home_miloco_control_t *control,
                                      int32_t value);
int smart_home_miloco_status_apply(smart_home_miloco_device_t *device,
                                    const char *body, bool *changed);
int smart_home_miloco_control_reply(const char *body, const char *did,
                                     const char *iid, bool action);
