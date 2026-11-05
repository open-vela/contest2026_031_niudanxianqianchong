/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <agent.h>
#include <stddef.h>
#include <nuttx/config.h>

#ifndef CONFIG_SMART_HOME_USER_MEMORY_MAX_ENTRIES
#define CONFIG_SMART_HOME_USER_MEMORY_MAX_ENTRIES 16
#endif
#ifndef CONFIG_SMART_HOME_USER_MEMORY_CONTEXT_BYTES
#define CONFIG_SMART_HOME_USER_MEMORY_CONTEXT_BYTES 768
#endif
#define SMART_HOME_USER_MEMORY_KEY_BYTES 48u
#define SMART_HOME_USER_MEMORY_VALUE_BYTES 128u
#define SMART_HOME_USER_MEMORY_EVIDENCE_BYTES 256u

typedef struct smart_home_user_memory smart_home_user_memory_t;
/* All operations run on the serialized Agent task, never the LVGL thread.
 * The store is application-owned; tool callbacks do not reenter Core Memory. */
int smart_home_user_memory_open(smart_home_user_memory_t **out, const char *path);
void smart_home_user_memory_close(smart_home_user_memory_t **memory);
/* Borrow the live user input only for this turn; end clears the pointer. */
void smart_home_user_memory_begin(smart_home_user_memory_t *memory, const char *input);
void smart_home_user_memory_end(smart_home_user_memory_t *memory);
int smart_home_user_memory_remember(smart_home_user_memory_t *memory,
    const char *key, const char *value, const char *evidence, int *changed);
int smart_home_user_memory_forget(smart_home_user_memory_t *memory,
    const char *key, int *changed);
/* Caller frees the bounded JSON. Entries are newest first; next_offset is null at end. */
int smart_home_user_memory_list(smart_home_user_memory_t *memory, size_t offset,
    size_t limit, size_t max_bytes, char **json);
int smart_home_user_memory_register_context(agent_t *agent, smart_home_user_memory_t *memory);
