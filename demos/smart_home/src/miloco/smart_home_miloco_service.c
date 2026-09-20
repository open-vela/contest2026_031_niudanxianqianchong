/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Miloco 网关服务：独立 worker 轮询设备列表并执行电源控制。
 *
 * 数据流：
 *   GET /miot/device_list           → 设备 did/name/room/online（小 JSON）
 *   GET /miot/devices/{did}/spec    → 新设备一次性取 category（小 JSON）
 *   GET /miot/devices/{did}/status?iid=prop.2.1 → 电源态回读
 *   POST /miot/devices/{did}/control → set_property 开关
 *
 * 响应统一为 {code, message, data}；code==0 才视为成功。
 */

#include "smart_home_miloco.h"
#include "smart_home_miloco_client.h"
#include "smart_home_miloco_specscan.h"

#include "../smart_home_memory.h"
#include "../config/cjson_compat.h"
#include "../config/smart_home_secrets.h"

#include <cagent/types.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <nuttx/irq.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

/* 24 KiB：HTTP 路径（getaddrinfo + 非阻塞 connect + 收包解析）实测
 * 栈深，8 KiB 时溢出会踩坏相邻 PSRAM，症状延迟到后续堆操作（cJSON）
 * 才爆——分段日志定位的教训，见开发日志。 */
#define MILOCO_WORKER_STACK_SIZE   24576u
#define MILOCO_CONTROL_QUEUE_DEPTH 8
/* 摄像机等复杂设备的 spec 达 17KB+（实测 17426B），16KB 缓冲会
 * 截断导致解析失败；32KB 走 PSRAM bulk 堆无压力。 */
#define MILOCO_RESPONSE_BYTES      (32u * 1024u)
#define MILOCO_STATUS_RESPONSE_BYTES 512u

typedef struct {
    char did[sizeof(((smart_home_miloco_device_t *)0)->did)];
    char iid[14];
    uint8_t is_action;                   /* 0=set_property 1=call_action */
    int32_t value;
} miloco_control_request_t;

struct smart_home_miloco {
    smart_home_miloco_client_config_t client_config;
    pthread_t worker;
    bool worker_started;
    bool stop_requested;
    /* worker 栈（PSRAM）：保存指针以便 stop 时释放，避免泄漏。 */
    void *worker_stack;
    size_t worker_stack_bytes;
    /* reconfigure 待生效配置：worker 在循环顶部安全切换，UI 线程
     * 永不 join、永不重建线程。 */
    smart_home_miloco_client_config_t pending_config;
    bool config_dirty;
    /* 天气数据与拉取节奏控制。 */
    smart_home_miloco_weather_t weather;
    uint32_t weather_poll_count;    /* 5s/轮，120 轮 = 10 分钟 */
    /* 保存请求：worker 代写 secrets.json 后应用配置。 */
    smart_home_miloco_config_t save_config;
    bool save_pending;

    pthread_mutex_t lock;
    sem_t wake;

    /* 以下字段由 lock 保护。 */
    miloco_control_request_t pending[MILOCO_CONTROL_QUEUE_DEPTH];
    size_t pending_count;
    smart_home_miloco_device_t devices[SMART_HOME_MILOCO_MAX_DEVICES];
    size_t device_count;
    uint32_t revision;
    bool reachable;
    bool xiaomi_bound;
};

static void lock_state(smart_home_miloco_t *service)
{
    pthread_mutex_lock(&service->lock);
}

static void unlock_state(smart_home_miloco_t *service)
{
    pthread_mutex_unlock(&service->lock);
}

bool smart_home_miloco_config_valid(const smart_home_miloco_config_t *config)
{
    const unsigned char *p;

    if (!config || config->port == 0) {
        return false;
    }
    /* 空 host 合法：worker 以"仅天气"模式运行，网关端点静默失败。 */
    if (config->host[0] == '\0') {
        return true;
    }
    for (p = (const unsigned char *)config->host; *p; p++) {
        if (*p < 0x20u || *p == 0x7fu) {
            return false;
        }
    }
    return true;
}

size_t smart_home_miloco_list(const smart_home_miloco_t *service,
                              smart_home_miloco_device_t *devices,
                              size_t capacity,
                              uint32_t *revision_out)
{
    size_t count;

    if (!service) {
        if (revision_out) {
            *revision_out = 0;
        }
        return 0;
    }
    lock_state((smart_home_miloco_t *)service);
    count = service->device_count;
    if (devices && capacity > 0) {
        if (count > capacity) {
            count = capacity;
        }
        memcpy(devices, service->devices,
               count * sizeof(devices[0]));
    }
    if (revision_out) {
        *revision_out = service->revision;
    }
    unlock_state((smart_home_miloco_t *)service);
    return count;
}

bool smart_home_miloco_get_config(const smart_home_miloco_t *service,
                                  smart_home_miloco_config_t *out)
{
    bool ok = false;

    if (service && out) {
        lock_state((smart_home_miloco_t *)service);
        snprintf(out->host, sizeof(out->host), "%s",
                 service->client_config.host);
        out->port = service->client_config.port;
        snprintf(out->token, sizeof(out->token), "%s",
                 service->client_config.token);
        ok = service->reachable || service->config_dirty ||
             service->client_config.host[0] != '\0';
        unlock_state((smart_home_miloco_t *)service);
    }
    return ok;
}

