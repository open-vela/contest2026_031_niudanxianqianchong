/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* POSIX byte I/O; caller-owned scratch and a trusted, stable root. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <agent_posix_file_store.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int path_stat(const char* path, struct stat* info)
{
#if defined(AGENT_POSIX_FILE_STORE_NO_SYMLINKS) && AGENT_POSIX_FILE_STORE_NO_SYMLINKS
    /* Explicit profile for VFS mounts without symlinks or an lstat syscall. */
    return stat(path, info);
#else
    return lstat(path, info);
#endif
}

static agent_error_t io_error(void)
{
    if (errno == ENOENT) return AGENT_ERROR_NOT_FOUND;
    if (errno == EEXIST) return AGENT_ERROR_EXISTS;
    if (errno == ENOSYS) return AGENT_ERROR_NOT_SUPPORTED;
#ifdef ENOTSUP
    if (errno == ENOTSUP) return AGENT_ERROR_NOT_SUPPORTED;
#endif
    return AGENT_ERROR_IO;
}

static bool overlaps(const void* a, size_t a_bytes, const void* b, size_t b_bytes)
{
    uintptr_t left = (uintptr_t)a;
    uintptr_t right = (uintptr_t)b;

    if (!a_bytes || !b_bytes) return false;
    if (a_bytes > UINTPTR_MAX - left || b_bytes > UINTPTR_MAX - right) return true;
    return left < right + b_bytes && right < left + a_bytes;
}

static size_t base_bytes(const agent_posix_file_store_t* state)
{
    return state->directory_length +
           (state->directory[state->directory_length - 1u] == '/' ? 0u : 1u);
}

static bool valid_input(const agent_posix_file_store_t* state,
                        const void* data, size_t bytes)
{
    return (!bytes || data) &&
           !overlaps(data, bytes, state->path, state->path_capacity) &&
           !overlaps(data, bytes, state, sizeof(*state)) &&
           !overlaps(data, bytes, state->directory, state->directory_length + 1u);
}

static agent_error_t make_path(agent_posix_file_store_t* state,
                               agent_string_view_t name)
{
    size_t base = base_bytes(state);

    if (!valid_input(state, name.data, name.size)) return AGENT_ERROR_INVALID;
    if (base >= state->path_capacity || name.size >= state->path_capacity - base)
        return AGENT_ERROR_CAPACITY;
    memcpy(state->path, state->directory, state->directory_length);
    if (base > state->directory_length) state->path[state->directory_length] = '/';
    memcpy(state->path + base, name.data, name.size);
    state->path[base + name.size] = '\0';
    return AGENT_OK;
}

static agent_error_t regular_file(const char* path, bool allow_missing)
{
    struct stat info;

    if (path_stat(path, &info) != 0) {
        if (allow_missing && errno == ENOENT) return AGENT_OK;
        return io_error();
    }
    return S_ISREG(info.st_mode) ? AGENT_OK : AGENT_ERROR_IO;
}

static int open_file(const char* path, int flags)
{
    int fd;
#if defined(O_NOFOLLOW) && (!defined(AGENT_POSIX_FILE_STORE_NO_SYMLINKS) || !AGENT_POSIX_FILE_STORE_NO_SYMLINKS)
    flags |= O_NOFOLLOW;
#endif
    do {
        fd = open(path, flags, 0600);
    } while (fd < 0 && errno == EINTR);
    return fd;
}

static agent_error_t close_file(int fd, agent_error_t status)
{
    /* Never retry close: some systems already release the descriptor on EINTR. */
    if (close(fd) != 0 && status == AGENT_OK) return AGENT_ERROR_IO;
    return status;
}

static agent_error_t sync_fd(int fd)
{
    while (fsync(fd) != 0) {
        if (errno != EINTR) return io_error();
    }
    return AGENT_OK;
}

static agent_error_t sync_root(agent_posix_file_store_t* state)
{
    int fd;
    int flags = O_RDONLY;
    agent_error_t status;

    if (!state->sync_directory) return AGENT_OK;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    do {
        fd = open(state->directory, flags);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) return io_error();
    status = sync_fd(fd);
    return close_file(fd, status);
}

static agent_error_t posix_size(void* context, agent_string_view_t name, uint64_t* bytes)
{
    agent_posix_file_store_t* state = context;
    struct stat info;
    agent_error_t status = make_path(state, name);

    if (status != AGENT_OK) return status;
    if (path_stat(state->path, &info) != 0) return io_error();
    if (!S_ISREG(info.st_mode) || info.st_size < 0) return AGENT_ERROR_IO;
    *bytes = (uint64_t)info.st_size;
    return AGENT_OK;
}

