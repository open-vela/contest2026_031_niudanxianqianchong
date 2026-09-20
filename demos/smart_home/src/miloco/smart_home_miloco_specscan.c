/****************************************************************************
 * demos/smart_home/src/miloco/smart_home_miloco_specscan.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 The NuttX Contributors
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "smart_home_miloco_specscan.h"

#include <stdlib.h>
#include <string.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 递归深度上限：本端点结构仅 5 层，16 层防御异常输入。 */
#define SPECSCAN_MAX_DEPTH 16

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 危险动作黑名单（由 spec 端点语义确定，与旧实现一致）。 */
static const char *const g_miloco_dangerous_types[] =
{
    "format",
    "pop-up",
    "restart-device",
};

/****************************************************************************
 * Private Functions: 极简 JSON 扫描原语
 ****************************************************************************/

static const char *scan_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
      {
        p++;
      }

    return p;
}

/* 解析带转义的 JSON 字符串。成功返回越过结尾引号的指针；结构不符
 * 返回 NULL。out 非 NULL 时写入截断到 cap-1 的内容（\uXXXX 转
 * UTF-8；非法转义按原样保留字符）。out 为 NULL 仅跳过。 */
static const char *scan_string(const char *p, char *out, size_t cap)
{
    size_t used = 0;

    if (*p != '"')
      {
        return NULL;
      }

    p++;
    while (*p != '"')
      {
        unsigned char ch = (unsigned char)*p;

        if (ch == '\0')
          {
            return NULL;
          }

        if (ch == '\\')
          {
            p++;
            switch (*p)
              {
              case '"':  ch = '"';  break;
              case '\\': ch = '\\'; break;
              case '/':  ch = '/';  break;
              case 'b':  ch = '\b'; break;
              case 'f':  ch = '\f'; break;
              case 'n':  ch = '\n'; break;
              case 'r':  ch = '\r'; break;
              case 't':  ch = '\t'; break;
              case 'u':
                {
                  unsigned int cp = 0;
                  int i;

                  for (i = 0; i < 4; i++)
                    {
                      p++;
                      cp <<= 4;
                      if (*p >= '0' && *p <= '9')
                        {
                          cp |= (unsigned int)(*p - '0');
                        }
                      else if (*p >= 'a' && *p <= 'f')
                        {
                          cp |= (unsigned int)(*p - 'a' + 10);
                        }
                      else if (*p >= 'A' && *p <= 'F')
                        {
                          cp |= (unsigned int)(*p - 'A' + 10);
                        }
                      else
                        {
                          return NULL;
                        }
                    }

                  /* BMP 内编码为 UTF-8；代理对区间以替代字符降级。 */
                  if (out != NULL)
                    {
                      char enc[4];
                      int n = 0;

                      if (cp < 0x80)
                        {
                          enc[n++] = (char)cp;
                        }
                      else if (cp < 0x800)
                        {
                          enc[n++] = (char)(0xC0 | (cp >> 6));
                          enc[n++] = (char)(0x80 | (cp & 0x3F));
                        }
                      else
                        {
                          enc[n++] = (char)(0xE0 | (cp >> 12));
                          enc[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                          enc[n++] = (char)(0x80 | (cp & 0x3F));
                        }

                      for (i = 0; i < n; i++)
                        {
                          if (out != NULL && used + 1 < cap)
                            {
                              out[used++] = enc[i];
                            }
                        }
                    }
                  p++;
                  continue;
                }
              default:
                  return NULL;
              }
          }
        else if (ch < 0x20)
          {
            return NULL; /* 控制字符必须转义 */
          }

        if (out != NULL && used + 1 < cap)
          {
            out[used++] = (char)ch;
          }

        p++;
      }

    if (out != NULL)
      {
        out[used] = '\0';
      }

    return p + 1;
}

/* 跳过任意 JSON 值（字符串/对象/数组/数字/true/false/null）。
 * 返回值结束后的指针；结构不符或超深返回 NULL。 */
static const char *scan_skip_value(const char *p, int depth)
{
    if (depth > SPECSCAN_MAX_DEPTH)
      {
        return NULL;
      }

    p = scan_ws(p);
    switch (*p)
      {
      case '"':
        return scan_string(p, NULL, 0);
      case '{':
      case '[':
        {
          char open = *p;
          char close = (open == '{') ? '}' : ']';

          p = scan_ws(p + 1);
          if (*p == close)
            {
              return p + 1;
            }

          while (true)
            {
              p = scan_ws(p);
              if (open == '{')
                {
                  if (*p != '"')
                    {
                      return NULL;
                    }

                  p = scan_string(p, NULL, 0);
                  if (p == NULL)
                    {
                      return NULL;
                    }

                  p = scan_ws(p);
                  if (*p != ':')
                    {
                      return NULL;
                    }

                  p++;
                }

              p = scan_skip_value(p, depth + 1);
              if (p == NULL)
                {
                  return NULL;
                }

              p = scan_ws(p);
              if (*p == ',')
                {
                  p++;
                  continue;
                }

              if (*p == close)
                {
                  return p + 1;
                }

              return NULL;
            }
        }
      default:
        /* 数字（含小数/科学计数，如 value_range 的 [0.0,270.0,90.0]）/
         * true / false / null：吃到结构分隔符为止。 */
        if (strncmp(p, "true", 4) == 0)
          {
            return p + 4;
          }

        if (strncmp(p, "false", 5) == 0)
          {
            return p + 5;
          }

        if (strncmp(p, "null", 4) == 0)
          {
            return p + 4;
          }

        if (*p == '-' || (*p >= '0' && *p <= '9'))
          {
            if (*p == '-')
              {
                p++;
              }

            while (*p >= '0' && *p <= '9')
              {
                p++;
              }

            if (*p == '.')
              {
                p++;
                while (*p >= '0' && *p <= '9')
                  {
                    p++;
                  }
              }

            if (*p == 'e' || *p == 'E')
              {
                p++;
                if (*p == '+' || *p == '-')
                  {
                    p++;
                  }

                while (*p >= '0' && *p <= '9')
                  {
                    p++;
                  }
              }

            return p;
          }

        return NULL;
      }
}

/* 在对象（p 已越过 '{'）的直接成员层找 key，返回其 value 起始指针。
 * 找不到返回 NULL（*found 置 0）；任何结构错误也返回 NULL。 */
static const char *scan_find_member(const char *obj_body, const char *key,
                                    bool *found)
{
    const char *p = scan_ws(obj_body);
    char name[32];

    *found = false;
    if (*p == '}')
      {
        return NULL;
      }

    while (true)
      {
        if (*p != '"')
          {
            return NULL;
          }

        p = scan_string(p, name, sizeof(name));
        if (p == NULL)
          {
            return NULL;
          }

        p = scan_ws(p);
        if (*p != ':')
          {
            return NULL;
          }

        p = scan_ws(p + 1);
        if (strcmp(name, key) == 0)
          {
            *found = true;
            return p;
          }

        p = scan_skip_value(p, 0);
        if (p == NULL)
          {
            return NULL;
          }

        p = scan_ws(p);
        if (*p == ',')
          {
            p = scan_ws(p + 1);
            continue;
          }

        return NULL; /* '}'（正常耗尽）或其他 */
      }
}

/****************************************************************************
 * Private Functions: 语义辅助（与旧 cJSON 实现逐条等价）
 ****************************************************************************/

static bool type_is_dangerous(const char *type_name)
{
    size_t i;

    if (!type_name)
      {
        return false;
      }

    for (i = 0; i < sizeof(g_miloco_dangerous_types) /
                    sizeof(g_miloco_dangerous_types[0]); i++)
      {
        if (strcmp(type_name, g_miloco_dangerous_types[i]) == 0)
          {
            return true;
          }
      }

    return false;
}

static smart_home_miloco_category_t category_from_name(const char *name)
{
    if (!name)
      {
        return SMART_HOME_MILOCO_CATEGORY_UNKNOWN;
      }

    if (strcmp(name, "light") == 0)
      {
        return SMART_HOME_MILOCO_CATEGORY_LIGHT;
      }

    if (strcmp(name, "air-conditioner") == 0 ||
        strcmp(name, "air-conditioner-outdoor") == 0)
      {
        return SMART_HOME_MILOCO_CATEGORY_AC;
      }

    if (strcmp(name, "outlet") == 0 || strcmp(name, "plug") == 0 ||
        strcmp(name, "switch") == 0)
      {
        return SMART_HOME_MILOCO_CATEGORY_OUTLET;
      }

    if (strcmp(name, "camera") == 0 || strcmp(name, "video-camera") == 0)
      {
        return SMART_HOME_MILOCO_CATEGORY_CAMERA;
      }

    if (strcmp(name, "fan") == 0 || strcmp(name, "ceiling-fan") == 0)
      {
        return SMART_HOME_MILOCO_CATEGORY_FAN;
      }

    return SMART_HOME_MILOCO_CATEGORY_OTHER;
}

/* 有界拷贝（对齐 snprintf "%s" 的截断语义）。 */
static void copy_bounded(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);

    if (n > cap - 1)
      {
        n = cap - 1;
      }

    memcpy(dst, src, n);
    dst[n] = '\0';
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int smart_home_miloco_specscan(const char *body,
                               char *did_out, size_t did_size,
                               smart_home_miloco_category_t *category_out,
                               smart_home_miloco_control_t *controls,
                               uint8_t max_controls, uint8_t *count_out,
                               bool *power_ctrl_out)
{
    const char *p;
    const char *v;
    bool found;
    char iid[24];
    char category_text[32];
    uint8_t count = 0;
    bool power_ctrl = false;

    *count_out = 0;
    *power_ctrl_out = false;
    *category_out = SMART_HOME_MILOCO_CATEGORY_UNKNOWN;
    did_out[0] = '\0';

    /* root.data */
    p = scan_ws(body);
    if (*p != '{')
      {
        return -1;
      }

    v = scan_find_member(p + 1, "data", &found);
    if (v == NULL || !found || *v != '{')
      {
        return -1;
      }

    /* data.did */
    v = scan_find_member(v + 1, "did", &found);
    if (v == NULL || !found || *v != '"')
      {
        return -1;
      }

    {
        char probe[32];
        const char *after = scan_string(v, probe, sizeof(probe));

        if (after == NULL || strlen(probe) >= did_size)
          {
            return -1;
          }

        strcpy(did_out, probe);
    }

    /* data.category（可选） */
    {
        const char *data_body = NULL;

        /* 重新从 data 对象起点找 category（find_member 是无副作用的
         * 纯扫描，可多次进入）。 */
        p = scan_ws(body);
        v = scan_find_member(p + 1, "data", &found);
        if (v != NULL && found)
          {
            data_body = v + 1;
          }

        category_text[0] = '\0';
        if (data_body != NULL)
          {
            v = scan_find_member(data_body, "category", &found);
            if (v != NULL && found && *v == '"')
              {
                scan_string(v, category_text, sizeof(category_text));
              }
          }
      }

    *category_out = category_from_name(category_text[0] ?
                                       category_text : NULL);

    /* data.spec 对象逐成员扫描。 */
    p = scan_ws(body);
    v = scan_find_member(p + 1, "data", &found);
    if (v == NULL || !found)
      {
        return -1;
      }

    v = scan_find_member(v + 1, "spec", &found);
    if (v == NULL || !found || *v != '{')
      {
        *count_out = count;
        *power_ctrl_out = power_ctrl;
        return 0; /* 无 spec：空控件，不算错误 */
      }

    p = scan_ws(v + 1);
    if (*p == '}')
      {
        *count_out = count;
        *power_ctrl_out = power_ctrl;
        return 0;
      }

    while (true)
      {
        const char *item;
        const char *member;
        char desc[64];
        char fmt[16];
        char type_name[32];
        bool writeable = false;
        smart_home_miloco_control_t *ctrl;

        if (*p != '"')
          {
            return -1;
          }

        p = scan_string(p, iid, sizeof(iid));
        if (p == NULL)
          {
            return -1;
          }

        p = scan_ws(p);
        if (*p != ':')
          {
            return -1;
          }

        item = scan_ws(p + 1);
        if (*item != '{')
          {
            p = scan_skip_value(item, 0);
            if (p == NULL)
              {
                return -1;
              }

            goto next_member;
          }

        /* prop.* 必须可写；action.* 视为可调用；其余前缀跳过。 */
        if (strncmp(iid, "prop.", 5) == 0)
          {
            member = scan_find_member(item + 1, "writeable", &found);
            writeable = member != NULL && found &&
                        strncmp(scan_ws(member), "true", 4) == 0;
            if (!writeable)
              {
                p = scan_skip_value(item, 0);
                if (p == NULL)
                  {
                    return -1;
                  }

                goto next_member;
              }
          }
        else if (strncmp(iid, "action.", 7) != 0)
          {
            p = scan_skip_value(item, 0);
            if (p == NULL)
              {
                return -1;
              }

            goto next_member;
          }

        /* 危险黑名单。 */
        member = scan_find_member(item + 1, "type_name", &found);
        type_name[0] = '\0';
        if (member != NULL && found && *member == '"')
          {
            scan_string(member, type_name, sizeof(type_name));
          }

        if (type_is_dangerous(type_name[0] ? type_name : NULL))
          {
            p = scan_skip_value(item, 0);
            if (p == NULL)
              {
                return -1;
              }

            goto next_member;
          }

        if (count >= max_controls)
          {
            p = scan_skip_value(item, 0);
            if (p == NULL)
              {
                return -1;
              }

            goto next_member;
          }

        ctrl = &controls[count];
        memset(ctrl, 0, sizeof(*ctrl));

        /* iid 截 13 字符（同旧实现 %.13s）。 */
        copy_bounded(ctrl->iid, 14, iid);
        if (strlen(iid) > 13)
          {
            ctrl->iid[13] = '\0';
          }

        member = scan_find_member(item + 1, "description", &found);
        desc[0] = '\0';
        if (member != NULL && found && *member == '"')
          {
            scan_string(member, desc, sizeof(desc));
          }

        copy_bounded(ctrl->desc, sizeof(ctrl->desc),
                     desc[0] ? desc : iid);
        ctrl->value = 0;

        if (strncmp(iid, "action.", 7) == 0)
          {
            ctrl->type = SMART_HOME_MILOCO_CTRL_ACTION;
            count++;
          }
        else
          {
            member = scan_find_member(item + 1, "format", &found);
            fmt[0] = '\0';
            if (member != NULL && found && *member == '"')
              {
                scan_string(member, fmt, sizeof(fmt));
              }

            if (strcmp(fmt, "bool") == 0)
              {
                ctrl->type = SMART_HOME_MILOCO_CTRL_BOOL;
                if (strcmp(iid, "prop.2.1") == 0)
                  {
                    power_ctrl = true;
                  }

                count++;
              }
            else
              {
                /* value_list → ENUM（最多 4 档，value 为整型）。 */
                member = scan_find_member(item + 1, "value_list", &found);
                if (member != NULL && found && *member == '[')
                  {
                    const char *opt = scan_ws(member + 1);
                    uint8_t opts = 0;

                    ctrl->type = SMART_HOME_MILOCO_CTRL_ENUM;
                    if (*opt != ']')
                      {
                        while (true)
                          {
                            const char *ov;
                            const char *elem;
                            char oname[24];

                            if (*opt != '{')
                              {
                                break;
                              }

                            ov = scan_find_member(opt + 1, "name", &found);
                            oname[0] = '\0';
                            if (ov != NULL && found && *ov == '"' &&
                                opts < SMART_HOME_MILOCO_MAX_OPTIONS)
                              {
                                scan_string(ov, oname, sizeof(oname));
                              }

                            elem = scan_find_member(opt + 1, "value",
                                                    &found);
                            if (ov != NULL && found && elem != NULL &&
                                found && oname[0] != '\0' &&
                                opts < SMART_HOME_MILOCO_MAX_OPTIONS)
                              {
                                ctrl->options[opts].value =
                                    (int32_t)strtol(elem, NULL, 10);
                                copy_bounded(
                                    ctrl->options[opts].name,
                                    sizeof(ctrl->options[opts].name),
                                    oname);
                                opts++;
                              }

                            opt = scan_skip_value(opt, 0);
                            if (opt == NULL)
                              {
                                return -1;
                              }

                            opt = scan_ws(opt);
                            if (*opt == ',')
                              {
                                opt = scan_ws(opt + 1);
                                continue;
                              }

                            break;
                          }
                      }

                    ctrl->option_count = opts;
                    if (opts > 0)
                      {
                        count++;
                      }
                  }
              }
          }

        p = scan_skip_value(item, 0);
        if (p == NULL)
          {
            return -1;
          }

next_member:
        p = scan_ws(p);
        if (*p == ',')
          {
            p = scan_ws(p + 1);
            continue;
          }

        if (*p == '}')
          {
            break;
          }

        return -1;
      }

    *count_out = count;
    *power_ctrl_out = power_ctrl;
    return 0;
}