bool smart_home_miloco_bound(const smart_home_miloco_t *service)
{
    bool bound = false;

    if (service) {
        lock_state((smart_home_miloco_t *)service);
        bound = service->reachable && service->xiaomi_bound;
        unlock_state((smart_home_miloco_t *)service);
    }
    return bound;
}

bool smart_home_miloco_reachable(const smart_home_miloco_t *service)
{
    bool reachable = false;

    if (service) {
        lock_state((smart_home_miloco_t *)service);
        reachable = service->reachable;
        unlock_state((smart_home_miloco_t *)service);
    }
    return reachable;
}

/* 危险动作黑名单与类别映射已迁入 smart_home_miloco_specscan.c
 * （随轻量解析整体迁移，仅 spec 端点使用）。 */

int smart_home_miloco_submit_control(smart_home_miloco_t *service,
                                     const char *did,
                                     const char *iid,
                                     const char *operation,
                                     int32_t value)
{
    int ret = AGENT_OK;

    if (!service || !did || !did[0] || !iid || !iid[0] ||
        strlen(did) >= sizeof(service->pending[0].did) ||
        strlen(iid) >= sizeof(service->pending[0].iid) || !operation) {
        return AGENT_ERROR_INVALID;
    }
    lock_state(service);
    if (service->stop_requested) {
        ret = AGENT_ERROR_INVALID;
    } else if (service->pending_count >= MILOCO_CONTROL_QUEUE_DEPTH) {
        ret = AGENT_ERROR_LIMIT;
    } else {
        miloco_control_request_t *slot =
            &service->pending[service->pending_count];

        strcpy(slot->did, did);
        strcpy(slot->iid, iid);
        slot->is_action = strcmp(operation, "action") == 0;
        slot->value = value;
        service->pending_count++;
    }
    unlock_state(service);
    if (ret == AGENT_OK) {
        sem_post(&service->wake);
    }
    return ret;
}

int smart_home_miloco_submit_power(smart_home_miloco_t *service,
                                   const char *did,
                                   bool on)
{
    return smart_home_miloco_submit_control(service, did, "prop.2.1",
                                            "set", on ? 1 : 0);
}

/*
 * 原子切换网关目标配置：worker 在下一个循环边界应用并立即轮询。
 * 与 stop+start 的区别：不销毁线程（无 join 阻塞、无栈重分配），
 * LVGL 线程调用安全。
 */
int smart_home_miloco_reconfigure(smart_home_miloco_t *service,
                                  const smart_home_miloco_config_t *config)
{
    smart_home_miloco_client_config_t client_config;
    int ret = AGENT_OK;

    if (!service || !smart_home_miloco_config_valid(config)) {
        return AGENT_ERROR_INVALID;
    }
    snprintf(client_config.host, sizeof(client_config.host), "%s",
             config->host);
    client_config.port = config->port;
    snprintf(client_config.token, sizeof(client_config.token), "%s",
             config->token);

    lock_state(service);
    if (service->stop_requested) {
        ret = AGENT_ERROR_INVALID;
    } else {
        service->pending_config = client_config;
        service->config_dirty = true;
    }
    unlock_state(service);
    if (ret == AGENT_OK) {
        sem_post(&service->wake);
    }
    return ret;
}

int smart_home_miloco_request_save(smart_home_miloco_t *service,
                                   const smart_home_miloco_config_t *config)
{
    int ret = AGENT_OK;

    if (!service || !smart_home_miloco_config_valid(config)) {
        return AGENT_ERROR_INVALID;
    }
    lock_state(service);
    if (service->stop_requested) {
        ret = AGENT_ERROR_INVALID;
    } else {
        service->save_config = *config;
        service->save_pending = true;
    }
    unlock_state(service);
    if (ret == AGENT_OK) {
        sem_post(&service->wake);
    }
    return ret;
}

/* ── JSON 解析 ─────────────────────────────────────────────── */

static void copy_text(char *dst, size_t dst_size, const cJSON *value)
{
    const char *text = cJSON_IsString(value) ? value->valuestring : "";
    size_t n = strlen(text);

    if (n > dst_size - 1)
      {
        /* UTF-8 字符边界回退：字节截断会产生非法序列（同
         * specscan copy_bounded 的教训——半个汉字曾让 agent 请求
         * 被 LLM 服务端以 invalid unicode 拒绝）。 */
        n = dst_size - 1;
        while (n > 0 && ((unsigned char)text[n] & 0xC0) == 0x80)
          {
            n--;
          }
      }

    memcpy(dst, text, n);
    dst[n] = '\0';
}

