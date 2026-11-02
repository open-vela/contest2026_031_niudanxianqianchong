/* SPDX-License-Identifier: Apache-2.0 */

/****************************************************************************
 * demos/smart_home/src/tools/smart_home_camera_tools.c
 *
 * Agent 本地摄像头工具：单工具 camera，四个动作：
 *   status  查询在线状态 / FPS / 丢帧
 *   on/off  请求开关（非阻塞合并命令，返回真实 desired 状态）
 *   shot    取最新预览帧 → 计算亮度/变化量 → 落盘两槽滚动 → 返回元数据
 *
 * 约束：
 * - 只通过 camera_service / camera_control 公开 API 访问摄像头，
 *   绝不直接 open /dev/video0（服务是唯一消费者）；
 * - 帧缓冲 307200 B 走 PSRAM（smart_home_bulk_alloc），handler 内
 *   局部变量很小（agent worker 栈仅 32 KB）；
 * - 拍照限流 800 ms，避免与采集 worker 抢 camera lock。
 ****************************************************************************/

#include "smart_home_camera_tools.h"

#include "../agent/smart_home_tool_metadata.h"
#include "../camera/smart_home_camera_control.h"
#include "../camera/smart_home_camera_service.h"
#include "../camera/smart_home_snapshot_store.h"
#include "../smart_home_memory.h"
#include "smart_home_tool_io.h"
#include "smart_home_tools.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CAMERA_TOOL_NAME        "camera"
#define CAMERA_TOOL_MIN_INTERVAL_MS 800u

#define CAMERA_SAMPLE_STEP      8u
#define CAMERA_SAMPLE_COLS      (SMART_HOME_CAMERA_PREVIEW_WIDTH / CAMERA_SAMPLE_STEP)
#define CAMERA_SAMPLE_ROWS      (SMART_HOME_CAMERA_PREVIEW_HEIGHT / CAMERA_SAMPLE_STEP)
#define CAMERA_SAMPLE_COUNT     (CAMERA_SAMPLE_COLS * CAMERA_SAMPLE_ROWS)

#define CAMERA_SCHEMA_JSON                                                  \
  "{\"type\":\"object\",\"properties\":{"                                   \
  "\"action\":{\"type\":\"string\","                                        \
  "\"enum\":[\"status\",\"on\",\"off\",\"shot\"],"                          \
  "\"description\":\"status=查询, on=开启, off=关闭, shot=拍照并保存\"},"   \
  "\"reason\":{\"type\":\"string\","                                        \
  "\"description\":\"用户意图摘要，可选\"}},"                               \
  "\"required\":[\"action\"],\"additionalProperties\":false}"

/* 采样亮度（0..255），供亮度与帧间变化量使用。 */
static uint8_t camera_luma(uint16_t pixel)
{
  unsigned int r = ((pixel >> 11) & 0x1fu) * 8u;
  unsigned int g = ((pixel >> 5) & 0x3fu) * 4u;
  unsigned int b = (pixel & 0x1fu) * 8u;

  return (uint8_t)((r * 77u + g * 150u + b * 29u) >> 8);
}

static void camera_sample_frame(const uint8_t *frame, uint8_t *samples,
                                unsigned int *sum_luma)
{
  const uint16_t *pixels = (const uint16_t *)frame;
  unsigned int index = 0u;
  unsigned int total = 0u;
  unsigned int y;
  unsigned int x;

  for (y = 0u; y < CAMERA_SAMPLE_ROWS; y++)
    {
      const uint16_t *row = pixels + (y * CAMERA_SAMPLE_STEP) *
                            SMART_HOME_CAMERA_PREVIEW_WIDTH;

      for (x = 0u; x < CAMERA_SAMPLE_COLS; x++)
        {
          uint8_t luma = camera_luma(row[x * CAMERA_SAMPLE_STEP]);

          samples[index++] = luma;
          total += luma;
        }
    }

  *sum_luma = total;
}

