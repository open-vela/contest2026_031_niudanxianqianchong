/* SPDX-License-Identifier: Apache-2.0 */
/*
 * 米家扫码绑定弹窗（V9）。
 *
 * 设计基准：v9 layout-spec 的 34-mi-qr。相对旧整页版的改动：
 *   - 改为 840x480 弹窗叠在当前页面上，关闭即回原页，不再占整屏；
 *   - 二维码内容为网关地址 http://host:port/，与固件 qrcodegen 同源；
 *   - 顶部显示网关连接状态，底部状态条显示已等待秒数；
 *   - 「检查绑定状态」作为兜底主按钮，绑定成功后自动返回合并后的首页。
 *
 * 状态机（UI 线程只读快照，网络 IO 全部在 service worker）：
 *   WAITING  网关可达且未绑定 —— 等待手机完成授权
 *   OFFLINE  网关不可达       —— 网关离线，请检查服务器
 *   BOUND    is_bound=true    —— 已绑定 N 台设备，2 秒后返回首页
 */

#include "smart_home_lvgl_internal.h"
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE

#include "images/smart_home_icons.h"
#include "../../config/smart_home_secrets.h"
#include "../../miloco/smart_home_miloco.h"
#include "../../miloco/smart_home_miloco_qr.h"

#include <stdio.h>

/* 面板在 1024×600 画布上的位置，对齐 v9 设计 34-mi-qr 的 (92,60)。 */
#define BIND_MODAL_X 92
#define BIND_MODAL_Y 60
#define BIND_MODAL_W 840
#define BIND_MODAL_H 480
#define BIND_QR_CARD_PX 256
/* 静区（quiet zone）取 12px：25 模块 × 9px = 225px，占白卡 88%，
 * 既保证扫码识读又保留必要留白。 */
#define BIND_QR_MODULE_PAD 12
#define BIND_JUMP_DELAY_TICKS 2

static uint8_t s_qr_matrix[SMART_HOME_MILOCO_QR_MAX_MODULES *
                            SMART_HOME_MILOCO_QR_MAX_MODULES];
/* 弹窗内 QR 容器与已渲染 host：配置可能在弹窗打开后才就绪，
 * 由 bind_timer 惰性重绘。 */
static lv_obj_t *s_qr_card;
static char s_qr_drawn_host[64];
static uint32_t s_wait_ticks;

