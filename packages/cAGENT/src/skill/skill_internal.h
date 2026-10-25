/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Private ordered skill registry and bounded projection. */
#pragma once

#include "core/arena_internal.h"
#include <agent/skill.h>
#include <agent/config.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct agent_skill_registry {
    size_t count;
    agent_skill_t entries[AGENT_MAX_SKILLS ? AGENT_MAX_SKILLS : 1u];
} agent_skill_registry_t;

#define AGENT_SKILL_REGISTRY_BYTES (AGENT_MAX_SKILLS ? sizeof(agent_skill_registry_t) : 0u)

agent_error_t agent_skill_registry_init(agent_skill_registry_t** registry, agent_arena_t* arena);
/* Sink failure may leave partial output; required overflow is checked before writing. */
agent_error_t agent_skill_project(const agent_skill_registry_t* registry,
                                       size_t max_bytes, const agent_text_sink_t* output);

#ifdef __cplusplus
}
#endif
