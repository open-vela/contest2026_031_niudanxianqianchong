/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Miloco 米家网关接入（路径 C：家庭服务器上的 Xiaomi Miloco 后端）。
 *
 * P4 不直接对接米家云，而是通过局域网明文 HTTP 调用 Miloco 的
 * /miot REST API。设备列表轮询与控制请求都由独立 worker 线程执行，
 * LVGL 与 Agent 只接触本头文件导出的快照与提交接口。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SMART_HOME_MILOCO_HOST_SIZE 64
#define SMART_HOME_MILOCO_TOKEN_SIZE 64
#define SMART_HOME_MILOCO_MAX_DEVICES 16
#define SMART_HOME_MILOCO_MAX_CONTROLS 8
#define SMART_HOME_MILOCO_MAX_OPTIONS 4
#define SMART_HOME_MILOCO_POLL_INTERVAL_SEC 5

/* Miloco 默认监听端口（backend settings server.port）。 */
#define SMART_HOME_MILOCO_DEFAULT_PORT 1810

typedef enum {
    SMART_HOME_MILOCO_CATEGORY_UNKNOWN = 0,
    SMART_HOME_MILOCO_CATEGORY_LIGHT,
    SMART_HOME_MILOCO_CATEGORY_AC,
    SMART_HOME_MILOCO_CATEGORY_OUTLET,
    SMART_HOME_MILOCO_CATEGORY_CAMERA,
    SMART_HOME_MILOCO_CATEGORY_FAN,
    SMART_HOME_MILOCO_CATEGORY_OTHER,
} smart_home_miloco_category_t;

/* 控制类型：BOOL=开关，ENUM=多档（带选项），ACTION=无参动作。 */
typedef enum {
    SMART_HOME_MILOCO_CTRL_BOOL = 0,
    SMART_HOME_MILOCO_CTRL_ENUM,
    SMART_HOME_MILOCO_CTRL_ACTION,
} smart_home_miloco_ctrl_type_t;

/* 单个可控项：由设备 spec 的可写属性/动作解析而来（黑名单过滤），
 * 是统一控制工具与通用控制抽屉的共同数据源。 */
typedef struct {
    char iid[14];                        /* 如 "prop.2.3" / "action.2.1" */
    char desc[24];                       /* spec 中文描述 */
    char type_name[24];                  /* spec 语义锚点，如 "night-shot"
                                          * （场景模式按此匹配，跨型号稳定） */
    uint8_t type;                        /* smart_home_miloco_ctrl_type_t */
    int32_t value;                       /* BOOL:0/1 ENUM:当前值 ACTION:无效 */
    uint8_t option_count;
    struct {
        char name[12];
        int32_t value;
    } options[SMART_HOME_MILOCO_MAX_OPTIONS];
} smart_home_miloco_control_t;

typedef struct {
    char did[24];
    char name[40];
    char room[24];
    smart_home_miloco_category_t category;
    bool online;
    bool controllable;                   /* 存在 prop.2.1 控制项 */
    bool power_on;
    smart_home_miloco_control_t controls[SMART_HOME_MILOCO_MAX_CONTROLS];
    uint8_t control_count;
} smart_home_miloco_device_t;

typedef struct {
    char host[SMART_HOME_MILOCO_HOST_SIZE];
    uint16_t port;
    char token[SMART_HOME_MILOCO_TOKEN_SIZE];
} smart_home_miloco_config_t;

typedef struct smart_home_miloco smart_home_miloco_t;

/* 配置校验：host 非空、无控制字符且端口非零。 */
bool smart_home_miloco_config_valid(const smart_home_miloco_config_t *config);

/*
 * 启动网关服务：创建 worker 线程（PSRAM 栈）并立即开始轮询。
 * 网络未就绪时轮询失败并按周期重试，不需要显式重连。
 * *service 已存在时返回 AGENT_ERROR_INVALID，调用方应先 stop。
 */
