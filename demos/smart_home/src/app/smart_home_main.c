#include "../agent/smart_home_agent.h"
#include "../net/smart_home_network.h"
#include "../config/smart_home_secrets.h"
#include "../smart_home_cpu_debug.h"
#include "../smart_home_memory.h"
#include "../ui/smart_home_ui.h"
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
#include "../miloco/smart_home_miloco.h"
#endif
#ifdef CONFIG_SMART_HOME_APP_BRIDGE
#include "../addons/smart_home_app_bridge.h"
#endif
#if defined(CONFIG_SMART_HOME_VOICE_TTS) || defined(CONFIG_SMART_HOME_REMINDER_RING)
#include "../voice/smart_home_voice_play.h"
#endif
#if defined(CONFIG_SMART_HOME_VOICE_ASR) || defined(CONFIG_SMART_HOME_KWS)
#include "../voice/smart_home_voice_capture.h"
#endif
#ifdef CONFIG_SMART_HOME_DEMO_UI_LVGL
#include "../ui/lvgl/smart_home_lvgl.h"
#endif
#ifdef CONFIG_SMART_HOME_CAMERA_PREVIEW
#include "../camera/smart_home_camera_service.h"
#include "../camera/smart_home_camera_control.h"
#endif
#ifdef CONFIG_SMART_HOME_CAMERA_TOOL
#include "../tools/smart_home_camera_tools.h"
#endif

#ifdef CONFIG_SMART_HOME_REMINDERS
#include "../reminder/smart_home_reminder.h"
#endif

#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

static int smart_home_selftest_miloco_write(void)
{
    int ret = smart_home_secrets_set_miloco("127.0.0.1", 1, "selftest");

    printf("smart_home_selftest: miloco write %s (%d)\n",
           ret == AGENT_OK ? "PASS" : "FAIL", ret);
    return ret == AGENT_OK ? 0 : 1;
}

static int smart_home_selftest_miloco_read(void)
{
    char host[64] = "";
    char token[64] = "";
    uint16_t port = 0;
    int ret = smart_home_secrets_get_miloco(host, sizeof(host), &port,
                                            token, sizeof(token));
    int pass = ret == AGENT_OK && strcmp(host, "127.0.0.1") == 0 &&
               port == 1 && strcmp(token, "selftest") == 0;

    memset(token, 0, sizeof(token));
    printf("smart_home_selftest: miloco read %s host=%s port=%u\n",
           pass ? "PASS" : "FAIL", host, port);
    return pass ? 0 : 1;
}

#if defined(CONFIG_SMART_HOME_CAMERA_PREVIEW)
static int smart_home_selftest_camera(void)
{
    struct smart_home_camera_status_s status;
    int ret = smart_home_camera_start();

    if (ret < 0)
    {
        printf("smart_home_selftest: camera start failed %d\n", ret);
        return 1;
    }

    for (int i = 0; i < 50; i++)
    {
        if (smart_home_camera_get_status(&status) == 0 &&
            status.state == SMART_HOME_CAMERA_RUNNING &&
            status.preview_sequence > 0)
        {
            printf("smart_home_selftest: camera PASS preview=%" PRIu32
                   " capture=%" PRIu32 "\n",
                   status.preview_sequence, status.capture_sequence);
            smart_home_camera_stop();
            return 0;
        }

        usleep(200 * 1000);
    }

    smart_home_camera_get_status(&status);
    printf("smart_home_selftest: camera FAIL state=%d err=%d preview=%" PRIu32 "\n",
           status.state, status.last_error, status.preview_sequence);
    smart_home_camera_stop();
    return 1;
}
#endif

#if defined(CONFIG_SMART_HOME_CAMERA_TOOL)
static int smart_home_selftest_camera_tool(const char *action)
{
    return smart_home_camera_tool_selftest(action);
}
#endif