/* 解析 data: [{did,name,online,room_name,...}, ...]（device_list 响应）。 */
static int parse_device_list(smart_home_miloco_t *service, const char *body,
                             smart_home_miloco_device_t *fresh_categories,
                             size_t fresh_capacity,
                             size_t *fresh_count)
{
    cJSON *root = cJSON_Parse(body);
    cJSON *data;
    cJSON *item;
    smart_home_miloco_device_t parsed[SMART_HOME_MILOCO_MAX_DEVICES];
    size_t count = 0;

    *fresh_count = 0;
    if (!root) {
        return AGENT_ERROR_PARSE;
    }
    data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsArray(data)) {
        cJSON_Delete(root);
        return AGENT_ERROR_PARSE;
    }

    cJSON_ArrayForEach(item, data) {
        const cJSON *did = cJSON_GetObjectItemCaseSensitive(item, "did");
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
        const cJSON *online = cJSON_GetObjectItemCaseSensitive(item, "online");
        const cJSON *room = cJSON_GetObjectItemCaseSensitive(item, "room_name");
        smart_home_miloco_device_t *slot;

        if (!cJSON_IsString(did) || !did->valuestring[0] ||
            count >= SMART_HOME_MILOCO_MAX_DEVICES) {
            continue;
        }
        slot = &parsed[count++];
        memset(slot, 0, sizeof(*slot));
        copy_text(slot->did, sizeof(slot->did), did);
        copy_text(slot->name, sizeof(slot->name), name);
        copy_text(slot->room, sizeof(slot->room), room);
        slot->online = cJSON_IsTrue(online);
    }
    cJSON_Delete(root);

    /* 保留旧条目的 category/controllable/power_on；新 did 记入 fresh 列表
     * 供 worker 后续拉取 spec。 */
    {
        size_t i;
        size_t j;

        for (i = 0; i < count; i++) {
            const smart_home_miloco_device_t *old = NULL;

            for (j = 0; j < service->device_count; j++) {
                if (strcmp(service->devices[j].did, parsed[i].did) == 0) {
                    old = &service->devices[j];
                    break;
                }
            }
            if (old) {
                parsed[i].category = old->category;
                parsed[i].controllable = old->controllable;
                parsed[i].power_on = old->power_on;
                parsed[i].control_count = old->control_count;
                memcpy(parsed[i].controls, old->controls,
                       sizeof(parsed[i].controls));
            } else if (fresh_categories && *fresh_count < fresh_capacity) {
                strcpy(fresh_categories[*fresh_count].did, parsed[i].did);
                (*fresh_count)++;
            }
        }
    }

    /* 真变才 revision++：列表内容不变时（绝大多数轮询）不打扰 UI——
     * 原先每 5 秒无条件 bump，触发全网格 lv_obj_clean 重建（白闪、
     * 滚动位置重置）并配合 UI 刷新顺序杀掉打开中的控制抽屉。
     * controls 内容与状态值的变化由 parse_device_spec /
     * refresh_device_status 各自 bump，此处只比列表级字段。 */
    {
        size_t i;
        int changed = service->device_count != count || !service->reachable;

        if (!changed)
          {
            for (i = 0; i < count; i++)
              {
                const smart_home_miloco_device_t *old =
                    &service->devices[i];

                if (strcmp(old->did, parsed[i].did) != 0 ||
                    strcmp(old->name, parsed[i].name) != 0 ||
                    strcmp(old->room, parsed[i].room) != 0 ||
                    old->online != parsed[i].online ||
                    old->category != parsed[i].category ||
                    old->controllable != parsed[i].controllable ||
                    old->power_on != parsed[i].power_on ||
                    old->control_count != parsed[i].control_count)
                  {
                    changed = 1;
                    break;
                  }
              }
          }

        lock_state(service);
        memcpy(service->devices, parsed, count * sizeof(parsed[0]));
        service->device_count = count;
        service->reachable = true;
        if (changed)
          {
            service->revision++;
          }
        unlock_state(service);
    }
    return AGENT_OK;
}

/* 解析 data: {did, name, category, spec:{iid:{...}}}。轻量扫描见
 * smart_home_miloco_specscan.c——17KB 摄像机 spec 的 cJSON 整树解析
 * （上千次 malloc/free 风暴，约 130KB 瞬时树落 SRAM 堆区）曾在真机
 * 引发分配器冻死；现为零堆分配的线性扫描，本函数只剩结果应用。 */
static int parse_device_spec(smart_home_miloco_t *service, const char *body)
{
    smart_home_miloco_category_t mapped;
    char target_did[24];
    size_t dev_index = SMART_HOME_MILOCO_MAX_DEVICES;
    smart_home_miloco_control_t parsed[SMART_HOME_MILOCO_MAX_CONTROLS];
    uint8_t parsed_count = 0;
    bool power_ctrl = false;
    size_t i;

    if (smart_home_miloco_specscan(body, target_did, sizeof(target_did),
                                   &mapped, parsed,
                                   SMART_HOME_MILOCO_MAX_CONTROLS,
                                   &parsed_count, &power_ctrl) != 0)
      {
        return AGENT_ERROR_PARSE;
      }

    lock_state(service);
    for (i = 0; i < service->device_count; i++) {
        if (strcmp(service->devices[i].did, target_did) == 0) {
            dev_index = i;
            break;
        }
    }
    if (dev_index != SMART_HOME_MILOCO_MAX_DEVICES) {
        smart_home_miloco_device_t *dev = &service->devices[dev_index];

        dev->category = mapped;
        memcpy(dev->controls, parsed, parsed_count * sizeof(parsed[0]));
        dev->control_count = parsed_count;
        dev->controllable = power_ctrl;
        service->revision++;
    }
    unlock_state(service);
    return AGENT_OK;
}

