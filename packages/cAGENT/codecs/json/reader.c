/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
#define JSMN_STATIC
#include "jsmn.h"
#undef JSMN_STATIC

#include "json_internal.h"

#include <limits.h>
#include <string.h>

typedef struct {
    const unsigned char* data;
    size_t size;
    size_t pos;
    size_t max_depth;
    bool unique_keys;
} json_cursor_t;

agent_error_t agent_json_utf8_width(const unsigned char* input, size_t available,
                                    size_t* width)
{
    unsigned char first;
    size_t count;
    size_t i;

    if (!input || !width || available == 0u) {
        return AGENT_ERROR_INVALID;
    }
    first = input[0];
    if (first < 0x80u) {
        *width = 1u;
        return AGENT_OK;
    }
    if (first >= 0xc2u && first <= 0xdfu) {
        count = 2u;
    } else if (first >= 0xe0u && first <= 0xefu) {
        count = 3u;
    } else if (first >= 0xf0u && first <= 0xf4u) {
        count = 4u;
    } else {
        return AGENT_ERROR_PARSE;
    }
    if (available < count) {
        return AGENT_ERROR_PARSE;
    }
    for (i = 1u; i < count; ++i) {
        if ((input[i] & 0xc0u) != 0x80u) {
            return AGENT_ERROR_PARSE;
        }
    }
    if ((first == 0xe0u && input[1] < 0xa0u) ||
        (first == 0xedu && input[1] >= 0xa0u) ||
        (first == 0xf0u && input[1] < 0x90u) ||
        (first == 0xf4u && input[1] >= 0x90u)) {
        return AGENT_ERROR_PARSE;
    }
    *width = count;
    return AGENT_OK;
}

static void skip_space(json_cursor_t* cursor)
{
    while (cursor->pos < cursor->size) {
        unsigned char c = cursor->data[cursor->pos];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            break;
        }
        ++cursor->pos;
    }
}

