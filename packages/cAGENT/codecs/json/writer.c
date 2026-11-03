/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
#include "json_internal.h"

#include <string.h>

static agent_error_t fail_writer(agent_json_writer_t* writer, agent_error_t status)
{
    if (writer && writer->status == AGENT_OK) {
        writer->status = status;
    }
    return status;
}

static agent_error_t append_bytes(agent_json_writer_t* writer,
                                  const char* data, size_t size)
{
    if (!writer) {
        return AGENT_ERROR_INVALID;
    }
    if (writer->status != AGENT_OK) {
        return writer->status;
    }
    if (size != 0u && !data) {
        return fail_writer(writer, AGENT_ERROR_INVALID);
    }
    if (!writer->data || writer->capacity == 0u ||
        writer->length >= writer->capacity) {
        writer->status = AGENT_ERROR_INVALID;
        return writer->status;
    }
    if (size > writer->capacity - writer->length - 1u) {
        writer->status = AGENT_ERROR_CAPACITY;
        return writer->status;
    }
    if (size != 0u) {
        memcpy(writer->data + writer->length, data, size);
        writer->length += size;
    }
    writer->data[writer->length] = '\0';
    return AGENT_OK;
}

void agent_json_writer_init(agent_json_writer_t* writer, char* buffer, size_t capacity)
{
    if (!writer) {
        return;
    }
    writer->data = buffer;
    writer->capacity = capacity;
    writer->length = 0u;
    writer->status = (buffer && capacity > 0u) ? AGENT_OK : AGENT_ERROR_INVALID;
    if (writer->status == AGENT_OK) {
        buffer[0] = '\0';
    }
}

agent_error_t agent_json_writer_literal(agent_json_writer_t* writer,
                                        agent_string_view_t literal)
{
    return append_bytes(writer, literal.data, literal.size);
}

agent_error_t agent_json_writer_string(agent_json_writer_t* writer,
                                       agent_string_view_t value)
{
    static const char hex[] = "0123456789abcdef";
    size_t i;
    agent_error_t status;

    if (value.size != 0u && !value.data) {
        return fail_writer(writer, AGENT_ERROR_INVALID);
    }
    status = append_bytes(writer, "\"", 1u);
    if (status != AGENT_OK) {
        return status;
    }
    for (i = 0u; i < value.size;) {
        unsigned char c = (unsigned char)value.data[i];
        if (c == '"' || c == '\\') {
            char escaped[2] = {'\\', (char)c};
            status = append_bytes(writer, escaped, sizeof(escaped));
            ++i;
        } else if (c < 0x20u) {
            char escaped[6] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 0x0fu]};
            status = append_bytes(writer, escaped, sizeof(escaped));
            ++i;
        } else {
            size_t width;
            status = agent_json_utf8_width((const unsigned char*)value.data + i,
                                           value.size - i, &width);
            if (status != AGENT_OK) {
                return fail_writer(writer, status);
            }
            status = append_bytes(writer, value.data + i, width);
            i += width;
        }
        if (status != AGENT_OK) {
            return status;
        }
    }
    return append_bytes(writer, "\"", 1u);
}

agent_error_t agent_json_writer_u32(agent_json_writer_t* writer, uint32_t value)
{
    char digits[10];
    char output[10];
    size_t count = 0u;
    size_t i;

    do {
        digits[count++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value != 0u);
    for (i = 0u; i < count; ++i) {
        output[i] = digits[count - i - 1u];
    }
    return append_bytes(writer, output, count);
}

agent_error_t agent_json_writer_bool(agent_json_writer_t* writer, bool value)
{
    return append_bytes(writer, value ? "true" : "false", value ? 4u : 5u);
}

agent_error_t agent_json_writer_raw_value(agent_json_writer_t* writer,
                                          const agent_json_document_t* document,
                                          size_t token)
{
    agent_string_view_t raw;
    agent_error_t status;

    if (!writer) {
        return AGENT_ERROR_INVALID;
    }
    if (writer->status != AGENT_OK) {
        return writer->status;
    }
    status = agent_json_raw_value(document, token, &raw);
    if (status != AGENT_OK) {
        return fail_writer(writer, status);
    }
    return append_bytes(writer, raw.data, raw.size);
}

agent_error_t agent_json_writer_finish(agent_json_writer_t* writer,
                                       agent_string_view_t* output)
{
    if (!writer || !output) {
        return AGENT_ERROR_INVALID;
    }
    *output = agent_string_view(NULL, 0u);
    if (writer->status != AGENT_OK) {
        return writer->status;
    }
    *output = agent_string_view(writer->data, writer->length);
    return AGENT_OK;
}