/* 解析 data: [{iid, value}, ...]，按设备 controls 的 prop 项回读。 */
static int refresh_device_status(smart_home_miloco_t *service,
                                 const char *did_text,
                                 char *body, size_t body_size)
{
    cJSON *root;
    cJSON *data;
    cJSON *item;
    size_t i;
    uint8_t k;
    bool changed = false;

    root = cJSON_Parse(body);
    if (!root) {
        return AGENT_ERROR_PARSE;
    }
    data = cJSON_GetObjectItemCaseSensitive(root, "data");
    lock_state(service);
    for (i = 0; i < service->device_count; i++) {
        smart_home_miloco_device_t *dev = &service->devices[i];

        if (strcmp(dev->did, did_text) != 0) {
            continue;
        }
        cJSON_ArrayForEach(item, data) {
            const cJSON *iid = cJSON_GetObjectItemCaseSensitive(item,
                                                                "iid");
            const cJSON *value = cJSON_GetObjectItemCaseSensitive(item,
                                                                  "value");
            int32_t v;

            if (!cJSON_IsString(iid) || !cJSON_IsNumber(value)) {
                continue;
            }
            v = (int32_t)value->valueint;
            for (k = 0; k < dev->control_count; k++) {
                smart_home_miloco_control_t *ctrl = &dev->controls[k];

                if (strcmp(ctrl->iid, iid->valuestring) == 0 &&
                    ctrl->value != v) {
                    ctrl->value = v;
                    changed = true;
                }
                if (strcmp(ctrl->iid, "prop.2.1") == 0) {
                    dev->power_on = v != 0;
                }
            }
        }
        break;
    }
    if (changed) {
        service->revision++;
    }
    unlock_state(service);
    cJSON_Delete(root);
    return AGENT_OK;
}

/* 拉取设备全部 prop 控制项的当前值（一次合并请求）。 */
static int refresh_device_values(smart_home_miloco_t *service,
                                 const char *did,
                                 char *body, size_t body_size)
{
    char path[160];
    char iids[96];
    size_t used = 0;
    int http_status = 0;
    int ret;
    size_t i;
    uint8_t k;

    iids[0] = '\0';
    lock_state(service);
    for (i = 0; i < service->device_count; i++) {
        const smart_home_miloco_device_t *dev = &service->devices[i];

        if (strcmp(dev->did, did) != 0) {
            continue;
        }
        for (k = 0; k < dev->control_count; k++) {
            const smart_home_miloco_control_t *ctrl = &dev->controls[k];

            if (ctrl->type == SMART_HOME_MILOCO_CTRL_ACTION) {
                continue;
            }
            if (used + strlen(ctrl->iid) + 2 >= sizeof(iids)) {
                break;
            }
            used += (size_t)snprintf(iids + used, sizeof(iids) - used,
                                     "%s%s", used ? "," : "", ctrl->iid);
        }
        break;
    }
    unlock_state(service);
    if (!used) {
        return AGENT_OK;
    }
    snprintf(path, sizeof(path), "/api/miot/devices/%.23s/status?iid=%s",
             did, iids);
    ret = smart_home_miloco_http_get(&service->client_config, path,
                                     body, body_size, &http_status);
    if (ret < 0) {
        return ret;
    }
    if (http_status != 200) {
        return -EIO;
    }
    return refresh_device_status(service, did, body, body_size);
}

/* ── worker ────────────────────────────────────────────────── */

/* spec 拉取重试记账（service 本地，避免改动 miloco.h 共享结构体触发
 * 全量重编）。原设计"新设备一次性拉 spec 且忽略返回值"：首次失败
 * （网关刚连上时网络/DNS 尚未稳定的高危时刻）即永久空 controls，
 * agent 与 UI 都无法控制。现改为 control_count 仍为 0 的在线设备
 * 每轮重试，超过上限放弃，防止 spec 恒空时打爆 backend。 */
#define MILOCO_SPEC_RETRY_MAX 12

static struct
{
    char did[24];
    uint8_t attempts;
} g_spec_attempts[SMART_HOME_MILOCO_MAX_DEVICES];

/* 记一次尝试；返回 false 表示已达上限或表满，本轮不再拉。 */
static bool spec_attempt_take(const char *did)
{
    int slot = -1;
    int i;

    if (!did || !did[0])
      {
        return false;
      }

    for (i = 0; i < SMART_HOME_MILOCO_MAX_DEVICES; i++)
      {
        if (g_spec_attempts[i].did[0] &&
            strcmp(g_spec_attempts[i].did, did) == 0)
          {
            slot = i;
            break;
          }

        if (slot < 0 && g_spec_attempts[i].did[0] == '\0')
          {
            slot = i;
          }
      }

    if (slot < 0)
      {
        return false;
      }

    if (g_spec_attempts[slot].did[0] == '\0')
      {
        snprintf(g_spec_attempts[slot].did,
                 sizeof(g_spec_attempts[slot].did), "%.23s", did);
        g_spec_attempts[slot].attempts = 0;
      }

    if (g_spec_attempts[slot].attempts >= MILOCO_SPEC_RETRY_MAX)
      {
        return false;
      }

    g_spec_attempts[slot].attempts++;
    return true;
}