int smart_home_miloco_start(smart_home_miloco_t **service,
                            const smart_home_miloco_config_t *config);

/* 原子切换网关目标配置：不销毁 worker 线程（无 join 阻塞），LVGL
 * 线程可安全调用；worker 在下一循环边界应用并立即轮询。 */
int smart_home_miloco_reconfigure(smart_home_miloco_t *service,
                                  const smart_home_miloco_config_t *config);

/* 提交保存请求：secrets.json 的文件写入由 worker 线程执行（LVGL/
 * 主线程上的 LittleFS 写入会挂死系统，见开发日志），worker 完成后
 * 应用配置并立即轮询。返回 AGENT_OK 仅表示请求已入队。 */
int smart_home_miloco_request_save(smart_home_miloco_t *service,
                                   const smart_home_miloco_config_t *config);

/* 停止 worker、等待退出并释放全部资源。容忍 NULL 与重复调用。 */
void smart_home_miloco_stop(smart_home_miloco_t **service);

/*
 * 拷贝当前设备快照（含在线态与电源态）。revision_out 非空时输出
 * 快照版本号：只有版本变化才需要刷新 UI。返回设备数量。
 */
size_t smart_home_miloco_list(const smart_home_miloco_t *service,
                              smart_home_miloco_device_t *devices,
                              size_t capacity,
                              uint32_t *revision_out);

/* 网关最近一次轮询是否成功；未启动返回 false。 */
bool smart_home_miloco_reachable(const smart_home_miloco_t *service);

/* 小米账号是否已在网关侧完成绑定（GET /api/miot/status 的 is_bound）。
 * 网关不可达或未启动时返回 false。 */
bool smart_home_miloco_bound(const smart_home_miloco_t *service);

/*
 * 提交一次电源控制（异步）：worker 醒来后 POST set_property。
 * 队列已满时返回 AGENT_ERROR_LIMIT；参数非法返回 AGENT_ERROR_INVALID。
 */
int smart_home_miloco_submit_power(smart_home_miloco_t *service,
                                   const char *did,
                                   bool on);

/* 统一控制提交：operation 取 "set"（set_property）或 "action"
 * （call_action）。iid 必须存在于该设备已解析的 controls 中（黑名单
 * 已在解析期过滤），否则 AGENT_ERROR_INVALID——这是统一工具的执行
 * 侧安全闸。 */
/* 场景模式在真实设备上执行（sleep/away/home/movie）。动作以
 * type_name 语义锚点在各在线设备的 controls 中匹配后提交。
 * 返回 0 成功；report 输出人类可读的执行报告。 */
int smart_home_miloco_run_scene(smart_home_miloco_t *service,
                                const char *scene,
                                char *report, size_t report_size);

int smart_home_miloco_submit_control(smart_home_miloco_t *service,
                                     const char *did,
                                     const char *iid,
                                     const char *operation,
                                     int32_t value);

/* ── 天气数据（wttr.in 文本格式，worker 每 10 分钟拉取） ────── */
typedef struct {
    char city[24];             /* 配置城市（英文/拼音），默认 Shenzhen */
    char condition_en[32];     /* "Clear" / "Light rain" 等 */
    char condition_cn[24];     /* "晴" / "小雨" 等中文映射 */
    int temperature;           /* °C */
    int humidity;              /* % */
    int wind_kmph;             /* km/h */
    bool valid;                /* 已成功获取至少一次 */
} smart_home_miloco_weather_t;

/* 读取当前天气快照（拷贝语义）。返回 false 表示尚未获取。 */
bool smart_home_miloco_get_weather(const smart_home_miloco_t *service,
                                   smart_home_miloco_weather_t *out);

/* 设置天气城市（拷贝语义，立即生效——下次轮询即用新城市）。 */
int smart_home_miloco_set_weather_city(smart_home_miloco_t *service,
                                       const char *city);

#ifdef __cplusplus
}
#endif