static agent_error_t posix_read(void* context, agent_string_view_t name, uint64_t offset,
                               void* output, size_t capacity, size_t* bytes_read)
{
    agent_posix_file_store_t* state = context;
    off_t position = (off_t)offset;
    struct stat info;
    size_t done = 0u;
    int fd;
    agent_error_t status;

    if (position < 0 || (uint64_t)position != offset ||
        !valid_input(state, output, capacity)) return AGENT_ERROR_INVALID;
    status = make_path(state, name);
    if (status != AGENT_OK) return status;
    status = regular_file(state->path, false);
    if (status != AGENT_OK) return status;
    fd = open_file(state->path, O_RDONLY);
    if (fd < 0) return io_error();
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) status = AGENT_ERROR_IO;
    if (status == AGENT_OK) {
        off_t actual;
        do {
            actual = lseek(fd, position, SEEK_SET);
        } while (actual < 0 && errno == EINTR);
        if (actual != position) status = AGENT_ERROR_IO;
    }
    while (status == AGENT_OK && done < capacity) {
        size_t amount = capacity - done;
        ssize_t count;
        if (amount > (size_t)INT_MAX) amount = (size_t)INT_MAX;
        count = read(fd, (unsigned char*)output + done, amount);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) status = AGENT_ERROR_IO;
        else if (!count) break;
        else done += (size_t)count;
    }
    status = close_file(fd, status);
    if (status == AGENT_OK) *bytes_read = done;
    return status;
}

static agent_error_t write_bytes(int fd, const void* data, size_t bytes)
{
    size_t done = 0u;

    while (done < bytes) {
        size_t amount = bytes - done;
        ssize_t count;
        if (amount > (size_t)INT_MAX) amount = (size_t)INT_MAX;
        count = write(fd, (const unsigned char*)data + done, amount);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return AGENT_ERROR_IO;
        done += (size_t)count;
    }
    return AGENT_OK;
}

static agent_error_t posix_append(void* context, agent_string_view_t name,
                                 const void* data, size_t bytes)
{
    agent_posix_file_store_t* state = context;
    struct stat info;
    int fd;
    agent_error_t status;

    if (!valid_input(state, data, bytes)) return AGENT_ERROR_INVALID;
    status = make_path(state, name);
    if (status != AGENT_OK) return status;
    status = regular_file(state->path, true);
    if (status != AGENT_OK) return status;
    fd = open_file(state->path, O_WRONLY | O_CREAT | O_APPEND);
    if (fd < 0) return io_error();
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) status = AGENT_ERROR_IO;
    if (status == AGENT_OK) status = write_bytes(fd, data, bytes);
    return close_file(fd, status);
}

static agent_error_t posix_truncate(void* context, agent_string_view_t name, uint64_t bytes)
{
    agent_posix_file_store_t* state = context;
    off_t length = (off_t)bytes;
    int fd;
    agent_error_t status;

    if (length < 0 || (uint64_t)length != bytes) return AGENT_ERROR_INVALID;
    status = make_path(state, name);
    if (status != AGENT_OK) return status;
    status = regular_file(state->path, false);
    if (status != AGENT_OK) return status;
    fd = open_file(state->path, O_WRONLY);
    if (fd < 0) return io_error();
    if (ftruncate(fd, length) != 0) status = io_error();
    return close_file(fd, status);
}

static agent_error_t posix_sync(void* context, agent_string_view_t name)
{
    agent_posix_file_store_t* state = context;
    int fd;
    agent_error_t status = make_path(state, name);

    if (status != AGENT_OK) return status;
    status = regular_file(state->path, false);
    if (status != AGENT_OK) return status;
    fd = open_file(state->path, O_RDONLY);
    if (fd < 0) return io_error();
    status = close_file(fd, sync_fd(fd));
    return status == AGENT_OK ? sync_root(state) : status;
}

static agent_error_t posix_remove(void* context, agent_string_view_t name)
{
    agent_posix_file_store_t* state = context;
    agent_error_t status = make_path(state, name);

    if (status != AGENT_OK) return status;
    status = regular_file(state->path, false);
    if (status != AGENT_OK) return status;
    if (unlink(state->path) != 0) return io_error();
    return sync_root(state);
}

static agent_error_t posix_visit(void* context, agent_file_visit_fn visitor, void* user_data)
{
    agent_posix_file_store_t* state = context;
    DIR* dir = opendir(state->directory);
    agent_error_t status = AGENT_OK;

    if (!dir) return io_error();
    for (;;) {
        struct dirent* entry;
        struct stat info;
        agent_string_view_t name;
        bool stop = false;

        errno = 0;
        entry = readdir(dir);
        if (!entry) {
            if (errno) status = AGENT_ERROR_IO;
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 ||
            strncmp(entry->d_name, ".cagent-", 8u) == 0) continue;
        name = agent_string_view(entry->d_name, strlen(entry->d_name));
        status = make_path(state, name);
        if (status != AGENT_OK) break;
        if (path_stat(state->path, &info) != 0) {
            status = io_error();
            break;
        }
        if (!S_ISREG(info.st_mode)) continue;
        status = visitor(user_data, name, &stop);
        if (status != AGENT_OK || stop) break;
    }
    if (closedir(dir) != 0 && status == AGENT_OK) status = AGENT_ERROR_IO;
    return status;
}