/* 状态名与 LVGL 侧保持一致，便于模型叙事。 */
static const char *camera_state_name(smart_home_camera_state_t state)
{
  switch (state)
    {
      case SMART_HOME_CAMERA_STARTING:
        return "STARTING";
      case SMART_HOME_CAMERA_RUNNING:
        return "RUNNING";
      case SMART_HOME_CAMERA_ERROR:
        return "ERROR";
      case SMART_HOME_CAMERA_OFF:
      default:
        return "OFF";
    }
}

static void camera_tool_status(const agent_text_sink_t *sink)
{
  struct smart_home_camera_status_s status;
  char output[192];
  int ret = smart_home_camera_get_status(&status);

  if (ret != 0)
    {
      snprintf(output, sizeof(output),
               "{\"ok\":false,\"error\":\"status_failed\",\"ret\":%d}", ret);
      (void)smart_home_tool_reply(sink, output, AGENT_ERROR_IO);
      return;
    }

  snprintf(output, sizeof(output),
           "{\"ok\":true,\"online\":%s,\"state\":\"%s\"," 
           "\"capture_fps_x100\":%lu,\"preview_fps_x100\":%lu,"
           "\"dropped\":%lu}",
           status.state == SMART_HOME_CAMERA_RUNNING ? "true" : "false",
           camera_state_name(status.state),
           (unsigned long)status.capture_fps_x100,
           (unsigned long)status.preview_fps_x100,
           (unsigned long)status.dropped_frames);
  (void)smart_home_tool_reply(sink, output, AGENT_OK);
}

static void camera_tool_power(const agent_text_sink_t *sink, int on)
{
  smart_home_camera_control_status_t control;
  char output[160];
  int ret = smart_home_camera_request(on != 0);

  if (ret != 0)
    {
      snprintf(output, sizeof(output),
               "{\"ok\":false,\"error\":\"request_failed\",\"ret\":%d}", ret);
      (void)smart_home_tool_reply(sink, output, AGENT_ERROR_TOOL_FAILED);
      return;
    }

  smart_home_camera_control_status(&control);
  snprintf(output, sizeof(output),
           "{\"ok\":true,\"desired_on\":%s,\"busy\":%s,"
           "\"note\":\"state changes asynchronously; verify with status\"}",
           control.desired_on ? "true" : "false",
           control.busy ? "true" : "false");
  (void)smart_home_tool_reply(sink, output, AGENT_OK);
}

/* 等首帧：摄像头刚启动时最多等 300 ms。 */
static int camera_wait_first_frame(void)
{
  struct smart_home_camera_status_s status;
  unsigned int waited = 0u;

  while (waited < 300u)
    {
      if (smart_home_camera_get_status(&status) == 0 &&
          status.state == SMART_HOME_CAMERA_RUNNING &&
          status.preview_sequence > 0u)
        {
          return 0;
        }

      usleep(20 * 1000);
      waited += 20u;
    }

  return -ETIMEDOUT;
}

