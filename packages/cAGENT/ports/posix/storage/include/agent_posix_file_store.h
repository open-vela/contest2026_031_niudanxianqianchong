/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Flat POSIX file store over an existing application-owned, trusted directory. */
#pragma once

#include <agent_file_store.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char* directory; /* Borrowed absolute root; no concurrent untrusted changes. */
    char* path_buffer;     /* Borrowed scratch, disjoint from state, config and input/output. */
    size_t path_capacity;
    bool read_only;       /* Bind only size/read/visit capabilities. */
    bool sync_directory;  /* Request directory fsync, attempted at init; not a durability proof. */
} agent_posix_file_store_config_t;

typedef struct {
    const char* directory;
    size_t directory_length;
    char* path;
    size_t path_capacity;
    uint32_t temporary_sequence;
    bool sync_directory;
} agent_posix_file_store_t;

/* No persistent descriptors; state/root/scratch remain alive while bound; config is copied. */
agent_error_t agent_posix_file_store_init(agent_posix_file_store_t* state,
                                         const agent_posix_file_store_config_t* config,
                                         agent_file_store_t* store);

#ifdef __cplusplus
}
#endif
