/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Build-profile capacities, caller-storage and runtime initialization configuration. Interface draft; declarations do not imply an implemented feature. */
#pragma once

/* Load one build-wide profile before exposing layout-dependent public types. */
#if defined(AGENT_BUILD_CONFIG_HEADER)
#include AGENT_BUILD_CONFIG_HEADER
#endif

/* Direct AGENT_* definitions take precedence over generated CONFIG_AGENT_* values. */
#ifndef AGENT_MAX_TOOLS
#ifdef CONFIG_AGENT_MAX_TOOLS
#define AGENT_MAX_TOOLS CONFIG_AGENT_MAX_TOOLS
#else
#define AGENT_MAX_TOOLS 12u
#endif
#endif

#ifndef AGENT_MAX_CONTEXTS
#ifdef CONFIG_AGENT_MAX_CONTEXTS
#define AGENT_MAX_CONTEXTS CONFIG_AGENT_MAX_CONTEXTS
#else
#define AGENT_MAX_CONTEXTS 8u
#endif
#endif

#ifndef AGENT_MAX_SKILLS
#ifdef CONFIG_AGENT_MAX_SKILLS
#define AGENT_MAX_SKILLS CONFIG_AGENT_MAX_SKILLS
#else
#define AGENT_MAX_SKILLS 8u
#endif
#endif

/* Transitional RAM Storage Provider capacity; not part of the Core layout. */
#ifndef AGENT_MAX_SESSIONS
#ifdef CONFIG_AGENT_MAX_SESSIONS
#define AGENT_MAX_SESSIONS CONFIG_AGENT_MAX_SESSIONS
#else
#define AGENT_MAX_SESSIONS 4u
#endif
#endif

/* Transitional RAM Storage Provider capacity; not part of the Core layout. */
#ifndef AGENT_SESSION_EVENT_CAPACITY
#ifdef CONFIG_AGENT_SESSION_EVENT_CAPACITY
#define AGENT_SESSION_EVENT_CAPACITY CONFIG_AGENT_SESSION_EVENT_CAPACITY
#else
#define AGENT_SESSION_EVENT_CAPACITY 96u
#endif
#endif

/* Transitional RAM Storage Provider capacity; not part of the Core layout. */
#ifndef AGENT_SESSION_PAYLOAD_BYTES
#ifdef CONFIG_AGENT_SESSION_PAYLOAD_BYTES
#define AGENT_SESSION_PAYLOAD_BYTES CONFIG_AGENT_SESSION_PAYLOAD_BYTES
#else
#define AGENT_SESSION_PAYLOAD_BYTES 8192u
#endif
#endif

/* Maximum message descriptors projected into one Model request. */
#ifndef AGENT_MAX_PROJECTED_MESSAGES
#ifdef CONFIG_AGENT_MAX_PROJECTED_MESSAGES
#define AGENT_MAX_PROJECTED_MESSAGES CONFIG_AGENT_MAX_PROJECTED_MESSAGES
#else
#define AGENT_MAX_PROJECTED_MESSAGES 32u
#endif
#endif

#ifndef AGENT_SCRATCH_BYTES
#ifdef CONFIG_AGENT_SCRATCH_BYTES
#define AGENT_SCRATCH_BYTES CONFIG_AGENT_SCRATCH_BYTES
#else
#define AGENT_SCRATCH_BYTES 12288u
#endif
#endif

#ifndef AGENT_MAX_INPUT_BYTES
#ifdef CONFIG_AGENT_MAX_INPUT_BYTES
#define AGENT_MAX_INPUT_BYTES CONFIG_AGENT_MAX_INPUT_BYTES
#else
#define AGENT_MAX_INPUT_BYTES 1024u
#endif
#endif

#ifndef AGENT_MAX_CONTEXT_BYTES
#ifdef CONFIG_AGENT_MAX_CONTEXT_BYTES
#define AGENT_MAX_CONTEXT_BYTES CONFIG_AGENT_MAX_CONTEXT_BYTES
#else
#define AGENT_MAX_CONTEXT_BYTES 4096u
#endif
#endif

#ifndef AGENT_MAX_SCHEMA_BYTES
#ifdef CONFIG_AGENT_MAX_SCHEMA_BYTES
#define AGENT_MAX_SCHEMA_BYTES CONFIG_AGENT_MAX_SCHEMA_BYTES
#else
#define AGENT_MAX_SCHEMA_BYTES 2048u
#endif
#endif

#ifndef AGENT_MAX_ARGUMENTS_BYTES
#ifdef CONFIG_AGENT_MAX_ARGUMENTS_BYTES
#define AGENT_MAX_ARGUMENTS_BYTES CONFIG_AGENT_MAX_ARGUMENTS_BYTES
#else
#define AGENT_MAX_ARGUMENTS_BYTES 1024u
#endif
#endif