static int fetch_spec_for(smart_home_miloco_t *service, const char *did,
                          char *body, size_t body_size)
{
    char path[80];
    int http_status = 0;
    int ret;

    snprintf(path, sizeof(path), "/api/miot/devices/%.23s/spec", did);
    ret = smart_home_miloco_http_get(&service->client_config, path,
                                     body, body_size, &http_status);
    if (ret < 0) {
        return ret;
    }
    if (http_status != 200) {
        return -EIO;
    }
    return parse_device_spec(service, body);
}

/* GET /api/miot/status 解析 data.is_bound。绑定状态翻转时递增 revision
 * 触发 UI 刷新；未绑定时设备列表清空。 */
static int poll_bind_status(smart_home_miloco_t *service,
                            char *body, size_t body_size)
{
    cJSON *root;
    cJSON *data;
    const cJSON *is_bound;
    bool bound = false;
    int http_status = 0;
    int ret;

    ret = smart_home_miloco_http_get(&service->client_config,
                                     "/api/miot/status", body, body_size,
                                     &http_status);
    if (ret < 0) {
        lock_state(service);
        if (service->reachable) {
            service->reachable = false;
            service->revision++;
        }
        unlock_state(service);
        return ret;
    }
    if (http_status != 200) {
        return -EIO;
    }
    root = cJSON_Parse(body);
    if (!root) {
        return AGENT_ERROR_PARSE;
    }
    data = cJSON_GetObjectItemCaseSensitive(root, "data");
    is_bound = cJSON_IsObject(data)
        ? cJSON_GetObjectItemCaseSensitive(data, "is_bound") : NULL;
    bound = cJSON_IsTrue(is_bound);
    cJSON_Delete(root);

    lock_state(service);
    if (!service->reachable || service->xiaomi_bound != bound) {
        service->revision++;
    }
    service->reachable = true;
    service->xiaomi_bound = bound;
    if (!bound && service->device_count > 0) {
        service->device_count = 0;
        service->revision++;
    }
    unlock_state(service);
    return bound ? AGENT_OK : AGENT_ERROR_NOTFOUND;
}

static int poll_device_list(smart_home_miloco_t *service,
                            char *body, size_t body_size)
{
    smart_home_miloco_device_t fresh[SMART_HOME_MILOCO_MAX_DEVICES];
    size_t fresh_count = 0;
    int http_status = 0;
    int ret;
    size_t i;

    ret = smart_home_miloco_http_get(&service->client_config,
                                     "/api/miot/device_list", body, body_size,
                                     &http_status);
    if (ret < 0) {
        /* 设备列表传输失败不降级网关在线状态：健康判定只属于
         * /api/miot/status（poll_bind_status）。保留上次设备快照，
         * 下轮轮询自然重试；绑定后服务端重编排期间曾出现短暂
         * 连接超时，属可恢复现象。 */
        return ret;
    }
    if (http_status != 200) {
        return -EIO;
    }
    ret = parse_device_list(service, body, fresh,
                            sizeof(fresh) / sizeof(fresh[0]), &fresh_count);
    if (ret != AGENT_OK) {
        return ret;
    }

    /* 新设备补 spec 类别；可控设备回读电源态。复用响应缓冲。 */
    for (i = 0; i < fresh_count; i++) {
        fetch_spec_for(service, fresh[i].did, body, body_size);
        spec_attempt_take(fresh[i].did);
    }
    {
        smart_home_miloco_device_t snapshot[SMART_HOME_MILOCO_MAX_DEVICES];
        size_t count;

        count = smart_home_miloco_list(service, snapshot,
                                       SMART_HOME_MILOCO_MAX_DEVICES, NULL);
        for (i = 0; i < count; i++) {
            /* controls 仍为空的在线设备重试 spec（带上限）：
             * 修复首次拉取失败一次即永久无 controls 的结构性缺口。 */
            if (snapshot[i].online && snapshot[i].control_count == 0 &&
                spec_attempt_take(snapshot[i].did)) {
                syslog(LOG_INFO,
                       "[milo] spec retry did=%.23s\n",
                       snapshot[i].did);
                fetch_spec_for(service, snapshot[i].did, body, body_size);
            }
    if (snapshot[i].online && snapshot[i].control_count > 0) {
                refresh_device_values(service, snapshot[i].did,
                                      body, body_size);
            }
        }
    }
    return AGENT_OK;
}

/* ── 天气（wttr.in 文本格式） ───────────────────────────────── */

/* 常见天气条件英中映射；未命中的保留英文原文。 */
static const struct {
    const char *en;
    const char *cn;
} g_weather_cn_map[] = {
    {"Clear",            "晴"},
    {"Sunny",            "晴"},
    {"Partly cloudy",    "多云"},
    {"Cloudy",           "阴"},
    {"Overcast",         "阴"},
    {"Light rain",       "小雨"},
    {"Moderate rain",    "中雨"},
    {"Heavy rain",       "大雨"},
    {"Light drizzle",    "毛毛雨"},
    {"Patchy rain",      "局部有雨"},
    {"Light snow",       "小雪"},
    {"Snow",             "雪"},
    {"Fog",              "雾"},
    {"Mist",             "薄雾"},
    {"Haze",             "霾"},
    {"Smoky haze",       "雾霾"},
    {"Thunder",          "雷阵雨"},
    {"Thundery showers", "雷阵雨"},
    {"Freezing fog",     "冻雾"},
};

