/****************************************************************************
 * demos/smart_home/src/miloco/smart_home_miloco_scene.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 The NuttX Contributors
 *
 ****************************************************************************/

/* 场景模式在真实米家设备上的执行器。
 *
 * 场景动作以 spec 的 type_name 语义锚点描述（而非硬编码 iid），
 * 执行时在各在线设备已解析的 controls 里查找匹配项提交——设备
 * 型号不同 iid 可能不同，type_name 是米家 spec 的稳定语义键。
 * 未命中匹配控件的设备自然跳过，在报告中说明。
 */

#include "smart_home_miloco.h"

#include <stdio.h>
#include <string.h>

/****************************************************************************
 * Private Types
 ****************************************************************************/

typedef struct
{
    const char *type_name;   /* spec 语义锚点，如 "night-shot" */
    int32_t value;           /* BOOL:0/1；ENUM:options 档位值 */
    const char *label;       /* 报告用中文描述 */
} miloco_scene_action_t;

typedef struct
{
    const char *scene;       /* 场景标识：sleep/away/home/movie */
    const char *title;       /* 中文名 */
    miloco_scene_action_t actions[4];   /* NULL 结尾 */
} miloco_scene_def_t;

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 场景→真实设备动作映射。数值依据摄像机 spec 常见档位：
 * night-shot: 0=关 1=开 2=自动；BOOL 类 0/1。 */
static const miloco_scene_def_t g_miloco_scenes[] =
{
    {
        "sleep", "睡眠模式",
        {
            {"night-shot", 2, "夜视=自动"},
            {"human-tracking", 1, "人形追踪=开"},
            {NULL, 0, NULL},
        }
    },
    {
        "away", "离家模式",
        {
            {"on", 1, "摄像头=开"},
            {"night-shot", 2, "夜视=自动"},
            {"human-tracking", 1, "人形追踪=开"},
            {NULL, 0, NULL},
        }
    },
    {
        "home", "回家模式",
        {
            {"night-shot", 0, "夜视=关"},
            {"human-tracking", 1, "人形追踪=开"},
            {NULL, 0, NULL},
        }
    },
    {
        "movie", "观影模式",
        {
            {"on", 0, "摄像头=关"},
            {NULL, 0, NULL},
        }
    },
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int smart_home_miloco_run_scene(smart_home_miloco_t *service,
                                const char *scene,
                                char *report, size_t report_size)
{
    const miloco_scene_def_t *def = NULL;
    smart_home_miloco_device_t devices[SMART_HOME_MILOCO_MAX_DEVICES];
    size_t count;
    size_t used = 0;
    size_t i;
    int submitted = 0;
    int d;

    if (!service || !scene || !scene[0] || !report || report_size == 0)
      {
        return -1;
      }

    report[0] = '\0';
    for (i = 0; i < sizeof(g_miloco_scenes) / sizeof(g_miloco_scenes[0]);
         i++)
      {
        if (strcmp(g_miloco_scenes[i].scene, scene) == 0)
          {
            def = &g_miloco_scenes[i];
            break;
          }
      }

    if (def == NULL)
      {
        snprintf(report, report_size, "未知场景：%s", scene);
        return -1;
      }

    count = smart_home_miloco_list(service, devices,
                                   SMART_HOME_MILOCO_MAX_DEVICES, NULL);
    if (count == 0)
      {
        snprintf(report, report_size,
                 "%s：网关上没有可用设备", def->title);
        return -1;
      }

    used += (size_t)snprintf(report + used, report_size - used,
                             "%s：", def->title);
    for (i = 0; i < count && used < report_size - 96u; i++)
      {
        const smart_home_miloco_device_t *dev = &devices[i];
        int device_actions = 0;

        if (!dev->online || dev->control_count == 0)
          {
            continue;
          }

        for (d = 0; def->actions[d].type_name != NULL; d++)
          {
            const miloco_scene_action_t *act = &def->actions[d];
            uint8_t k;

            for (k = 0; k < dev->control_count; k++)
              {
                const smart_home_miloco_control_t *ctrl =
                    &dev->controls[k];

                if (ctrl->type_name[0] == '\0' ||
                    strcmp(ctrl->type_name, act->type_name) != 0)
                  {
                    continue;
                  }

                if (smart_home_miloco_submit_control(
                        service, dev->did, ctrl->iid, "set",
                        act->value) == AGENT_OK)
                  {
                    if (device_actions > 0 && used < report_size - 96u)
                      {
                        report[used++] = '、';
                      }

                    used += (size_t)snprintf(
                        report + used, report_size - used,
                        "%s %s(%.13s)", dev->name, act->label,
                        ctrl->iid);
                    device_actions++;
                    submitted++;
                  }

                break;   /* 同名控件取第一个 */
              }
          }
      }

    if (submitted == 0)
      {
        snprintf(report + used, report_size - used,
                 "网关在线但没有设备支持本场景的控件（需要摄像头类设备）");
        return -1;
      }

    return 0;
}
