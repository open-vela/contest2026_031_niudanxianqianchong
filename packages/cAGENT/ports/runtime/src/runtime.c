/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* OpenVela/NuttX monotonic clock adapter. */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <agent_openvela_runtime.h>

#include <stdint.h>
#include <time.h>

static uint64_t agent_port_openvela_now_ms(void* context)
{
    struct timespec time_value;

    (void)context;
    if (clock_gettime(CLOCK_MONOTONIC, &time_value) != 0)
    {
        return UINT64_MAX; /* Fail closed for absolute deadlines. */
    }
    return (uint64_t)time_value.tv_sec * UINT64_C(1000) +
           (uint64_t)time_value.tv_nsec / UINT64_C(1000000);
}

agent_error_t agent_port_openvela_runtime_init(agent_runtime_t* out)
{
    struct timespec time_value;

    if (out == NULL)
    {
        return AGENT_ERROR_INVALID;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &time_value) != 0 || time_value.tv_sec < 0 ||
        time_value.tv_nsec < 0 || time_value.tv_nsec >= 1000000000L)
    {
        return AGENT_ERROR_IO;
    }
    *out = (agent_runtime_t){0};
    out->now_ms = agent_port_openvela_now_ms;
    return AGENT_OK;
}