static const char *weather_condition_cn(const char *en)
{
    size_t i;

    for (i = 0; i < sizeof(g_weather_cn_map) /
                    sizeof(g_weather_cn_map[0]); i++) {
        if (strcmp(en, g_weather_cn_map[i].en) == 0) {
            return g_weather_cn_map[i].cn;
        }
    }
    return en;
}

/* 拉取 wttr.in/{city}?format=%C|%t|%h|%w 并解析管道分隔响应。
 * 文本格式约 40 字节，远小于 JSON 25KB，复用现有响应缓冲。 */
static void poll_weather(smart_home_miloco_t *service,
                         char *body, size_t body_size)
{
    smart_home_miloco_client_config_t cfg;
    char path[64];
    char *fields[4];
    time_t server_date = (time_t)-1;
    char *saveptr;
    int http_status = 0;
    int field_count;
    int i;
    int ret;

    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.host, sizeof(cfg.host), "wttr.in");
    cfg.port = 80;
    cfg.token[0] = '\0';

    snprintf(path, sizeof(path), "/%.20s?format=%%C|%%t|%%h|%%w",
             service->weather.city);
    ret = smart_home_miloco_http_get_date(&cfg, path, body, body_size,
                                          &http_status, &server_date);
    if (ret < 0 || http_status != 200) {
        syslog(LOG_WARNING,
               "[milo] weather fetch failed ret=%d status=%d\n",
               ret, http_status);
        return;
    }

    syslog(LOG_INFO, "[milo] weather http date=%ld cur=%ld\n",
           (long)server_date, (long)time(NULL));

    /* 响应头 Date 即时间源（砍掉独立 SNTP 后的对时途径）：仅当
     * 本地时钟明显落后时前拨，避免正常走时被反复回写。 */
    if (server_date > (time_t)1000000000L) {
        struct timeval tv;

        gettimeofday(&tv, NULL);
        if (tv.tv_sec < server_date - 30L) {
            tv.tv_sec = server_date;
            tv.tv_usec = 0;
            settimeofday(&tv, NULL);
            syslog(LOG_INFO, "[milo] time set from HTTP Date: %ld\n",
                   (long)server_date);
        }
    }

    /* 响应形如 "Clear|+29°C|72%|→13km/h"，按管道切四段。 */
    field_count = 0;
    for (i = 0; i < 4; i++) {
        fields[i] = i == 0 ? strtok_r(body, "|", &saveptr) :
                             strtok_r(NULL, "|", &saveptr);
        if (!fields[i]) {
            break;
        }
        field_count++;
    }
    if (field_count < 4) {
        syslog(LOG_WARNING, "[milo] weather parse: got %d fields\n",
               field_count);
        return;
    }

    lock_state(service);
    snprintf(service->weather.condition_en,
             sizeof(service->weather.condition_en), "%s", fields[0]);
    snprintf(service->weather.condition_cn,
             sizeof(service->weather.condition_cn), "%s",
             weather_condition_cn(fields[0]));
    service->weather.temperature = atoi(fields[1]);
    service->weather.humidity = atoi(fields[2]);
    service->weather.wind_kmph = atoi(fields[3]);
    service->weather.valid = true;
    service->revision++;    /* 触发 UI 刷新 */
    unlock_state(service);
    syslog(LOG_INFO,
           "[milo] weather %s: %s(%s) %d°C %d%% %dkm/h\n",
           service->weather.city, fields[0],
           service->weather.condition_cn,
           service->weather.temperature, service->weather.humidity,
           service->weather.wind_kmph);
}



bool smart_home_miloco_get_weather(const smart_home_miloco_t *service,
                                   smart_home_miloco_weather_t *out)
{
    bool valid;

    if (!service || !out) {
        return false;
    }
    lock_state((smart_home_miloco_t *)service);
    *out = service->weather;
    valid = service->weather.valid;
    unlock_state((smart_home_miloco_t *)service);
    return valid;
}

int smart_home_miloco_set_weather_city(smart_home_miloco_t *service,
                                       const char *city)
{
    if (!service || !city || !city[0] || strlen(city) >= 24) {
        return -EINVAL;
    }
    lock_state(service);
    snprintf(service->weather.city, sizeof(service->weather.city),
             "%s", city);
    service->weather_poll_count = 0;    /* 下轮立即拉取 */
    unlock_state(service);
    return 0;
}

