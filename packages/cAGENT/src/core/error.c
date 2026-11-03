/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/**
 * cAgent 错误码 实现（骨架）。
 *
 * 职责：agent_error_t 到稳定 ASCII 标识的映射
 * V1 参考：src/core/error.c
 */

#include <agent/error.h>

const char* agent_error_str(agent_error_t code)
{
    switch (code)
    {
        case AGENT_OK:
            return "AGENT_OK";
        case AGENT_ERROR:
            return "AGENT_ERROR";
        case AGENT_ERROR_NOMEM:
            return "AGENT_ERROR_NOMEM";
        case AGENT_ERROR_INVALID:
            return "AGENT_ERROR_INVALID";
        case AGENT_ERROR_STATE:
            return "AGENT_ERROR_STATE";
        case AGENT_ERROR_BUSY:
            return "AGENT_ERROR_BUSY";
        case AGENT_ERROR_LIMIT:
            return "AGENT_ERROR_LIMIT";
        case AGENT_ERROR_TIMEOUT:
            return "AGENT_ERROR_TIMEOUT";
        case AGENT_ERROR_CANCELLED:
            return "AGENT_ERROR_CANCELLED";
        case AGENT_ERROR_NOT_FOUND:
            return "AGENT_ERROR_NOT_FOUND";
        case AGENT_ERROR_EXISTS:
            return "AGENT_ERROR_EXISTS";
        case AGENT_ERROR_NOT_SUPPORTED:
            return "AGENT_ERROR_NOT_SUPPORTED";
        case AGENT_ERROR_IO:
            return "AGENT_ERROR_IO";
        case AGENT_ERROR_AUTH:
            return "AGENT_ERROR_AUTH";
        case AGENT_ERROR_TRUNCATED:
            return "AGENT_ERROR_TRUNCATED";
        case AGENT_ERROR_PARSE:
            return "AGENT_ERROR_PARSE";
        case AGENT_ERROR_CAPACITY:
            return "AGENT_ERROR_CAPACITY";
        case AGENT_ERROR_CONTEXT_OVERFLOW:
            return "AGENT_ERROR_CONTEXT_OVERFLOW";
        case AGENT_ERROR_MODEL_FAILED:
            return "AGENT_ERROR_MODEL_FAILED";
        case AGENT_ERROR_MODEL_PARSE:
            return "AGENT_ERROR_MODEL_PARSE";
        case AGENT_ERROR_MODEL_RATE_LIMIT:
            return "AGENT_ERROR_MODEL_RATE_LIMIT";
        case AGENT_ERROR_MODEL_UNAVAILABLE:
            return "AGENT_ERROR_MODEL_UNAVAILABLE";
        case AGENT_ERROR_POLICY_DENIED:
            return "AGENT_ERROR_POLICY_DENIED";
        case AGENT_ERROR_TOOL_ARGUMENT:
            return "AGENT_ERROR_TOOL_ARGUMENT";
        case AGENT_ERROR_TOOL_FAILED:
            return "AGENT_ERROR_TOOL_FAILED";
        default:
            return "AGENT_ERROR_UNKNOWN";
    }
}