int main(int argc, char *argv[])
{
    char stack_marker;
    smart_home_agent_app_t app;
    smart_home_network_status_t network_status;
#ifndef CONFIG_SMART_HOME_DEMO_OFFLINE_UI
    int network_ret;
#endif
    int ret;

    /* Board self-tests run before network and agent initialization. */

    if (argc > 1 && strcmp(argv[1], "--selftest-miloco-write") == 0)
    {
        return smart_home_selftest_miloco_write();
    }

    if (argc > 1 && strcmp(argv[1], "--selftest-miloco-read") == 0)
    {
        return smart_home_selftest_miloco_read();
    }

#if defined(CONFIG_SMART_HOME_CAMERA_PREVIEW)
    if (argc > 1 && strcmp(argv[1], "--selftest-camera") == 0)
    {
        return smart_home_selftest_camera();
    }
#endif

#if defined(CONFIG_SMART_HOME_CAMERA_TOOL)
    if (argc > 2 && strcmp(argv[1], "--selftest-camera-tool") == 0)
    {
        return smart_home_selftest_camera_tool(argv[2]);
    }
#endif

    printf("=== Smart Home Agent Demo ===\n");
    if (argc > 1) {
        printf("input: %s\n\n", argv[1]);
    } else {
#if defined(CONFIG_SMART_HOME_DEMO_UI_LVGL)
        printf("starting LVGL UI\n\n");
#elif defined(CONFIG_SMART_HOME_DEMO_UI_QUICKAPP)
        printf("starting QuickApp native service\n\n");
#else
        printf("type 'quit' to exit\n\n");
#endif
    }

    /* Initialize the platform network before initializing the agent.
     * ESP32-S3 uses Wi-Fi (wlan0), while goldfish uses Ethernet (eth0).
     * Continue on failure so the UI can still show diagnostics.
     */

#ifdef CONFIG_SMART_HOME_DEMO_OFFLINE_UI
    smart_home_network_status_init(&network_status);
    network_status.init_status = SMART_HOME_NETWORK_STATUS_NA;
    network_status.ip_status = SMART_HOME_NETWORK_STATUS_NA;
    network_status.dns_status = SMART_HOME_NETWORK_STATUS_NA;
    printf("[smart_home_net] offline UI profile: network initialization skipped\n");
#else
#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_ESP_HOSTED
    {
        char ssid[33] = "";
        char password[65] = "";

        /* Runtime credentials take precedence over optional local board
         * Kconfig values.  Do not log either string. */
        if (smart_home_secrets_get_wifi_credentials(ssid, sizeof(ssid),
                                                    password,
                                                    sizeof(password)) == AGENT_OK) {
            network_ret = smart_home_network_connect_credentials(
                &network_status, ssid, password);
            memset(ssid, 0, sizeof(ssid));
            memset(password, 0, sizeof(password));
        } else {
            network_ret = smart_home_network_prepare_setup(&network_status);
        }
    }
#else
    network_ret = smart_home_network_init(&network_status);
#endif
    if (network_ret < 0) {
        syslog(LOG_WARNING, "Network init failed: %d. "
               "Continuing with existing network.\n", network_ret);
        fprintf(stderr,
                "Network init failed: %d (continuing)\n",
                network_ret);
    }
#endif
    smart_home_cpu_debug_log("main-network-ready");
    smart_home_memory_log_region("main-stack", &stack_marker);
    smart_home_memory_log_region("smart-home-app", &app);

    ret = smart_home_agent_app_init(&app);
    if (ret != AGENT_OK) {
        fprintf(stderr, "smart_home_agent_app_init failed: %d\n", ret);
        return EXIT_FAILURE;
    }
    smart_home_agent_app_set_network_status(&app, &network_status);

#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
    {
        extern smart_home_miloco_t *g_weather_miloco_service;
        smart_home_miloco_config_t miloco_config;

        /* 无条件启动 worker：天气拉取（wttr.in）不依赖米家网关；
         * 网关配置存在则一并轮询，否则网关端点静默失败。 */
        miloco_config.port = SMART_HOME_MILOCO_DEFAULT_PORT;
        miloco_config.token[0] = '\0';
        miloco_config.host[0] = '\0';
        if (smart_home_secrets_get_miloco(miloco_config.host,
                                          sizeof(miloco_config.host),
                                          &miloco_config.port,
                                          miloco_config.token,
                                          sizeof(miloco_config.token))
                != AGENT_OK) {
            /* secrets 无网关配置：host 留空，worker 只跑天气。 */
            miloco_config.host[0] = '\0';
        }
        if (smart_home_miloco_start(&app.miloco, &miloco_config) != AGENT_OK) {
            fprintf(stderr, "miloco worker start failed\n");
        }
        /* 全局桥接：weather_tool 经此指针读 wttr.in 快照。 */
        g_weather_miloco_service = app.miloco;
    }
#endif
#ifdef CONFIG_SMART_HOME_APP_BRIDGE
    ret = smart_home_app_bridge_start(&app.app_bridge, &app);
    if (ret != AGENT_OK) {
        fprintf(stderr, "smart_home app bridge failed: %d\n", ret);
        smart_home_agent_app_deinit(&app);
        return EXIT_FAILURE;
    }
#endif

#if defined(CONFIG_SMART_HOME_VOICE_TTS) || defined(CONFIG_SMART_HOME_REMINDER_RING)
    /* 播报服务与 UI 并行启动；初始化失败只损失语音播报，不阻断主流程。 */
    printf("[main] voice_play_init begin\n");
    if (voice_play_init() != 0) {
        fprintf(stderr, "voice_play_init failed; announcements disabled\n");
    }
    printf("[main] voice_play_init done\n");
#endif

#ifdef CONFIG_SMART_HOME_REMINDERS
    if (smart_home_reminder_start(NULL) != AGENT_OK) {
        fprintf(stderr,"reminder worker start failed; reminders unavailable\n");
    }
#endif

#if defined(CONFIG_SMART_HOME_DEMO_UI_QUICKAPP)
    if (argc > 1) {
        fprintf(stderr, "QuickApp mode ignores console input\n");
    }
    printf("SmartHome IPC service is ready; start the RPK with vapp separately.\n");
    for (;;) {
        pause();
    }
#elif defined(CONFIG_SMART_HOME_DEMO_UI_LVGL)
    smart_home_cpu_debug_log("main-before-ui");
    if (argc > 1) {
        ret = smart_home_ui_run_once(&app, argv[1]);
    } else {
        ret = smart_home_lvgl_run(&app, NULL);
    }
#else
    if (argc > 1) {
        ret = smart_home_ui_run_once(&app, argv[1]);
    } else {
        ret = smart_home_ui_run(&app);
    }
#endif

#ifdef CONFIG_SMART_HOME_APP_BRIDGE
    smart_home_app_bridge_stop(&app.app_bridge);
#endif
#ifdef CONFIG_SMART_HOME_REMINDERS
    smart_home_reminder_stop();
#endif
#if defined(CONFIG_SMART_HOME_VOICE_TTS) || defined(CONFIG_SMART_HOME_REMINDER_RING)
    voice_play_deinit();
#endif
#if defined(CONFIG_SMART_HOME_VOICE_ASR) || defined(CONFIG_SMART_HOME_KWS)
    /* 采集服务懒启动；退出时统一释放录音设备。 */
    voice_capture_deinit();
#endif
#ifdef CONFIG_SMART_HOME_CAMERA_PREVIEW
    (void)smart_home_camera_control_shutdown();
#endif
    smart_home_agent_app_deinit(&app);
    return ret == AGENT_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}
