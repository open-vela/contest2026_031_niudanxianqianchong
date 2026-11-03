/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/*
 * cagent2_chat：基于 cAgent2（vendored cAgentV2）的单轮问答 demo（极简版）。
 * 链路：输入 -> agent_run（ReAct 单步，无工具/无会话历史）
 * -> OpenAI 兼容 provider -> webclient transport + mbedTLS -> 打印回复。
 */

#include <agent.h>
#include <agent/model.h>

#include <agent_openai_model.h>
#include <agent_openvela_runtime.h>
#include <agent_openvela_tls.h>
#include <agent_openvela_transport.h>

#include <stdio.h>
#include <string.h>

/* ---- 硬编码配置：改这三个宏。真实 key 勿提交仓库/随固件分发 ---- */

#define CAGENT2_CHAT_API_KEY  "sk-请替换成你的key"
#define CAGENT2_CHAT_ENDPOINT "https://api.deepseek.com/chat/completions"
#define CAGENT2_CHAT_MODEL    "deepseek-chat"

/* ---- 借用式缓冲与状态：全部静态，生命周期覆盖进程 ---- */

static agent_workspace_t g_core_workspace;
static agent_model_workspace_t g_model_workspace;
static agent_port_openvela_tls_t g_tls_state;
static agent_port_openvela_transport_t g_transport_state;
static agent_openai_provider_t g_provider;

/* transport 六块缓冲 */
static char g_url_buffer[512];
static char g_request_header_buffer[1024];
static const char* g_request_headers[8];
static char g_io_buffer[2048];
static agent_http_header_t g_response_headers[16];
static char g_response_header_buffer[2048];

/* provider 三块互不重叠缓冲 */
static char g_request_buffer[16384];
static char g_response_buffer[16384];
static char g_decoded_buffer[4096];

static char g_authorization[320]; /* "Bearer " + key */
static char g_output[AGENT_MAX_MODEL_OUTPUT_BYTES];

/* TLS -> transport -> provider -> agent 一次装配 */
static agent_error_t build_stack(agent_t** agent_out, agent_model_t** model_out)
{
    static const char system_prompt[] =
        "你是运行在 openvela 嵌入式设备上的助手，回答请简洁。";

    agent_runtime_t runtime;
    agent_transport_t transport;
    agent_port_openvela_transport_config_t transport_config = { 0 };
    agent_openai_config_t openai_config = { 0 };
    agent_config_t config;
    agent_error_t status;

    status = agent_port_openvela_runtime_init(&runtime);
    if (status != AGENT_OK)
    {
        return status;
    }

    /* 零初始化 TLS 配置 = VERIFY_OPTIONAL（已知债务，见包 README） */
    status = agent_port_openvela_tls_init(
        &g_tls_state, &(agent_port_openvela_tls_config_t){ 0 });
    if (status != AGENT_OK)
    {
        return status;
    }

    transport_config.runtime = runtime;
    transport_config.tls_ops = g_tls_state.ops;
    transport_config.tls_context = &g_tls_state;
    transport_config.request_timeout_ms = 30000u;
    transport_config.url_buffer = g_url_buffer;
    transport_config.url_buffer_size = sizeof(g_url_buffer);
    transport_config.request_header_buffer = g_request_header_buffer;
    transport_config.request_header_buffer_size =
        sizeof(g_request_header_buffer);
    transport_config.request_headers = g_request_headers;
    transport_config.request_header_capacity =
        sizeof(g_request_headers) / sizeof(g_request_headers[0]);
    transport_config.io_buffer = g_io_buffer;
    transport_config.io_buffer_size = sizeof(g_io_buffer);
    transport_config.response_headers = g_response_headers;
    transport_config.response_header_capacity =
        sizeof(g_response_headers) / sizeof(g_response_headers[0]);
    transport_config.response_header_buffer = g_response_header_buffer;
    transport_config.response_header_buffer_size =
        sizeof(g_response_header_buffer);
    status = agent_port_openvela_transport_init(&transport, &g_transport_state,
                                                &transport_config);
    if (status != AGENT_OK)
    {
        return status;
    }

    snprintf(g_authorization, sizeof(g_authorization), "Bearer %s",
             CAGENT2_CHAT_API_KEY);
    openai_config.transport = transport;
    openai_config.runtime = runtime;
    openai_config.endpoint = (agent_string_view_t)
        AGENT_SV_LITERAL(CAGENT2_CHAT_ENDPOINT);
    openai_config.model = (agent_string_view_t)
        AGENT_SV_LITERAL(CAGENT2_CHAT_MODEL);
    openai_config.authorization =
        (agent_string_view_t){ g_authorization, strlen(g_authorization) };
    openai_config.request_buffer = g_request_buffer;
    openai_config.request_capacity = sizeof(g_request_buffer);
    openai_config.response_buffer = g_response_buffer;
    openai_config.response_capacity = sizeof(g_response_buffer);
    openai_config.decoded_buffer = g_decoded_buffer;
    openai_config.decoded_capacity = sizeof(g_decoded_buffer);
    openai_config.response_token_capacity = 512u;
    openai_config.max_response_header_bytes = sizeof(g_response_header_buffer);
    openai_config.max_json_depth = 16u;
    openai_config.use_legacy_max_tokens = true; /* DeepSeek 等网关兼容 */
    status = agent_openai_provider_init(&g_provider, &openai_config);
    if (status != AGENT_OK)
    {
        return status;
    }

    config = agent_config_default();
    config.system_prompt = (agent_string_view_t)
        AGENT_SV_LITERAL(system_prompt);
    config.runtime = runtime;
    status = agent_init(agent_out, &g_core_workspace, &config);
    if (status != AGENT_OK)
    {
        return status;
    }
    status = agent_model_init(model_out, &g_model_workspace,
                              agent_openai_model_ops(), &g_provider);
    if (status == AGENT_OK)
    {
        status = agent_set_model(*agent_out, *model_out);
    }
    if (status == AGENT_OK)
    {
        status = agent_start(*agent_out);
    }
    if (status != AGENT_OK)
    {
        agent_destroy(*agent_out);
        agent_model_destroy(*model_out);
    }
    return status;
}

int cagent2_chat_main(int argc, char* argv[])
{
    agent_request_t request = { 0 };
    agent_response_t response = { 0 };
    agent_t* agent = NULL;
    agent_model_t* model = NULL;
    char input[AGENT_MAX_INPUT_BYTES];
    agent_error_t status;

    if (argc > 1)
    {
        snprintf(input, sizeof(input), "%s", argv[1]);
    }
    else
    {
        printf("输入问题：");
        fflush(stdout);
        if (fgets(input, sizeof(input), stdin) == NULL || input[0] == '\n')
        {
            fprintf(stderr, "未收到输入\n");
            return 1;
        }
        input[strcspn(input, "\r\n")] = '\0';
    }

    status = build_stack(&agent, &model);
    if (status != AGENT_OK)
    {
        fprintf(stderr, "装配失败：%s\n", agent_error_str(status));
        return 1;
    }

    request.input = (agent_string_view_t){ input, strlen(input) };
    response.output = g_output;
    response.output_size = sizeof(g_output);

    status = agent_run(agent, &request, &response);
    if (status == AGENT_OK)
    {
        printf("回复：%.*s\n", (int)response.output_written, g_output);
    }
    else
    {
        fprintf(stderr, "失败：%s\n", agent_error_str(status));
    }

    /* 借用式模型：先销毁 agent 再销毁模型包装 */
    agent_destroy(agent);
    agent_model_destroy(model);
    return status == AGENT_OK ? 0 : 1;
}
