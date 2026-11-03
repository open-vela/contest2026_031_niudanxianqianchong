/* Validate user-facing text against actual settings status formatters. */
#include "smart_home_lvgl_settings.c"
#include <assert.h>

int main(void)
{
    char buffer[128];
    smart_home_network_status_t network = {0};
    smart_home_agent_app_t app = {0};
    assert(!strcmp(network_status_text(NULL, buffer, sizeof(buffer)), "待检测"));
    network.ifname = "wlan0";
    network.online = true;
    assert(!strcmp(network_status_text(&network, buffer, sizeof(buffer)), "在线（wlan0）"));
    network.online = false;
    network.ip_status = SMART_HOME_NETWORK_ERR_NO_IP;
    assert(strstr(network_status_text(&network, buffer, sizeof(buffer)), "未获取地址"));
    network.ip_status = SMART_HOME_NETWORK_ERR_DHCP;
    assert(strstr(network_status_text(&network, buffer, sizeof(buffer)), "地址获取失败"));
    network.ip_status = AGENT_OK;
    network.dns_status = SMART_HOME_NETWORK_ERR_DNS;
    assert(strstr(network_status_text(&network, buffer, sizeof(buffer)), "域名解析失败"));
    network.backend = SMART_HOME_NETWORK_BACKEND_WIFI;
    network.init_status = SMART_HOME_WIFI_ERR_ASSOC;
    assert(!strcmp(wifi_status_text(&network, buffer, sizeof(buffer)), "路由器连接失败"));
    assert(!strcmp(tool_category_name(SMART_HOME_TOOL_CATEGORY_QUERY), "查询"));
    assert(!strcmp(tool_category_name(SMART_HOME_TOOL_CATEGORY_CONTROL), "控制"));
    assert(!strcmp(smart_home_tool_source_label(smart_home_tool_source(0, "board_camera")), "本机"));
    assert(!strcmp(smart_home_tool_source_label(smart_home_tool_source(0, "miot_device_control")), "米家"));
    assert(!strcmp(smart_home_tool_source_label(smart_home_tool_source(0, "set_light")), "演示"));
    assert(!strcmp(smart_home_tool_source_label(smart_home_tool_source(10, "status")), "Node"));
    assert(!strcmp(smart_home_tool_source_label(smart_home_tool_source(20, "status")), "MCP"));
    app.system_status.skills_status = AGENT_OK;
    app.system_status.skills_loaded = 3;
    assert(!strcmp(skills_status_text(&app, buffer, sizeof(buffer)), "已加载 3 项"));
    app.system_status.model_status = AGENT_OK;
    strcpy(app.model_config.backend_id, "sample-backend");
    assert(!strcmp(model_status_text(&app, buffer, sizeof(buffer)), "就绪（sample-backend）"));
    assert(!strcmp(simple_agent_status_text(SMART_HOME_STATUS_UNKNOWN, "就绪", buffer,
                                          sizeof(buffer)), "待检测"));
    assert(strstr(simple_agent_status_text(AGENT_ERROR_INVALID, "就绪", buffer,
                                          sizeof(buffer)), "失败"));
#ifndef CONFIG_SMART_HOME_NODE_GATEWAY
    assert(!strcmp(node_gateway_status_text(&app, buffer, sizeof(buffer)), "未启用"));
#else
    app.system_status.node_gateway_status = AGENT_OK;
    assert(!strcmp(node_gateway_status_text(&app, buffer, sizeof(buffer)), "监听中（18790）"));
#endif
    puts("PASS: Chinese network, permission, model and service text; raw backend IDs preserved");
    return 0;
}
