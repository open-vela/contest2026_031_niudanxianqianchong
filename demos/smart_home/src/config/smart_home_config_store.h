/* SPDX-License-Identifier: Apache-2.0 */
/**
 * smart_home_config_store: 统一 JSON 配置存取模块。
 *
 * 为 /data/smart_home/ 下的持久化文件提供通用骨架：
 *   - 加载：读文件 → cJSON 解析 → 校验 version → 调 validate 回调
 *   - 失败按恢复策略处理：RECOVER_DEFAULT（用内存默认工厂）或 REJECT（禁用功能）
 *   - 保存：原子写（同目录 .tmp.<generation> + fsync + rename + fsync 目录）
 *
 * 设计约定（见 docs/design/config-store-plan.md）：
 *   - 纯应用层模块，不碰 cAGENT；依赖 cJSON（CONFIG_NETUTILS_CJSON）；
 *   - 默认值由各模块的内存默认工厂构造（从 g_llm_presets[] /
 *     smart_home_device_init() 等唯一来源），不以静态 JSON 重复表达业务默认；
 *   - 配置损坏时绝不覆盖原文件；
 *   - 运行期保存走"单一 config writer"，业务线程不直接调 store_save；
 *   - cJSON 所有权：load 成功返回堆 cJSON *，调用者 cJSON_Delete 释放。
 */

#pragma once

#include <stddef.h>

#include "cjson_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── 通用骨架 ── */

/** 校验回调：应用层解析 cJSON root，返回 AGENT_OK 或错误码。 */
typedef int (*smart_home_config_validate_fn)(const cJSON *root,
                                             void *user_data);

/** 默认工厂：构造调用方唯一的内存默认值，返回对象由调用者释放。 */
typedef cJSON *(*smart_home_config_default_fn)(void *user_data);

/** 加载失败时的恢复策略。 */
typedef enum {
    SMART_HOME_CONFIG_RECOVER_DEFAULT, /* 使用内存默认值，保留坏文件 */
    SMART_HOME_CONFIG_REJECT,          /* 不提供结果，由调用方禁用功能 */
} smart_home_config_recovery_t;

/**
 * 加载配置文件。
 *
 * 流程：读 path → cJSON 解析 → 校验 version（须为 1）→ 调 validate。
 * 失败（缺失/损坏/版本不符/校验失败）时按 recovery 决定：
 *   - RECOVER_DEFAULT：调 make_default 返回默认，返回 AGENT_OK；
 *   - REJECT：返回 NULL 与错误码，由调用方禁用功能。
 * 绝不覆盖原文件。
 *
 * @param path         配置文件路径
 * @param make_default 默认工厂（RECOVER_DEFAULT 时用），可为 NULL（则 REJECT）
 * @param recovery     恢复策略
 * @param validate     业务校验回调，可为 NULL（只做 version 检查）
 * @param user_data    透传 validate / make_default
 * @param out_root     成功时输出 cJSON *（堆分配），调用者 cJSON_Delete 释放
 * @param error        错误信息缓冲，可为 NULL
 * @param error_size   error 大小
 * @return AGENT_OK（含 RECOVER_DEFAULT 成功）；AGENT_ERROR_* 见实现
 */
int smart_home_config_store_load(const char *path,
                                 smart_home_config_default_fn make_default,
                                 smart_home_config_recovery_t recovery,
                                 smart_home_config_validate_fn validate,
                                 void *user_data,
                                 cJSON **out_root,
                                 char *error,
                                 size_t error_size);

/* Explicit caller-scoped legacy version. Other loaders still accept only v1.
 * Load does not overwrite the original document; next successful save uses v1.
 */
int smart_home_config_store_load_compatible(const char *path,
                                 int previous_version,
                                 smart_home_config_default_fn make_default,
                                 smart_home_config_recovery_t recovery,
                                 smart_home_config_validate_fn validate,
                                 void *user_data, cJSON **out_root,
                                 char *error, size_t error_size);

/**
 * 保存配置文件（原子写）。
 *
 * 流程：同目录创建 "<path>.tmp.<generation>" → 完整写入 + fsync →
 * rename → fsync 目录。任一步失败仅清理本次临时文件，保留原文件。
 * 注意：业务校验由薄封装 *save() 在调用前完成，本函数不做业务校验。
 */
int smart_home_config_store_save(const char *path, const cJSON *root);

/* ── 文件路径（与 config/README 一致） ── */

#ifndef CONFIG_SMART_HOME_DEMO_DATA_ROOT
#define CONFIG_SMART_HOME_DEMO_DATA_ROOT "/data"
#endif

#ifndef SMART_HOME_CONFIG_DIR
#define SMART_HOME_CONFIG_DIR CONFIG_SMART_HOME_DEMO_DATA_ROOT "/smart_home"
#endif

#ifndef CONFIG_SMART_HOME_MAX_BACKENDS
#define CONFIG_SMART_HOME_MAX_BACKENDS 8
#endif

#ifndef CONFIG_STORE_MAX_FILE_SIZE
#define CONFIG_STORE_MAX_FILE_SIZE (64 * 1024)
#endif

/* 临时文件路径缓冲（原路径 + ".tmp." + generation） */
#ifndef SMART_HOME_CONFIG_PATH_MAX
#define SMART_HOME_CONFIG_PATH_MAX 256
#endif

#ifndef SMART_HOME_CONFIG_BACKENDS_PATH
#define SMART_HOME_CONFIG_BACKENDS_PATH \
    SMART_HOME_CONFIG_DIR "/backends.json"
#endif
#ifndef SMART_HOME_CONFIG_SETTINGS_PATH
#define SMART_HOME_CONFIG_SETTINGS_PATH \
    SMART_HOME_CONFIG_DIR "/settings.json"
#endif
#ifndef SMART_HOME_CONFIG_STATE_PATH
#define SMART_HOME_CONFIG_STATE_PATH \
    SMART_HOME_CONFIG_DIR "/state.json"
#endif

/* secrets.json 不在本 store 常规机制内（config/README）：
 * 普通 store 不加载、回退或写回 secrets；由专用 secrets loader 消费，
 * 缺少密钥只能禁用关联 backend/provider，绝不回退到源码默认值。 */

/* ── 每类文件的薄封装（secrets 除外，见上） ── */

/* backends.json：LLM 后端预设库（可写回）。
 * 默认工厂从 g_llm_presets[] 构造；恢复策略 RECOVER_DEFAULT。 */
int smart_home_config_backends_load(cJSON **out_root,
                                    char *error,
                                    size_t error_size);
int smart_home_config_backends_save(const cJSON *root);

/* settings.json：当前选中的后端 ID（active_backend_id）。
 * 默认工厂返回 deepseek；恢复策略 RECOVER_DEFAULT。 */
int smart_home_config_settings_load(cJSON **out_root,
                                    char *error,
                                    size_t error_size);
int smart_home_config_settings_save(const cJSON *root);

/* state.json：本地设备状态 + 环境模拟值。
 * 默认工厂复用 smart_home_device_init() 的三设备默认；恢复策略 RECOVER_DEFAULT。 */
int smart_home_config_state_load(cJSON **out_root,
                                 char *error,
                                 size_t error_size);
int smart_home_config_state_save(const cJSON *root);

#ifdef __cplusplus
}
#endif