static int execute_control(smart_home_miloco_t *service,
                           const miloco_control_request_t *request,
                           char *body, size_t body_size)
{
    char path[80];
    char request_body[128];
    int http_status = 0;
    int ret;
    size_t i;
    uint8_t k;
    bool iid_allowed = false;
    bool is_bool = false;

    /* 执行侧安全闸：iid 必须命中该设备已解析（且黑名单已过滤）的
     * controls；未知 iid 一律拒绝。 */
    lock_state(service);
    for (i = 0; i < service->device_count && !iid_allowed; i++) {
        const smart_home_miloco_device_t *dev = &service->devices[i];

        if (strcmp(dev->did, request->did) != 0) {
            continue;
        }
        for (k = 0; k < dev->control_count; k++) {
            if (strcmp(dev->controls[k].iid, request->iid) == 0) {
                iid_allowed = true;
                is_bool = dev->controls[k].type ==
                          SMART_HOME_MILOCO_CTRL_BOOL;
                break;
            }
        }
    }
    unlock_state(service);
    if (!iid_allowed) {
        syslog(LOG_WARNING,
               "[milo] control rejected unknown iid %s did %s\n",
               request->iid, request->did);
        return -EINVAL;
    }

    snprintf(path, sizeof(path), "/api/miot/devices/%.23s/control",
             request->did);
    if (request->is_action) {
        snprintf(request_body, sizeof(request_body),
                 "{\"type\":\"call_action\",\"iid\":\"%.13s\"}",
                 request->iid);
    } else if (is_bool) {
        snprintf(request_body, sizeof(request_body),
                 "{\"type\":\"set_property\",\"iid\":\"%.13s\","
                 "\"value\":%s}",
                 request->iid, request->value ? "true" : "false");
    } else {
        snprintf(request_body, sizeof(request_body),
                 "{\"type\":\"set_property\",\"iid\":\"%.13s\","
                 "\"value\":%d}",
                 request->iid, (int)request->value);
    }
    ret = smart_home_miloco_http_post(&service->client_config, path,
                                      request_body, body, body_size,
                                      &http_status);
    if (ret < 0) {
        return ret;
    }
    if (http_status != 200) {
        return -EIO;
    }
    /* 控制成功后回读真实状态，失败也接受（下轮轮询会补）。 */
    (void)refresh_device_values(service, request->did, body, body_size);
    return AGENT_OK;
}

static void *miloco_worker(void *argument)
{
    smart_home_miloco_t *service = argument;
    char *body;
    struct timespec wait_until;
    bool poll_due = true;

    syslog(LOG_INFO, "[milo] worker: entry\n");
    body = smart_home_bulk_alloc(MILOCO_RESPONSE_BYTES);
    if (!body) {
        syslog(LOG_ERR, "ERROR: [miloco] response buffer alloc failed\n");
        return NULL;
    }

    while (1) {
        bool stop;
        bool has_control = false;
        miloco_control_request_t control;

        /* 天气拉取：首次立即拉取，此后每 120 轮（10 分钟）一次。
         * 时间同步随天气走（HTTP Date 头），不再有独立 SNTP。 */
        /* 天气拉取：首次立即，此后每 120 轮（10 分钟）一次；
         * 拉取失败（WiFi 未就绪/外网不通）时每 12 轮（1 分钟）重试，
         * 开机联网后最迟 1 分钟内即有天气和时间（HTTP Date 同步）。 */
        service->weather_poll_count++;
        if (service->weather_poll_count >= 120 ||
            (!service->weather.valid &&
             (service->weather_poll_count % 12) == 0)) {
            poll_weather(service, body, MILOCO_RESPONSE_BYTES);
            service->weather_poll_count = 0;
        }

        if (poll_due) {
            syslog(LOG_INFO, "[milo] worker: poll begin\n");
            if (poll_bind_status(service, body, MILOCO_RESPONSE_BYTES)
                    == AGENT_OK) {
                /* 已绑定才轮询设备；未绑定时绑定页只需要 is_bound。 */
                poll_device_list(service, body, MILOCO_RESPONSE_BYTES);
            }
            poll_due = false;
        }

        lock_state(service);
        stop = service->stop_requested;
        if (service->save_pending) {
            smart_home_miloco_config_t save = service->save_config;
            smart_home_miloco_client_config_t client_config;
            int save_ret;

            service->save_pending = false;
            snprintf(client_config.host, sizeof(client_config.host), "%s",
                     save.host);
            client_config.port = save.port;
            snprintf(client_config.token, sizeof(client_config.token), "%s",
                     save.token);
            unlock_state(service);
#ifdef CONFIG_SMART_HOME_MILOCO_VOLATILE_CONFIG
            /* 挥发型配置：跳过 secrets.json 写入（flash 写挂死的临时
             * 规避），仅在工作线程内应用并立即轮询。 */
            save_ret = AGENT_OK;
            syslog(LOG_INFO, "[milo] worker: volatile config applied\n");
#else
            syslog(LOG_INFO, "[milo] worker: saving secrets\n");
            save_ret = smart_home_secrets_set_miloco(save.host, save.port,
                                                     save.token);
            syslog(LOG_INFO, "[milo] worker: secrets save ret=%d\n",
                   save_ret);
#endif
            lock_state(service);
            if (save_ret == AGENT_OK) {
                service->client_config = client_config;
                service->reachable = false;
                service->revision++;
            }
            poll_due = true;
        }
        if (service->config_dirty) {
            service->client_config = service->pending_config;
            service->config_dirty = false;
            poll_due = true;
        }
        if (!stop && service->pending_count > 0) {
            control = service->pending[0];
            memmove(&service->pending[0], &service->pending[1],
                    (service->pending_count - 1) *
                        sizeof(service->pending[0]));
            service->pending_count--;
            has_control = true;
        }
        unlock_state(service);

        if (stop) {
            break;
        }
        if (has_control) {
            if (execute_control(service, &control, body,
                                MILOCO_RESPONSE_BYTES) != AGENT_OK) {
                syslog(LOG_WARNING,
                       "WARNING: [miloco] control did=%s iid=%s failed\n",
                       control.did, control.iid);
            }
            continue; /* 立即处理后续排队的控制请求。 */
        }

        if (clock_gettime(CLOCK_REALTIME, &wait_until) == 0) {
            wait_until.tv_sec += SMART_HOME_MILOCO_POLL_INTERVAL_SEC;
            if (sem_timedwait(&service->wake, &wait_until) == 0) {
                /* 被控制请求或停机唤醒；poll 不提前。 */
                continue;
            }
        } else {
            sleep(SMART_HOME_MILOCO_POLL_INTERVAL_SEC);
        }
        poll_due = true;
    }

    smart_home_bulk_free(body);
    return NULL;
}

