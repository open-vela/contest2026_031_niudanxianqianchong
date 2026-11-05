/* SPDX-License-Identifier: Apache-2.0 */
/**
 * 首次配置引导（onboarding）。
 *
 * 三个配置项（家庭网络 / 米家网关 / 模型服务）并列而非串联：
 *   - 只做总览浮层，不强制线性向导；
 *   - 每一项点击后跳到设置页对应分类，复用已有面板，不重复实现表单；
 *   - 始终提供「稍后再说」，保证演示场景能直接看首页。
 *
 * 跳过状态只保存在运行期内存里，不在 LVGL 主线程执行文件写入。
 * 每次开机重新评估一次即可，代价只是重启后再提示一次。
 */

#pragma once

#include "smart_home_lvgl.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 三项配置的完成状态，供首页状态点与总览浮层共用。 */
typedef struct {
    bool wifi_ready;    /* 已联网 */
    bool miloco_ready;  /* 网关可达且已绑定 */
    bool model_ready;   /* 模型配置已成功应用 */
    int pending;        /* 三项中未完成的个数 */
} smart_home_onboarding_status_t;

/* 读取三项配置状态。ui 可为 NULL，此时全部按未完成处理。 */
void smart_home_lvgl_onboarding_status(const smart_home_lvgl_t *ui,
                                       smart_home_onboarding_status_t *out);

/* 是否应该弹出总览：存在未完成项，且本次运行尚未跳过。 */
bool smart_home_lvgl_onboarding_should_present(const smart_home_lvgl_t *ui);

/* 弹出总览浮层。已弹出过或全部就绪时不做任何事。 */
void smart_home_lvgl_onboarding_present(smart_home_lvgl_t *ui);

/* 关闭总览。skipped 为 true 时记录「本次运行不再提示」。 */
void smart_home_lvgl_onboarding_dismiss(smart_home_lvgl_t *ui, bool skipped);

/* 总览是否正在显示。 */
bool smart_home_lvgl_onboarding_visible(void);

/* 运行期重置（测试与重新开机用）。 */
void smart_home_lvgl_onboarding_reset(void);

#ifdef __cplusplus
}
#endif