#ifndef AGENT_MAX_TOOL_OUTPUT_BYTES
#ifdef CONFIG_AGENT_MAX_TOOL_OUTPUT_BYTES
#define AGENT_MAX_TOOL_OUTPUT_BYTES CONFIG_AGENT_MAX_TOOL_OUTPUT_BYTES
#else
#define AGENT_MAX_TOOL_OUTPUT_BYTES 1024u
#endif
#endif

#ifndef AGENT_MAX_MODEL_OUTPUT_BYTES
#ifdef CONFIG_AGENT_MAX_MODEL_OUTPUT_BYTES
#define AGENT_MAX_MODEL_OUTPUT_BYTES CONFIG_AGENT_MAX_MODEL_OUTPUT_BYTES
#else
#define AGENT_MAX_MODEL_OUTPUT_BYTES 2048u
#endif
#endif

#ifndef AGENT_MAX_MODEL_TOOL_CALLS
#ifdef CONFIG_AGENT_MAX_MODEL_TOOL_CALLS
#define AGENT_MAX_MODEL_TOOL_CALLS CONFIG_AGENT_MAX_MODEL_TOOL_CALLS
#else
#define AGENT_MAX_MODEL_TOOL_CALLS 4u
#endif
#endif

#ifndef AGENT_MAX_NAME_BYTES
#ifdef CONFIG_AGENT_MAX_NAME_BYTES
#define AGENT_MAX_NAME_BYTES CONFIG_AGENT_MAX_NAME_BYTES
#else
#define AGENT_MAX_NAME_BYTES 64u
#endif
#endif

#ifndef AGENT_MAX_DESCRIPTION_BYTES
#ifdef CONFIG_AGENT_MAX_DESCRIPTION_BYTES
#define AGENT_MAX_DESCRIPTION_BYTES CONFIG_AGENT_MAX_DESCRIPTION_BYTES
#else
#define AGENT_MAX_DESCRIPTION_BYTES 256u
#endif
#endif

#ifndef AGENT_MAX_IDENTIFIER_BYTES
#ifdef CONFIG_AGENT_MAX_IDENTIFIER_BYTES
#define AGENT_MAX_IDENTIFIER_BYTES CONFIG_AGENT_MAX_IDENTIFIER_BYTES
#else
#define AGENT_MAX_IDENTIFIER_BYTES 64u
#endif
#endif

#ifndef AGENT_MAX_JSON_DEPTH
#ifdef CONFIG_AGENT_MAX_JSON_DEPTH
#define AGENT_MAX_JSON_DEPTH CONFIG_AGENT_MAX_JSON_DEPTH
#else
#define AGENT_MAX_JSON_DEPTH 16u
#endif
#endif

/* Total Core caller-storage; implementation verifies the profile is sufficient. */
#ifndef AGENT_CORE_WORKSPACE_BYTES
#ifdef CONFIG_AGENT_CORE_WORKSPACE_BYTES
#define AGENT_CORE_WORKSPACE_BYTES CONFIG_AGENT_CORE_WORKSPACE_BYTES
#else
#define AGENT_CORE_WORKSPACE_BYTES 32768u
#endif
#endif

/* Model wrapper caller-storage; provider state remains provider-owned. */
#ifndef AGENT_MODEL_WORKSPACE_BYTES
#ifdef CONFIG_AGENT_MODEL_WORKSPACE_BYTES
#define AGENT_MODEL_WORKSPACE_BYTES CONFIG_AGENT_MODEL_WORKSPACE_BYTES
#else
#define AGENT_MODEL_WORKSPACE_BYTES 128u
#endif
#endif

#include <agent/runtime.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed Core caller-storage; size and alignment follow the active build profile. */
typedef union {
    void* align_pointer;                                   /* Pointer alignment. */
    uint64_t align_u64;                                    /* 64-bit scalar alignment. */
    long double align_long_double;                          /* Conservative scalar alignment. */
    unsigned char bytes[AGENT_CORE_WORKSPACE_BYTES];       /* Private Core storage. */
} agent_workspace_t;

/* Initialization configuration; value copied, referenced objects borrowed. */
typedef struct {
    agent_string_view_t system_prompt; /* Optional immutable instructions. */
    agent_limits_t limits;             /* Default execution limits. */
    agent_runtime_t runtime;           /* Supplied platform services, including clock. */
} agent_config_t;

/* Return a bounded general-purpose configuration. */
agent_config_t agent_config_default(void);

#ifdef __cplusplus
}
#endif