static agent_error_t posix_replace(void* context, agent_string_view_t name,
                                  const void* data, size_t bytes, bool* published)
{
    agent_posix_file_store_t* state = context;
    size_t base = base_bytes(state);
    size_t target_bytes;
    size_t remaining;
    char* temporary;
    unsigned int attempts;
    int fd = -1;
    agent_error_t status;

    if (!valid_input(state, data, bytes)) return AGENT_ERROR_INVALID;
    status = make_path(state, name);
    if (status != AGENT_OK) return status;
    status = regular_file(state->path, true);
    if (status != AGENT_OK) return status;
    target_bytes = base + name.size + 1u;
    remaining = state->path_capacity - target_bytes;
    if (base >= remaining || remaining - base < sizeof(".cagent-ffffffff"))
        return AGENT_ERROR_CAPACITY;
    temporary = state->path + target_bytes;
    memcpy(temporary, state->path, base);
    for (attempts = 0u; attempts < 128u; ++attempts) {
        static const char hex[] = "0123456789abcdef";
        uint32_t sequence = state->temporary_sequence++;
        size_t i;
        memcpy(temporary + base, ".cagent-", 8u);
        for (i = 0u; i < 8u; ++i)
            temporary[base + 8u + i] = hex[(sequence >> (28u - (unsigned int)i * 4u)) & 15u];
        temporary[base + 16u] = '\0';
        fd = open_file(temporary, O_WRONLY | O_CREAT | O_EXCL);
        if (fd >= 0) break;
        if (errno != EEXIST) return io_error();
    }
    if (fd < 0) return AGENT_ERROR_EXISTS;
    status = write_bytes(fd, data, bytes);
    if (status == AGENT_OK) status = sync_fd(fd);
    status = close_file(fd, status);
    if (status == AGENT_OK && rename(temporary, state->path) != 0) status = io_error();
    if (status != AGENT_OK) {
        /* Cleanup failure still reports IO; a private leftover never becomes a user file. */
        if (unlink(temporary) != 0) return AGENT_ERROR_IO;
        return status;
    }
    *published = true;
    return sync_root(state);
}

agent_error_t agent_posix_file_store_init(agent_posix_file_store_t* state,
                                         const agent_posix_file_store_config_t* config,
                                         agent_file_store_t* store)
{
    static const agent_file_store_ops_t readonly_ops = {
        posix_size, posix_read, posix_visit, NULL, NULL, NULL, NULL, NULL
    };
    static const agent_file_store_ops_t writable_ops = {
        posix_size, posix_read, posix_visit, posix_append, posix_truncate,
        posix_sync, posix_remove, posix_replace
    };
    agent_posix_file_store_t candidate;
    struct stat info;
    size_t length;
    size_t base;
    agent_error_t status;

    if (!state || !config || !store || !config->directory || !config->path_buffer ||
        !config->path_capacity) return AGENT_ERROR_INVALID;
    length = strlen(config->directory);
    if (!length || config->directory[0] != '/' ||
        overlaps(state, sizeof(*state), store, sizeof(*store)) ||
        overlaps(state, sizeof(*state), config, sizeof(*config)) ||
        overlaps(config->path_buffer, config->path_capacity, state, sizeof(*state)) ||
        overlaps(config->path_buffer, config->path_capacity, store, sizeof(*store)) ||
        overlaps(config->path_buffer, config->path_capacity, config, sizeof(*config)) ||
        overlaps(config->directory, length + 1u, state, sizeof(*state)) ||
        overlaps(config->directory, length + 1u, store, sizeof(*store)) ||
        overlaps(config->directory, length + 1u, config->path_buffer, config->path_capacity) ||
        (config->read_only && config->sync_directory)) return AGENT_ERROR_INVALID;
    base = length + (config->directory[length - 1u] == '/' ? 0u : 1u);
    if (base < length || base >= config->path_capacity || config->path_capacity - base < 2u)
        return AGENT_ERROR_CAPACITY;
    if (path_stat(config->directory, &info) != 0) return io_error();
    if (!S_ISDIR(info.st_mode)) return AGENT_ERROR_INVALID;
    candidate.directory = config->directory;
    candidate.directory_length = length;
    candidate.path = config->path_buffer;
    candidate.path_capacity = config->path_capacity;
    candidate.temporary_sequence = 0u;
    candidate.sync_directory = config->sync_directory;
    status = sync_root(&candidate);
    if (status != AGENT_OK) return status;
    *state = candidate;
    store->ops = config->read_only ? &readonly_ops : &writable_ops;
    store->context = state;
    return AGENT_OK;
}