static void camera_tool_shot(const agent_text_sink_t *sink)
{
  static uint8_t *frame;
  static uint8_t previous[CAMERA_SAMPLE_COUNT];
  static uint8_t current[CAMERA_SAMPLE_COUNT];
  static int have_previous;
  static uint64_t last_shot_ms;
  static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
  struct smart_home_snapshot_info_s info;
  struct smart_home_camera_status_s status;
  struct timespec now;
  char output[384];
  unsigned int sum_luma = 0u;
  unsigned int motion = 0u;
  uint32_t sequence = 0u;
  uint64_t now_ms;
  uint64_t wall;
  int ret;

  if (pthread_mutex_lock(&lock) != 0)
    {
      (void)smart_home_tool_reply(sink,
                                  "{\"ok\":false,\"error\":\"busy\"}",
                                  AGENT_ERROR_BUSY);
      return;
    }

  clock_gettime(CLOCK_MONOTONIC, &now);
  now_ms = (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
  if (last_shot_ms != 0u && now_ms - last_shot_ms < CAMERA_TOOL_MIN_INTERVAL_MS)
    {
      pthread_mutex_unlock(&lock);
      (void)smart_home_tool_reply(
          sink, "{\"ok\":false,\"error\":\"rate_limited\"}", AGENT_ERROR_BUSY);
      return;
    }

  if (frame == NULL)
    {
      frame = smart_home_bulk_alloc(SMART_HOME_CAMERA_PREVIEW_BYTES);
      if (frame == NULL)
        {
          pthread_mutex_unlock(&lock);
          (void)smart_home_tool_reply(sink,
                                      "{\"ok\":false,\"error\":\"no_memory\"}",
                                      AGENT_ERROR_NOMEM);
          return;
        }
    }

  ret = smart_home_camera_get_status(&status);
  if (ret != 0 || status.state != SMART_HOME_CAMERA_RUNNING)
    {
      (void)smart_home_camera_request(true);
      ret = camera_wait_first_frame();
      if (ret != 0)
        {
          pthread_mutex_unlock(&lock);
          (void)smart_home_tool_reply(
              sink,
              "{\"ok\":true,\"pending\":true,"
              "\"note\":\"camera starting; retry in a moment\"}",
              AGENT_OK);
          return;
        }
    }

  ret = smart_home_camera_copy_latest(frame, SMART_HOME_CAMERA_PREVIEW_BYTES,
                                      &sequence);
  if (ret == -EAGAIN)
    {
      usleep(50 * 1000);
      ret = smart_home_camera_copy_latest(frame,
                                          SMART_HOME_CAMERA_PREVIEW_BYTES,
                                          &sequence);
    }
  if (ret != 0)
    {
      pthread_mutex_unlock(&lock);
      snprintf(output, sizeof(output),
               "{\"ok\":false,\"error\":\"frame_unavailable\",\"ret\":%d}",
               ret);
      (void)smart_home_tool_reply(sink, output, AGENT_ERROR_TOOL_FAILED);
      return;
    }

  camera_sample_frame(frame, current, &sum_luma);
  if (have_previous)
    {
      unsigned int diff = 0u;
      unsigned int i;

      for (i = 0u; i < CAMERA_SAMPLE_COUNT; i++)
        {
          diff += current[i] > previous[i] ? current[i] - previous[i]
                                           : previous[i] - current[i];
        }

      motion = diff * 1000u / (CAMERA_SAMPLE_COUNT * 255u);
    }

  memcpy(previous, current, sizeof(previous));
  have_previous = 1;

  wall = (uint64_t)time(NULL);
  if (wall < 1600000000u)
    {
      wall = 0; /* 墙钟未同步（HTTP Date 尚未取得），只留 seq */
    }

  ret = smart_home_snapshot_store_write(
      frame, SMART_HOME_CAMERA_PREVIEW_BYTES,
      SMART_HOME_CAMERA_PREVIEW_WIDTH, SMART_HOME_CAMERA_PREVIEW_HEIGHT,
      wall, sum_luma / CAMERA_SAMPLE_COUNT, motion, &info);
  last_shot_ms = now_ms;

  pthread_mutex_unlock(&lock);

  if (ret != 0)
    {
      snprintf(output, sizeof(output),
               "{\"ok\":false,\"error\":\"save_failed\",\"ret\":%d}", ret);
      (void)smart_home_tool_reply(sink, output, AGENT_ERROR_IO);
      return;
    }

  snprintf(output, sizeof(output),
           "{\"ok\":true,\"path\":\"%s\",\"slot\":%u,\"seq\":%lu,"
           "\"bytes\":%lu,\"width\":%u,\"height\":%u,"
           "\"format\":\"rgb565\",\"brightness\":%u,\"motion\":%u,"
           "\"ts\":%llu,\"frame_seq\":%lu}",
           info.path, (unsigned)info.slot, (unsigned long)info.seq,
           (unsigned long)info.bytes, (unsigned)info.width,
           (unsigned)info.height, (unsigned)info.brightness,
           (unsigned)info.motion, (unsigned long long)info.ts,
           (unsigned long)sequence);
  (void)smart_home_tool_reply(sink, output, AGENT_OK);
}

static agent_error_t camera_tool_execute(void *user_data,
                                         const agent_tool_context_t *context,
                                         const agent_text_sink_t *sink)
{
  char action[8];
  agent_error_t ret;

  (void)user_data;

  if (context == NULL || sink == NULL)
    {
      return AGENT_ERROR_INVALID;
    }

  ret = smart_home_tool_get_string(context->call.arguments_json,
                                   "action", action, sizeof(action));
  if (ret != AGENT_OK)
    {
      return smart_home_tool_reply(
          sink, "{\"ok\":false,\"error\":\"missing_action\"}",
          AGENT_ERROR_TOOL_ARGUMENT);
    }

  if (strcmp(action, "status") == 0)
    {
      camera_tool_status(sink);
      return AGENT_OK;
    }

  if (strcmp(action, "on") == 0)
    {
      camera_tool_power(sink, 1);
      return AGENT_OK;
    }

  if (strcmp(action, "off") == 0)
    {
      camera_tool_power(sink, 0);
      return AGENT_OK;
    }

  if (strcmp(action, "shot") == 0)
    {
      camera_tool_shot(sink);
      return AGENT_OK;
    }

  return smart_home_tool_reply(sink,
                               "{\"ok\":false,\"error\":\"unknown_action\"}",
                               AGENT_ERROR_TOOL_ARGUMENT);
}

int smart_home_camera_tools_register(agent_t *agent)
{
  if (agent == NULL)
    {
      return AGENT_ERROR_INVALID;
    }

  return smart_home_tool_register_local(
      agent, CAMERA_TOOL_NAME,
      "Control the local camera. action=status/on/off or shot "
      "(capture preview, save to /data/smart_home/snapshots, "
      "return brightness and motion).",
      CAMERA_SCHEMA_JSON, SMART_HOME_TOOL_CATEGORY_CONTROL,
      camera_tool_execute, NULL, AGENT_TOOL_SIDE_EFFECT);
}

/* 板端自测：构造与模型调用等价的 arguments_json，走同一个 execute 路径。 */

struct camera_selftest_sink_s
{
  char *buffer;
  size_t size;
  size_t used;
};

static agent_error_t camera_selftest_write(void *context,
                                          agent_string_view_t text)
{
  struct camera_selftest_sink_s *sink = context;

  if (sink == NULL || (text.data == NULL && text.size > 0u))
    {
      return AGENT_ERROR_INVALID;
    }

  if (text.size >= sink->size - sink->used)
    {
      return AGENT_ERROR_CAPACITY;
    }

  memcpy(sink->buffer + sink->used, text.data, text.size);
  sink->used += text.size;
  sink->buffer[sink->used] = '\0';
  return AGENT_OK;
}

int smart_home_camera_tool_selftest(const char *action)
{
  char arguments[48];
  char output[768];
  struct camera_selftest_sink_s state;
  agent_text_sink_t sink;
  agent_tool_context_t context;
  agent_error_t ret;

  if (action == NULL || action[0] == '\0')
    {
      return AGENT_ERROR_INVALID;
    }

  snprintf(arguments, sizeof(arguments), "{\"action\":\"%s\"}", action);
  output[0] = '\0';

  state.buffer = output;
  state.size = sizeof(output);
  state.used = 0u;
  sink.write = camera_selftest_write;
  sink.context = &state;

  memset(&context, 0, sizeof(context));
  context.call.arguments_json = agent_string_view(arguments,
                                                 strlen(arguments));

  ret = camera_tool_execute(NULL, &context, &sink);
  printf("camera_tool[%s] ret=%d %s\n", action, (int)ret, output);
  return ret == AGENT_OK ? 0 : 1;
}
