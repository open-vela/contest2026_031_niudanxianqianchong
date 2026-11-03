/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 tanglinjie */
/* Shared text admission, independent of JSON and optional domains. */
#pragma once
#include <agent/types.h>

static inline bool agent_bytes_overlap(const void* a, size_t na, const void* b, size_t nb)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    if (!na || !nb) return false;
    if (na > UINTPTR_MAX - x || nb > UINTPTR_MAX - y) return true;
    return x < y + nb && y < x + na;
}

static inline agent_error_t agent_text_validate(agent_string_view_t text)
{
    size_t i = 0u;
    if (text.size && !text.data) return AGENT_ERROR_INVALID;
    while (i < text.size) {
        unsigned char c = (unsigned char)text.data[i++];
        uint32_t value, minimum;
        size_t continuation;
        if (!c) return AGENT_ERROR_INVALID;
        if (c < 0x80u) continue;
        if (c >= 0xc2u && c <= 0xdfu) {
            value = c & 0x1fu; minimum = 0x80u; continuation = 1u;
        } else if (c >= 0xe0u && c <= 0xefu) {
            value = c & 0x0fu; minimum = 0x800u; continuation = 2u;
        } else if (c >= 0xf0u && c <= 0xf4u) {
            value = c & 0x07u; minimum = 0x10000u; continuation = 3u;
        } else return AGENT_ERROR_PARSE;
        if (continuation > text.size - i) return AGENT_ERROR_PARSE;
        while (continuation--) {
            c = (unsigned char)text.data[i++];
            if ((c & 0xc0u) != 0x80u) return AGENT_ERROR_PARSE;
            value = (value << 6) | (c & 0x3fu);
        }
        if (value < minimum || value > 0x10ffffu ||
            (value >= 0xd800u && value <= 0xdfffu)) return AGENT_ERROR_PARSE;
    }
    return AGENT_OK;
}

static inline bool agent_text_name_valid(agent_string_view_t name, size_t maximum)
{
    size_t i;
    if (!name.data || !name.size || name.size > maximum) return false;
    for (i = 0u; i < name.size; ++i) {
        unsigned char c = (unsigned char)name.data[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}
