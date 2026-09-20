/****************************************************************************
 * demos/smart_home/src/miloco/smart_home_miloco_specscan.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 The NuttX Contributors
 *
 ****************************************************************************/

/* 设备 spec 响应的轻量解析（零堆分配）。
 *
 * 摄像机等复杂设备的 spec 达 17KB+，cJSON 整树解析意味着上千次
 * malloc/free 的小分配风暴（约 130KB 瞬时树，主要落在 SRAM 堆区，
 * 与 LVGL 运行时小分配竞争），曾在真机上引发分配器冻死。本模块
 * 对已知结构做一次线性扫描，全部输出写入调用方提供的固定缓冲。
 *
 * 支持且仅支持 /api/miot/devices/{did}/spec 的响应结构：
 *   {"code":0,"message":"..","data":{"did":"..","category":"..",
 *    "spec":{"prop.2.1":{"description":"..","format":"bool",
 *     "writeable":true,"type_name":"..","value_list":
 *     [{"name":"..","value":0},..]},..}}}
 *
 * 提取规则与旧 cJSON 实现逐条等价（含黑名单、截断、prop.2.1 判定）。
 * 纯 C、无 NuttX 依赖，可在宿主机对真实响应做单元测试。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "smart_home_miloco.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 解析 spec 响应。
 *
 * body:        NUL 终止的响应文本（HTTP 客户端已保证终止）
 * did_out:     输出 data.did（截断到 did_size-1）；超长视为解析失败
 * category_out:输出设备类别映射（缺失时 UNKNOWN）
 * controls:    输出控件数组（最多 max_controls 条，逐条截断规则同旧实现）
 * count_out:   输出控件数量
 * power_ctrl_out: 输出是否存在可写 bool 的 prop.2.1（电源开关）
 *
 * 返回 0 成功；-1 解析失败（结构不符/超深/越界）。
 */
int smart_home_miloco_specscan(const char *body,
                               char *did_out, size_t did_size,
                               smart_home_miloco_category_t *category_out,
                               smart_home_miloco_control_t *controls,
                               uint8_t max_controls, uint8_t *count_out,
                               bool *power_ctrl_out);

#ifdef __cplusplus
}
#endif