static int hex_digit(unsigned char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static agent_error_t hex_quad(const unsigned char* data, size_t end, size_t pos,
                              uint32_t* value)
{
    uint32_t codepoint = 0u;
    size_t i;

    if (pos > end || end - pos < 4u) {
        return AGENT_ERROR_PARSE;
    }
    for (i = 0u; i < 4u; ++i) {
        int digit = hex_digit(data[pos + i]);
        if (digit < 0) {
            return AGENT_ERROR_PARSE;
        }
        codepoint = (codepoint << 4) | (uint32_t)digit;
    }
    *value = codepoint;
    return AGENT_OK;
}

static agent_error_t unicode_escape(const unsigned char* data, size_t end,
                                    size_t* pos, uint32_t* codepoint)
{
    uint32_t first;
    uint32_t second;
    agent_error_t status;

    status = hex_quad(data, end, *pos, &first);
    if (status != AGENT_OK) {
        return status;
    }
    *pos += 4u;
    if (first >= 0xd800u && first <= 0xdbffu) {
        if (*pos > end || end - *pos < 6u || data[*pos] != '\\' ||
            data[*pos + 1u] != 'u') {
            return AGENT_ERROR_PARSE;
        }
        status = hex_quad(data, end, *pos + 2u, &second);
        if (status != AGENT_OK || second < 0xdc00u || second > 0xdfffu) {
            return AGENT_ERROR_PARSE;
        }
        *pos += 6u;
        first = 0x10000u + ((first - 0xd800u) << 10) + (second - 0xdc00u);
    } else if (first >= 0xdc00u && first <= 0xdfffu) {
        return AGENT_ERROR_PARSE;
    }
    *codepoint = first;
    return AGENT_OK;
}

static agent_error_t parse_string(json_cursor_t* cursor)
{
    if (cursor->pos >= cursor->size || cursor->data[cursor->pos] != '"') {
        return AGENT_ERROR_PARSE;
    }
    ++cursor->pos;
    while (cursor->pos < cursor->size) {
        unsigned char c = cursor->data[cursor->pos++];
        if (c == '"') {
            return AGENT_OK;
        }
        if (c < 0x20u) {
            return AGENT_ERROR_PARSE;
        }
        if (c == '\\') {
            uint32_t codepoint;
            if (cursor->pos >= cursor->size) {
                return AGENT_ERROR_PARSE;
            }
            c = cursor->data[cursor->pos++];
            if (c == 'u') {
                agent_error_t status = unicode_escape(cursor->data, cursor->size,
                                                     &cursor->pos, &codepoint);
                if (status != AGENT_OK) {
                    return status;
                }
            } else if (c != '"' && c != '\\' && c != '/' && c != 'b' &&
                       c != 'f' && c != 'n' && c != 'r' && c != 't') {
                return AGENT_ERROR_PARSE;
            }
        } else if (c >= 0x80u) {
            size_t width;
            agent_error_t status = agent_json_utf8_width(cursor->data + cursor->pos - 1u,
                                                          cursor->size - cursor->pos + 1u,
                                                          &width);
            if (status != AGENT_OK) {
                return AGENT_ERROR_PARSE;
            }
            cursor->pos += width - 1u;
        }
    }
    return AGENT_ERROR_PARSE;
}

static agent_error_t parse_number(json_cursor_t* cursor)
{
    const unsigned char* data = cursor->data;
    size_t pos = cursor->pos;

    if (data[pos] == '-') {
        ++pos;
    }
    if (pos >= cursor->size) {
        return AGENT_ERROR_PARSE;
    }
    if (data[pos] == '0') {
        ++pos;
    } else {
        if (data[pos] < '1' || data[pos] > '9') {
            return AGENT_ERROR_PARSE;
        }
        do {
            ++pos;
        } while (pos < cursor->size && data[pos] >= '0' && data[pos] <= '9');
    }
    if (pos < cursor->size && data[pos] == '.') {
        ++pos;
        if (pos >= cursor->size || data[pos] < '0' || data[pos] > '9') {
            return AGENT_ERROR_PARSE;
        }
        do {
            ++pos;
        } while (pos < cursor->size && data[pos] >= '0' && data[pos] <= '9');
    }
    if (pos < cursor->size && (data[pos] == 'e' || data[pos] == 'E')) {
        ++pos;
        if (pos < cursor->size && (data[pos] == '+' || data[pos] == '-')) {
            ++pos;
        }
        if (pos >= cursor->size || data[pos] < '0' || data[pos] > '9') {
            return AGENT_ERROR_PARSE;
        }
        do {
            ++pos;
        } while (pos < cursor->size && data[pos] >= '0' && data[pos] <= '9');
    }
    cursor->pos = pos;
    return AGENT_OK;
}

static agent_error_t parse_value(json_cursor_t* cursor, size_t depth);

static agent_error_t check_object_key(const json_cursor_t* cursor, size_t first,
                                      size_t start, size_t end, size_t depth);

static agent_error_t parse_object(json_cursor_t* cursor, size_t depth)
{
    agent_error_t status;
    size_t first;

    if (depth >= cursor->max_depth) {
        return AGENT_ERROR_LIMIT;
    }
    ++cursor->pos;
    skip_space(cursor);
    first = cursor->pos;
    if (cursor->pos < cursor->size && cursor->data[cursor->pos] == '}') {
        ++cursor->pos;
        return AGENT_OK;
    }
    for (;;) {
        size_t start = cursor->pos;
        status = parse_string(cursor);
        if (status != AGENT_OK) {
            return status;
        }
        if (cursor->unique_keys) {
            status = check_object_key(cursor, first, start, cursor->pos, depth);
            if (status != AGENT_OK) return status;
        }
        skip_space(cursor);
        if (cursor->pos >= cursor->size || cursor->data[cursor->pos++] != ':') {
            return AGENT_ERROR_PARSE;
        }
        status = parse_value(cursor, depth + 1u);
        if (status != AGENT_OK) {
            return status;
        }
        skip_space(cursor);
        if (cursor->pos >= cursor->size) {
            return AGENT_ERROR_PARSE;
        }
        if (cursor->data[cursor->pos] == '}') {
            ++cursor->pos;
            return AGENT_OK;
        }
        if (cursor->data[cursor->pos++] != ',') {
            return AGENT_ERROR_PARSE;
        }
        skip_space(cursor);
    }
}

static agent_error_t parse_array(json_cursor_t* cursor, size_t depth)
{
    agent_error_t status;

    if (depth >= cursor->max_depth) {
        return AGENT_ERROR_LIMIT;
    }
    ++cursor->pos;
    skip_space(cursor);
    if (cursor->pos < cursor->size && cursor->data[cursor->pos] == ']') {
        ++cursor->pos;
        return AGENT_OK;
    }
    for (;;) {
        status = parse_value(cursor, depth + 1u);
        if (status != AGENT_OK) {
            return status;
        }
        skip_space(cursor);
        if (cursor->pos >= cursor->size) {
            return AGENT_ERROR_PARSE;
        }
        if (cursor->data[cursor->pos] == ']') {
            ++cursor->pos;
            return AGENT_OK;
        }
        if (cursor->data[cursor->pos++] != ',') {
            return AGENT_ERROR_PARSE;
        }
        skip_space(cursor);
    }
}

static agent_error_t parse_value(json_cursor_t* cursor, size_t depth)
{
    const char* literal = NULL;
    size_t literal_size = 0u;

    skip_space(cursor);
    if (cursor->pos >= cursor->size) {
        return AGENT_ERROR_PARSE;
    }
    switch (cursor->data[cursor->pos]) {
    case '{':
        return parse_object(cursor, depth);
    case '[':
        return parse_array(cursor, depth);
    case '"':
        return parse_string(cursor);
    case 't':
        literal = "true";
        literal_size = 4u;
        break;
    case 'f':
        literal = "false";
        literal_size = 5u;
        break;
    case 'n':
        literal = "null";
        literal_size = 4u;
        break;
    default:
        if (cursor->data[cursor->pos] == '-' ||
            (cursor->data[cursor->pos] >= '0' && cursor->data[cursor->pos] <= '9')) {
            return parse_number(cursor);
        }
        return AGENT_ERROR_PARSE;
    }
    if (cursor->size - cursor->pos < literal_size ||
        memcmp(cursor->data + cursor->pos, literal, literal_size) != 0) {
        return AGENT_ERROR_PARSE;
    }
    cursor->pos += literal_size;
    return AGENT_OK;
}

static agent_error_t validate_input(agent_string_view_t input, size_t max_depth)
{
    json_cursor_t cursor;
    agent_error_t status;

    if (!input.data || input.size == 0u || input.size > INT_MAX ||
        max_depth == 0u || max_depth > 32u) {
        return AGENT_ERROR_INVALID;
    }
    cursor.data = (const unsigned char*)input.data;
    cursor.size = input.size;
    cursor.pos = 0u;
    cursor.max_depth = max_depth;
    cursor.unique_keys = false;
    status = parse_value(&cursor, 0u);
    if (status != AGENT_OK) {
        return status;
    }
    skip_space(&cursor);
    if (cursor.pos != cursor.size) {
        return AGENT_ERROR_PARSE;
    }
    return AGENT_OK;
}

agent_error_t agent_json_validate_object(agent_string_view_t input, size_t max_depth)
{
    size_t pos = 0u;
    agent_error_t status = validate_input(input, max_depth);

    if (status != AGENT_OK) {
        return status;
    }
    while (pos < input.size &&
           (input.data[pos] == ' ' || input.data[pos] == '\t' ||
            input.data[pos] == '\r' || input.data[pos] == '\n')) {
        ++pos;
    }
    return pos < input.size && input.data[pos] == '{' ? AGENT_OK : AGENT_ERROR_PARSE;
}

agent_error_t agent_json_parse(agent_string_view_t input, jsmntok_t* tokens,
                               size_t token_capacity, size_t max_depth,
                               agent_json_document_t* document)
{
    jsmn_parser parser;
    agent_error_t status;
    int count;

    if (!document) {
        return AGENT_ERROR_INVALID;
    }
    memset(document, 0, sizeof(*document));
    if (!tokens || token_capacity == 0u || token_capacity > UINT_MAX) {
        return AGENT_ERROR_INVALID;
    }
    status = validate_input(input, max_depth);
    if (status != AGENT_OK) {
        return status;
    }
    jsmn_init(&parser);
    count = jsmn_parse(&parser, input.data, input.size, tokens,
                       (unsigned int)token_capacity);
    if (count == JSMN_ERROR_NOMEM) {
        return AGENT_ERROR_CAPACITY;
    }
    if (count <= 0 || tokens[0].start < 0 ||
        (tokens[0].type != JSMN_STRING && (size_t)tokens[0].end > input.size)) {
        return AGENT_ERROR_PARSE;
    }
    document->input = input;
    document->tokens = tokens;
    document->token_count = (size_t)count;
    return AGENT_OK;
}

static bool document_token_valid(const agent_json_document_t* document, size_t index)
{
    return document && document->input.data && document->tokens &&
           index < document->token_count && document->tokens[index].start >= 0 &&
           document->tokens[index].end >= document->tokens[index].start &&
           (size_t)document->tokens[index].end <= document->input.size;
}

static size_t next_after(const agent_json_document_t* document, size_t index)
{
    int end = document->tokens[index].end;
    ++index;
    while (index < document->token_count && document->tokens[index].start < end) {
        ++index;
    }
    return index;
}

static size_t encode_codepoint(uint32_t codepoint, unsigned char output[4])
{
    if (codepoint < 0x80u) {
        output[0] = (unsigned char)codepoint;
        return 1u;
    }
    if (codepoint < 0x800u) {
        output[0] = (unsigned char)(0xc0u | (codepoint >> 6));
        output[1] = (unsigned char)(0x80u | (codepoint & 0x3fu));
        return 2u;
    }
    if (codepoint < 0x10000u) {
        output[0] = (unsigned char)(0xe0u | (codepoint >> 12));
        output[1] = (unsigned char)(0x80u | ((codepoint >> 6) & 0x3fu));
        output[2] = (unsigned char)(0x80u | (codepoint & 0x3fu));
        return 3u;
    }
    output[0] = (unsigned char)(0xf0u | (codepoint >> 18));
    output[1] = (unsigned char)(0x80u | ((codepoint >> 12) & 0x3fu));
    output[2] = (unsigned char)(0x80u | ((codepoint >> 6) & 0x3fu));
    output[3] = (unsigned char)(0x80u | (codepoint & 0x3fu));
    return 4u;
}

static agent_error_t decode_escape(const unsigned char* data, size_t end,
                                   size_t* pos, unsigned char output[4], size_t* count)
{
    unsigned char escape;
    uint32_t codepoint;

    if (*pos >= end || data[(*pos)++] != '\\' || *pos >= end) {
        return AGENT_ERROR_PARSE;
    }
    escape = data[(*pos)++];
    if (escape == 'u') {
        agent_error_t status = unicode_escape(data, end, pos, &codepoint);
        if (status != AGENT_OK) {
            return status;
        }
        *count = encode_codepoint(codepoint, output);
        return AGENT_OK;
    }
    switch (escape) {
    case '"': case '\\': case '/':
        output[0] = escape;
        break;
    case 'b': output[0] = '\b'; break;
    case 'f': output[0] = '\f'; break;
    case 'n': output[0] = '\n'; break;
    case 'r': output[0] = '\r'; break;
    case 't': output[0] = '\t'; break;
    default: return AGENT_ERROR_PARSE;
    }
    *count = 1u;
    return AGENT_OK;
}

typedef struct {
    const unsigned char* data;
    size_t pos, end, used, count;
    unsigned char bytes[4];
} key_cursor_t;

static int key_next(key_cursor_t* key)
{
    if (key->used < key->count) return key->bytes[key->used++];
    if (key->pos == key->end) return -1;
    key->used = 0u;
    key->count = 1u;
    if (key->data[key->pos] == '\\') {
        if (decode_escape(key->data, key->end, &key->pos, key->bytes, &key->count) != AGENT_OK)
            return -2;
    } else {
        key->bytes[0] = key->data[key->pos++];
    }
    return key->bytes[key->used++];
}

/* Re-scan earlier members without recursive uniqueness checks: bounded O(n^2), O(depth) stack. */
static agent_error_t check_object_key(const json_cursor_t* cursor, size_t first,
                                      size_t start, size_t end, size_t depth)
{
    json_cursor_t previous = *cursor;
    key_cursor_t current = {cursor->data, start + 1u, end - 1u, 0u, 0u, {0}};
    int byte;
    while ((byte = key_next(&current)) >= 0) {
        if (byte == 0) return AGENT_ERROR_PARSE;
    }
    if (byte != -1) return AGENT_ERROR_PARSE;
    previous.pos = first;
    previous.unique_keys = false;
    while (previous.pos < start) {
        size_t old_start = previous.pos;
        key_cursor_t old;
        bool equal = true;
        agent_error_t status = parse_string(&previous);
        if (status != AGENT_OK) return status;
        old = (key_cursor_t){cursor->data, old_start + 1u, previous.pos - 1u, 0u, 0u, {0}};
        current = (key_cursor_t){cursor->data, start + 1u, end - 1u, 0u, 0u, {0}};
        do {
            byte = key_next(&current);
            if (byte != key_next(&old)) equal = false;
        } while (equal && byte >= 0);
        if (equal) return AGENT_ERROR_PARSE;
        skip_space(&previous);
        if (previous.pos >= previous.size || previous.data[previous.pos++] != ':')
            return AGENT_ERROR_PARSE;
        status = parse_value(&previous, depth + 1u);
        if (status != AGENT_OK) return status;
        skip_space(&previous);
        if (previous.pos >= previous.size || previous.data[previous.pos++] != ',')
            return AGENT_ERROR_PARSE;
        skip_space(&previous);
    }
    return AGENT_OK;
}

agent_error_t agent_json_validate_unique_object(agent_string_view_t input, size_t max_depth)
{
    json_cursor_t cursor;
    agent_error_t status = agent_json_validate_object(input, max_depth);
    if (status != AGENT_OK) return status;
    cursor.data = (const unsigned char*)input.data;
    cursor.size = input.size;
    cursor.pos = 0u;
    cursor.max_depth = max_depth;
    cursor.unique_keys = true;
    return parse_value(&cursor, 0u);
}

static bool string_equals(const agent_json_document_t* document, size_t token,
                          agent_string_view_t expected)
{
    const unsigned char* data = (const unsigned char*)document->input.data;
    size_t pos = (size_t)document->tokens[token].start;
    size_t end = (size_t)document->tokens[token].end;
    size_t matched = 0u;

    while (pos < end) {
        unsigned char bytes[4];
        size_t count = 1u;
        size_t i;

        if (data[pos] == '\\') {
            if (decode_escape(data, end, &pos, bytes, &count) != AGENT_OK) {
                return false;
            }
        } else {
            bytes[0] = data[pos++];
        }
        if (count > expected.size - matched) {
            return false;
        }
        for (i = 0u; i < count; ++i) {
            if ((unsigned char)expected.data[matched + i] != bytes[i]) {
                return false;
            }
        }
        matched += count;
    }
    return matched == expected.size;
}

agent_error_t agent_json_object_get(const agent_json_document_t* document,
                                    size_t object, agent_string_view_t key,
                                    size_t* value)
{
    size_t current;
    size_t found = SIZE_MAX;
    int end;

    if (!value || (key.size != 0u && !key.data) ||
        !document_token_valid(document, object) ||
        document->tokens[object].type != JSMN_OBJECT) {
        return AGENT_ERROR_INVALID;
    }
    end = document->tokens[object].end;
    current = object + 1u;
    while (current < document->token_count && document->tokens[current].start < end) {
        size_t child = current + 1u;
        if (!document_token_valid(document, current) ||
            document->tokens[current].type != JSMN_STRING ||
            !document_token_valid(document, child) ||
            document->tokens[child].start >= end) {
            return AGENT_ERROR_PARSE;
        }
        if (string_equals(document, current, key)) {
            if (found != SIZE_MAX) {
                return AGENT_ERROR_PARSE;
            }
            found = child;
        }
        current = next_after(document, child);
    }
    if (found == SIZE_MAX) {
        return AGENT_ERROR_NOT_FOUND;
    }
    *value = found;
    return AGENT_OK;
}

agent_error_t agent_json_array_get(const agent_json_document_t* document,
                                   size_t array, size_t element, size_t* value)
{
    size_t current;
    size_t index = 0u;
    int end;

    if (!value || !document_token_valid(document, array) ||
        document->tokens[array].type != JSMN_ARRAY) {
        return AGENT_ERROR_INVALID;
    }
    end = document->tokens[array].end;
    current = array + 1u;
    while (current < document->token_count && document->tokens[current].start < end) {
        if (index == element) {
            *value = current;
            return AGENT_OK;
        }
        current = next_after(document, current);
        ++index;
    }
    return AGENT_ERROR_NOT_FOUND;
}

agent_error_t agent_json_path_get(const agent_json_document_t* document,
                                  size_t root, const agent_json_path_segment_t* path,
                                  size_t count, size_t* value)
{
    size_t current = root;
    size_t i;

    if (!value || !document_token_valid(document, root) || (count > 0u && !path)) {
        return AGENT_ERROR_INVALID;
    }
    for (i = 0u; i < count; ++i) {
        agent_error_t status;
        if (path[i].kind == AGENT_JSON_PATH_KEY) {
            status = agent_json_object_get(document, current, path[i].key, &current);
        } else if (path[i].kind == AGENT_JSON_PATH_INDEX) {
            status = agent_json_array_get(document, current, path[i].index, &current);
        } else {
            return AGENT_ERROR_INVALID;
        }
        if (status != AGENT_OK) {
            return status;
        }
    }
    *value = current;
    return AGENT_OK;
}

agent_error_t agent_json_decode_string(const agent_json_document_t* document,
                                       size_t token, char* output, size_t capacity,
                                       agent_string_view_t* decoded)
{
    const unsigned char* data;
    size_t pos;
    size_t end;
    size_t length = 0u;

    if (!decoded || !document_token_valid(document, token) ||
        document->tokens[token].type != JSMN_STRING ||
        (capacity != 0u && !output)) {
        return AGENT_ERROR_INVALID;
    }
    data = (const unsigned char*)document->input.data;
    pos = (size_t)document->tokens[token].start;
    end = (size_t)document->tokens[token].end;
    while (pos < end) {
        unsigned char bytes[4];
        size_t count = 1u;
        if (data[pos] == '\\') {
            agent_error_t status = decode_escape(data, end, &pos, bytes, &count);
            if (status != AGENT_OK) {
                return status;
            }
        } else {
            bytes[0] = data[pos++];
        }
        if (count > capacity - length) {
            return AGENT_ERROR_CAPACITY;
        }
        memcpy(output + length, bytes, count);
        length += count;
    }
    decoded->data = output;
    decoded->size = length;
    return AGENT_OK;
}

agent_error_t agent_json_raw_value(const agent_json_document_t* document,
                                   size_t token, agent_string_view_t* raw)
{
    size_t start;
    size_t end;

    if (!raw || !document_token_valid(document, token)) {
        return AGENT_ERROR_INVALID;
    }
    start = (size_t)document->tokens[token].start;
    end = (size_t)document->tokens[token].end;
    if (document->tokens[token].type == JSMN_STRING) {
        if (start == 0u || end >= document->input.size ||
            document->input.data[start - 1u] != '"' ||
            document->input.data[end] != '"') {
            return AGENT_ERROR_PARSE;
        }
        --start;
        ++end;
    }
    *raw = agent_string_view(document->input.data + start, end - start);
    return AGENT_OK;
}