int smart_home_miloco_start(smart_home_miloco_t **service_out,
                            const smart_home_miloco_config_t *config)
{
    smart_home_miloco_t *service;
    pthread_attr_t attr;
    void *stack;
    int ret;

    if (!service_out || !smart_home_miloco_config_valid(config)) {
        return AGENT_ERROR_INVALID;
    }
    if (*service_out) {
        return AGENT_ERROR_INVALID;
    }

    syslog(LOG_INFO, "[milo] start: enter host=%s\n", config->host);
    service = calloc(1, sizeof(*service));
    if (!service) {
        return AGENT_ERROR_NOMEM;
    }
    snprintf(service->client_config.host,
             sizeof(service->client_config.host), "%s", config->host);
    service->client_config.port = config->port;
    snprintf(service->client_config.token,
             sizeof(service->client_config.token), "%s", config->token);

    ret = pthread_mutex_init(&service->lock, NULL);
    if (ret != 0) {
        free(service);
        return AGENT_ERROR;
    }
    if (sem_init(&service->wake, 0, 0) != 0) {
        pthread_mutex_destroy(&service->lock);
        free(service);
        return AGENT_ERROR;
    }

    /* worker 栈走 PSRAM，避免占用默认 pthread 栈预算；pthread 栈有
     * 架构对齐要求（RISC-V 16B），PSRAM 分配不保证，必须向上对齐
     * （网络配网 worker 同款做法）。 */
    syslog(LOG_INFO, "[milo] start: mutex/sem ready\n");
    stack = smart_home_bulk_alloc(MILOCO_WORKER_STACK_SIZE +
                                  STACK_ALIGNMENT - 1u);
    if (!stack) {
        sem_destroy(&service->wake);
        pthread_mutex_destroy(&service->lock);
        free(service);
        return AGENT_ERROR_NOMEM;
    }
    service->worker_stack = (void *)STACK_ALIGN_UP((uintptr_t)stack);
    service->worker_stack_bytes = MILOCO_WORKER_STACK_SIZE;
    syslog(LOG_INFO, "[milo] start: stack=%p bytes=%u\n",
           service->worker_stack, (unsigned)service->worker_stack_bytes);
    ret = pthread_attr_init(&attr);
    if (ret == 0) {
        ret = pthread_attr_setstack(&attr, service->worker_stack,
                                    service->worker_stack_bytes);
    }
    syslog(LOG_INFO, "[milo] start: attr ret=%d, creating thread\n", ret);
    if (ret == 0) {
        ret = pthread_create(&service->worker, &attr, miloco_worker, service);
    }
    syslog(LOG_INFO, "[milo] start: pthread_create ret=%d\n", ret);
    pthread_attr_destroy(&attr);
    if (ret != 0) {
        smart_home_bulk_free(service->worker_stack);
        sem_destroy(&service->wake);
        pthread_mutex_destroy(&service->lock);
        free(service);
        return AGENT_ERROR;
    }
    service->worker_started = true;
    snprintf(service->weather.city, sizeof(service->weather.city),
             "Shenzhen");
    service->weather_poll_count = 119;  /* 首轮立即拉取天气+SNTP */
    syslog(LOG_INFO,
           "INFO: [miloco] gateway worker started host=%s port=%u\n",
           config->host, (unsigned)config->port);
    *service_out = service;
    return AGENT_OK;
}

void smart_home_miloco_stop(smart_home_miloco_t **service_ptr)
{
    smart_home_miloco_t *service;

    if (!service_ptr || !*service_ptr) {
        return;
    }
    service = *service_ptr;
    *service_ptr = NULL;

    lock_state(service);
    service->stop_requested = true;
    unlock_state(service);
    sem_post(&service->wake);

    if (service->worker_started) {
        pthread_join(service->worker, NULL);
    }
    smart_home_bulk_free(service->worker_stack);
    sem_destroy(&service->wake);
    pthread_mutex_destroy(&service->lock);
    free(service);
    syslog(LOG_INFO, "INFO: [miloco] gateway worker stopped\n");
}