/* QR 渲染：白底卡片上按模块画深色块。一次性构建，不参与后续刷新。 */
static void bind_render_qr(lv_obj_t *parent, smart_home_lvgl_t *ui)
{
    smart_home_miloco_config_t qr_config;
    char url[96];
    char host[64];
    char token[64];
    uint16_t port = SMART_HOME_MILOCO_DEFAULT_PORT;
    int modules = 0;
    int cell;
    int origin;
    int x;
    int y;

    host[0] = '\0';
    token[0] = '\0';
    /* 幂等重绘：清掉旧 QR/占位再画。 */
    lv_obj_clean(parent);
    s_qr_drawn_host[0] = '\0';
    /* 运行态配置优先（挥发型模式下 secrets 为空）；回落 secrets。 */
    if (ui && ui->app && ui->app->miloco &&
        smart_home_miloco_get_config(ui->app->miloco, &qr_config)) {
        snprintf(host, sizeof(host), "%s", qr_config.host);
        port = qr_config.port;
    } else if (smart_home_secrets_get_miloco(host, sizeof(host), &port,
                                             token, sizeof(token))
                   != AGENT_OK ||
               !host[0]) {
        lv_obj_t *label = smart_home_lvgl_label_create(
            parent, "请先在米家网关设置中\n配置服务器地址",
            SMART_HOME_UI_COLOR_WARNING, 14);
        lv_obj_center(label);
        return;
    }
    snprintf(url, sizeof(url), "http://%s:%u/", host, (unsigned)port);
    snprintf(s_qr_drawn_host, sizeof(s_qr_drawn_host), "%s", host);

    if (smart_home_miloco_qr_encode(url, s_qr_matrix, &modules) != 0) {
        lv_obj_t *label = smart_home_lvgl_label_create(
            parent, "二维码生成失败", SMART_HOME_UI_COLOR_DANGER, 14);
        lv_obj_center(label);
        return;
    }

    cell = (BIND_QR_CARD_PX - 2 * BIND_QR_MODULE_PAD) / modules;
    if (cell < 2) {
        cell = 2;
    }
    origin = (BIND_QR_CARD_PX - modules * cell) / 2;
    for (y = 0; y < modules; y++) {
        for (x = 0; x < modules; x++) {
            lv_obj_t *dot;

            if (!s_qr_matrix[y * SMART_HOME_MILOCO_QR_MAX_MODULES + x]) {
                continue;
            }
            dot = lv_obj_create(parent);
            lv_obj_remove_style_all(dot);
            lv_obj_set_pos(dot, origin + x * cell, origin + y * cell);
            lv_obj_set_size(dot, cell, cell);
            lv_obj_set_style_bg_color(dot, SMART_HOME_UI_COLOR_TEXT_PRIMARY, 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        }
    }
}

static void bind_set_status(smart_home_lvgl_t *ui, const char *text,
                            lv_color_t color)
{
    if (ui->bind_status_label) {
        lv_label_set_text(ui->bind_status_label, text);
        lv_obj_set_style_text_color(ui->bind_status_label, color, 0);
    }
}

static void bind_refresh_head(smart_home_lvgl_t *ui)
{
    if (!ui || !ui->app || !ui->app->miloco || !ui->bind_head_label) {
        return;
    }
    if (smart_home_miloco_bound(ui->app->miloco)) {
        lv_label_set_text(ui->bind_head_label, "账号已绑定");
        if (ui->bind_head_dot) {
            lv_obj_set_style_bg_color(ui->bind_head_dot,
                                      SMART_HOME_UI_COLOR_SUCCESS, 0);
        }
    } else if (smart_home_miloco_reachable(ui->app->miloco)) {
        lv_label_set_text(ui->bind_head_label, "网关已连接");
        if (ui->bind_head_dot) {
            lv_obj_set_style_bg_color(ui->bind_head_dot,
                                      SMART_HOME_UI_COLOR_SUCCESS, 0);
        }
    } else {
        lv_label_set_text(ui->bind_head_label, "尚未连接");
        if (ui->bind_head_dot) {
            lv_obj_set_style_bg_color(ui->bind_head_dot,
                                      SMART_HOME_UI_COLOR_WARNING, 0);
        }
    }
}

static void bind_check_now(smart_home_lvgl_t *ui)
{
    if (!ui || !ui->app || !ui->app->miloco) {
        return;
    }
    bind_refresh_head(ui);
    if (smart_home_miloco_bound(ui->app->miloco)) {
        size_t count = smart_home_miloco_list(ui->app->miloco, NULL, 0, NULL);
        char text[64];

        snprintf(text, sizeof(text), "✓ 已绑定 · %u 台设备", (unsigned)count);
        bind_set_status(ui, text, SMART_HOME_UI_COLOR_SUCCESS);
    } else if (!smart_home_miloco_reachable(ui->app->miloco)) {
        bind_set_status(ui, "网关离线，请检查 Miloco 服务器",
                        SMART_HOME_UI_COLOR_WARNING);
    } else {
        bind_set_status(ui, "仍在等待手机完成授权…",
                        SMART_HOME_UI_COLOR_TEXT_SECONDARY);
    }
}

static void bind_timer_cb(lv_timer_t *timer)
{
    smart_home_lvgl_t *ui = lv_timer_get_user_data(timer);
    static int jump_countdown;
    char text[96];

    if (!ui || !ui->app || !ui->app->miloco || !ui->screen_miloco_bind) {
        jump_countdown = 0;
        return;
    }

    /* 惰性重绘：配置在弹窗构建后才就绪（挥发型保存）或已变化。 */
    {
        smart_home_miloco_config_t cfg;

        if (s_qr_card &&
            smart_home_miloco_get_config(ui->app->miloco, &cfg) &&
            strcmp(cfg.host, s_qr_drawn_host) != 0) {
            bind_render_qr(s_qr_card, ui);
        }
    }

    if (smart_home_miloco_bound(ui->app->miloco)) {
        if (jump_countdown == 0) {
            size_t count = smart_home_miloco_list(ui->app->miloco, NULL, 0,
                                                  NULL);
            snprintf(text, sizeof(text), "✓ 已绑定 · %u 台设备，正在进入…",
                     (unsigned)count);
            bind_set_status(ui, text, SMART_HOME_UI_COLOR_SUCCESS);
            jump_countdown = BIND_JUMP_DELAY_TICKS;
        } else if (--jump_countdown == 0) {
            smart_home_lvgl_miloco_bind_close(ui);
            smart_home_lvgl_load_tab(ui, SMART_HOME_TAB_HOME);
        }
        return;
    }
    jump_countdown = 0;
    bind_refresh_head(ui);
    s_wait_ticks++;
    if (!smart_home_miloco_reachable(ui->app->miloco)) {
        bind_set_status(ui, "网关离线，请检查 Miloco 服务器",
                        SMART_HOME_UI_COLOR_WARNING);
    } else {
        snprintf(text, sizeof(text),
                 "等待手机完成授权…  已等待 %02u:%02u  ·  每秒自动检查",
                 (unsigned)(s_wait_ticks / 60u),
                 (unsigned)(s_wait_ticks % 60u));
        bind_set_status(ui, text, SMART_HOME_UI_COLOR_TEXT_MUTED);
    }
}

static void bind_modal_close_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);

    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        smart_home_lvgl_miloco_bind_close(ui);
    }
}

