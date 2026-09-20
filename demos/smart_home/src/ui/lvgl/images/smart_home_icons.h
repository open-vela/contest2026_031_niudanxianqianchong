/**
 * smart_home icon constants.
 *
 * Source: quickapp/smart_home_ui/prototype-web/assets/icons/（全部 PNG 文件）
 * Build:  scripts/generate_smart_home_lvgl_icons.py -> LVGL A8 C images
 *
 * Product icons use stable `asset:<png-stem>` names. The icon factory maps
 * them to linked LVGL A8 images; no LittleFS lookup is needed at runtime.
 * To update assets: replace the source PNG, run the generator, and rebuild.
 */

#pragma once

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Navigation bar ────────────────────────────────────────── */
#define ICON_NAV_HOME        "asset:home"
#define ICON_NAV_DEVICES     "asset:devices"
#define ICON_NAV_SCENES      "asset:sparkle-2"
#define ICON_NAV_SECURITY    "asset:shield"
#define ICON_NAV_MORE        "asset:dots"
#define ICON_NAV_CHAT        "asset:sparkle-2"
#define ICON_NAV_SETTINGS    "asset:settings"

/* ── Top bar status ───────────────────────────────────────── */
#define ICON_STATUS_MICROPHONE "asset:microphone"
#define ICON_STATUS_CAMERA     "asset:camera"
#define ICON_STATUS_DND        "asset:moon"
#define ICON_STATUS_WIFI       "asset:wifi"
#define ICON_STATUS_WIFI_OFF   "asset:wifi-off"

/* ── Device type indicators ────────────────────────────────── */
#define ICON_DEVICE_LIGHT    "asset:bulb"
#define ICON_DEVICE_AC       "asset:air-conditioning"
#define ICON_DEVICE_FAN      "asset:propeller"
#define ICON_DEVICE_GENERIC  "asset:devices"
#define ICON_MIJIA           "asset:MIJIA"

/* ── Room markers ──────────────────────────────────────────── */
#define ICON_ROOM_LIVING     "asset:sofa-single-outline"
#define ICON_ROOM_BEDROOM    "asset:bed-king-outline"

/* ── Action buttons ────────────────────────────────────────── */
#define ICON_ADD             "asset:plus"
#define ICON_DELETE          "icon_delete"
#define ICON_BACK            "icon_back"
#define ICON_CANCEL          "icon_cancel"
#define ICON_TOGGLE          "icon_toggle"

/* ── Status badges ─────────────────────────────────────────── */
#define ICON_STATUS_OK       "asset:home-shield"
#define ICON_STATUS_FAIL     "asset:cancel"
#define ICON_LOADING         "asset:loader"
#define ICON_TOOL            "asset:tool"       /* Agent 工具调用 */
#define ICON_SCENE_AWAY      "asset:run"        /* 离家模式 */
#define ICON_SCENE_SLEEP     "asset:zzz"        /* 睡眠模式 */
#define ICON_SERVICE_MCP     "asset:link"       /* MCP 服务/连接 */
#define ICON_MEDIA_AUDIO     "asset:music-circle-outline"
#define ICON_MEDIA_VIDEO     "asset:television-classic"
#define ICON_MEDIA_PREVIOUS  "asset:player-track-prev"
#define ICON_MEDIA_PLAY      "asset:player-play"
#define ICON_MEDIA_PAUSE     "asset:player-pause"
#define ICON_MEDIA_NEXT      "asset:player-track-next"

/* ── Sensor strip ──────────────────────────────────────────── */
#define ICON_TEMP            "asset:temperature"
#define ICON_HUMIDITY        "asset:cloud-rain"
#define ICON_SUN             "asset:sun"
#define ICON_DROP            "asset:cloud-rain"

/* ── Settings groups ───────────────────────────────────────── */
#define ICON_SETTING_AI      "asset:cloud-cog"
#define ICON_WIFI            "asset:wifi"
#define ICON_LANGUAGE        "icon_language"

#ifdef __cplusplus
}
#endif