static void bind_stop_cb(lv_event_t *event)
{
    lv_event_stop_bubbling(event);
}

static void bind_check_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);

    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        bind_check_now(ui);
    }
}

void smart_home_lvgl_miloco_bind_close(smart_home_lvgl_t *ui)
{
    if (!ui || !ui->screen_miloco_bind) {
        return;
    }
    if (ui->bind_timer) {
        lv_timer_delete(ui->bind_timer);
        ui->bind_timer = NULL;
    }
    lv_obj_delete(ui->screen_miloco_bind);
    ui->screen_miloco_bind = NULL;
    ui->bind_status_label = NULL;
    ui->bind_head_label = NULL;
    ui->bind_head_dot = NULL;
    ui->bind_check_button = NULL;
    s_qr_card = NULL;
    s_qr_drawn_host[0] = '\0';
    s_wait_ticks = 0;
}

int smart_home_lvgl_miloco_bind_is_open(const smart_home_lvgl_t *ui)
{
    return ui && ui->screen_miloco_bind;
}

void smart_home_lvgl_open_miloco_bind(smart_home_lvgl_t *ui)
{
    lv_obj_t *mask;
    lv_obj_t *panel;
    lv_obj_t *label;
    lv_obj_t *card;
    lv_obj_t *button;
    lv_obj_t *dot;

    if (!ui) {
        return;
    }
    smart_home_lvgl_miloco_bind_close(ui);

    mask = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(mask);
    lv_obj_set_size(mask, smart_home_lvgl_disp_w(), smart_home_lvgl_disp_h());
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x071726), 0);
    lv_obj_set_style_bg_opa(mask, 148, 0);   /* 58%，对齐 34-mi-qr */
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(mask, bind_modal_close_cb, LV_EVENT_CLICKED, ui);
    ui->screen_miloco_bind = mask;

    /* 面板左上对齐设计的 (92,60)；内部一律用设计给出的页面绝对坐标
     * 减去该原点，因此不能加 pad —— 否则所有子元素整体偏移。
     */
    panel = lv_obj_create(mask);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, BIND_MODAL_W, BIND_MODAL_H);
    lv_obj_set_pos(panel, BIND_MODAL_X, BIND_MODAL_Y);
    smart_home_lvgl_card_style(panel);
    lv_obj_set_style_radius(panel, 24, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(panel, bind_stop_cb, LV_EVENT_CLICKED, NULL);

    /* 标题 (116,102) 24px。 */
    label = smart_home_lvgl_label_create(panel, "绑定米家账号",
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY, 24);
    lv_obj_set_pos(label, 24, 22);

    /* 右上关闭 chip (868,76) 48×48，圆角 16。 */
    button = lv_button_create(panel);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, 48, 48);
    lv_obj_set_pos(button, 776, 16);
    lv_obj_set_style_radius(button, 16, 0);
    smart_home_lvgl_set_bg(button, lv_color_hex(0xE2E9EC));
    smart_home_lvgl_press_feedback(button);
    lv_obj_add_event_cb(button, bind_modal_close_cb, LV_EVENT_CLICKED, ui);
    label = smart_home_lvgl_label_create(button, "X",
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY, 20);
    lv_obj_center(label);

    /* 网关状态：绿点 (432,175) r=3 + 14px 文字 (446,166)。 */
    dot = lv_obj_create(panel);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 6, 6);
    lv_obj_set_pos(dot, 337, 112);
    lv_obj_set_style_radius(dot, 3, 0);
    smart_home_lvgl_set_bg(dot, SMART_HOME_UI_COLOR_SUCCESS);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    ui->bind_head_dot = dot;

    ui->bind_head_label = smart_home_lvgl_label_create(
        panel, "尚未连接", SMART_HOME_UI_COLOR_TEXT_SECONDARY, 14);
    lv_obj_set_pos(ui->bind_head_label, 354, 106);

    /* 左：QR 白卡 (124,160) 256×256，圆角 12。 */
    card = lv_obj_create(panel);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, BIND_QR_CARD_PX, BIND_QR_CARD_PX);
    lv_obj_set_pos(card, 32, 100);
    lv_obj_set_style_radius(card, 12, 0);
    smart_home_lvgl_set_bg(card, lv_color_white());
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    s_qr_card = card;
    bind_render_qr(card, ui);

    /* 米家二维码标注：与 QR 白卡同宽同左边界，水平居中于卡片下方。
     * 白卡在面板内位于 (32,100) 宽 256，标注取 x=32 宽 256 即居中。
     */
    label = smart_home_lvgl_label_create(panel, "米家二维码",
                                         SMART_HOME_UI_COLOR_TEXT_MUTED, 13);
    lv_obj_set_pos(label, 32, 368);
    lv_obj_set_width(label, BIND_QR_CARD_PX);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);

    /* 右侧：标题 (420,236) 24px。 */
    label = smart_home_lvgl_label_create(panel, "等待手机完成授权",
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY, 24);
    lv_obj_set_pos(label, 328, 153);

    /* 三步说明 (420,280/318/356) 16px，行内编号，不用圆形 chip。 */
    {
        static const char *const steps[3] = {
            "1. 手机扫描二维码进入家庭服务器",
            "2. 在手机页面完成账号授权",
            "3. 授权成功后，此处同步设备",
        };

        for (int i = 0; i < 3; i++) {
            label = smart_home_lvgl_label_create(
                panel, steps[i], SMART_HOME_UI_COLOR_TEXT_SECONDARY, 16);
            lv_obj_set_pos(label, 328, 204 + i * 38);
        }
    }

    /* 实时状态（设计里没有，实现层补：每秒轮询需要可见反馈）。
     * 放在第三行之后、提示语之前的空档 (420,380)，不挤占设计位置。
     */
    ui->bind_status_label = smart_home_lvgl_label_create(
        panel, "等待手机完成授权…  已等待 00:00",
        SMART_HOME_UI_COLOR_TEXT_MUTED, 13);
    lv_obj_set_pos(ui->bind_status_label, 328, 314);

    /* 提示语 (420,408) 14px。 */
    label = smart_home_lvgl_label_create(
        panel, "请在手机上完成账号授权，再检查绑定状态。",
        SMART_HOME_UI_COLOR_TEXT_MUTED, 14);
    lv_obj_set_pos(label, 328, 336);

    /* 底部左：返回配置 (116,464) 168×48，描边按钮。 */
    button = lv_button_create(panel);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, 168, 48);
    lv_obj_set_pos(button, 24, 404);
    lv_obj_set_style_radius(button, 12, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, SMART_HOME_UI_COLOR_BORDER, 0);
    smart_home_lvgl_set_bg(button, SMART_HOME_UI_COLOR_SURFACE);
    smart_home_lvgl_press_feedback(button);
    lv_obj_add_event_cb(button, bind_modal_close_cb, LV_EVENT_CLICKED, ui);
    label = smart_home_lvgl_label_create(button, "返回配置",
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY, 18);
    lv_obj_center(label);

    /* 底部右：检查绑定状态 (652,464) 256×48，深色实底。 */
    button = lv_button_create(panel);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, 256, 48);
    lv_obj_set_pos(button, 560, 404);
    lv_obj_set_style_radius(button, 12, 0);
    smart_home_lvgl_set_bg(button, SMART_HOME_UI_COLOR_PRIMARY_DARK);
    smart_home_lvgl_press_feedback(button);
    lv_obj_add_event_cb(button, bind_check_cb, LV_EVENT_CLICKED, ui);
    ui->bind_check_button = button;
    label = smart_home_lvgl_label_create(button, "检查绑定状态",
                                         SMART_HOME_UI_COLOR_SURFACE, 18);
    lv_obj_center(label);

    if (ui->bind_timer) {
        lv_timer_delete(ui->bind_timer);
    }
    ui->bind_timer = lv_timer_create(bind_timer_cb, 1000, ui);

    /* 打开即刷新一次头部状态，避免首秒显示未连接。 */
    bind_refresh_head(ui);
}

#endif /* CONFIG_SMART_HOME_MILOCO_BRIDGE */
