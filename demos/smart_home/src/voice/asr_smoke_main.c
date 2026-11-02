/****************************************************************************
 * demos/smart_home/src/voice/asr_smoke_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 The NuttX Contributors
 *
 ****************************************************************************/

/* asr_smoke：语音识别链路的 NSH 冒烟命令。
 *
 * record [seconds]  经共享采集服务录制一段音频并打印时长/字节数，
 *                   验证 /dev/audio/pcm_in0 采集基线。
 * recognize [seconds]
 *                   录制后立即走 MiMo-V2.5-ASR 云端识别并打印转写文本，
 *                   验证 ASR 全链路（不经过聊天 UI）。
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "smart_home_asr.h"
#include "smart_home_voice_capture.h"

#include "../smart_home_memory.h"

#define ASR_SMOKE_DEFAULT_SECONDS 4
#define ASR_SMOKE_MAX_SECONDS     30
#define ASR_SMOKE_TEXT_MAX        512

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void asr_smoke_usage(FAR const char *progname)
{
  printf("Usage:\n"
         "  %s record [seconds]\n"
         "  %s record-vad [seconds] [silence_ms]\n"
         "  %s recognize [seconds]\n"
         "record:     capture raw PCM via the shared capture service\n"
         "record-vad: same, but stop early after trailing silence (VAD)\n"
         "recognize:  capture then recognize via MiMo ASR and print text\n"
         "Defaults: %d s, max %d s, silence 500 ms\n",
         progname, progname, ASR_SMOKE_DEFAULT_SECONDS, ASR_SMOKE_MAX_SECONDS);
}

static int asr_smoke_parse_seconds(FAR const char *text, int *seconds)
{
  FAR char *end;
  long value;

  errno = 0;
  value = strtol(text, &end, 10);
  if (errno != 0 || *text == '\0' || *end != '\0' || value <= 0 ||
      value > ASR_SMOKE_MAX_SECONDS)
    {
      return -EINVAL;
    }

  *seconds = (int)value;
  return 0;
}

static int asr_smoke_run(int seconds, int recognize, int silence_ms)
{
  uint8_t *pcm = NULL;
  size_t bytes = 0;
  char text[ASR_SMOKE_TEXT_MAX];
  int ret;

  if (silence_ms > 0)
    {
      printf("asr_smoke: recording %d s (VAD auto-stop %d ms)...\n",
             seconds, silence_ms);
      ret = voice_capture_record_vad(seconds, NULL, silence_ms, &pcm, &bytes);
    }
  else
    {
      printf("asr_smoke: recording %d s...\n", seconds);
      ret = voice_capture_record(seconds, NULL, &pcm, &bytes);
    }

  if (ret != 0)
    {
      fprintf(stderr, "asr_smoke: record failed: %d (%s)\n",
              ret, strerror(-ret));
      return EXIT_FAILURE;
    }

  printf("asr_smoke: captured %zu bytes (%.1f s at 16 kHz PCM16 mono)\n",
         bytes, (double)bytes / 32000.0);

  if (!recognize)
    {
      smart_home_bulk_free(pcm);
      return EXIT_SUCCESS;
    }

  ret = smart_home_asr_recognize(pcm, bytes, text, sizeof(text));
  smart_home_bulk_free(pcm);
  if (ret != 0)
    {
      fprintf(stderr, "asr_smoke: recognize failed: %d\n", ret);
      return EXIT_FAILURE;
    }

  printf("asr_smoke: recognized: %s\n", text);
  return EXIT_SUCCESS;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  int seconds = ASR_SMOKE_DEFAULT_SECONDS;
  int recognize = 0;
  int silence_ms = 0;
  int ret;

  if (argc >= 2)
    {
      if (strcmp(argv[1], "record") == 0)
        {
          recognize = 0;
        }
      else if (strcmp(argv[1], "record-vad") == 0)
        {
          recognize = 0;
          silence_ms = 500;
        }
      else if (strcmp(argv[1], "recognize") == 0)
        {
          recognize = 1;
        }
      else
        {
          asr_smoke_usage(argv[0]);
          return EXIT_FAILURE;
        }
    }
  else
    {
      asr_smoke_usage(argv[0]);
      return EXIT_FAILURE;
    }

  if (argc >= 3 && asr_smoke_parse_seconds(argv[2], &seconds) < 0)
    {
      asr_smoke_usage(argv[0]);
      return EXIT_FAILURE;
    }

  if (argc >= 4 && silence_ms > 0)
    {
      int parsed;

      if (asr_smoke_parse_seconds(argv[3], &parsed) == 0)
        {
          silence_ms = parsed;
        }
    }

  ret = asr_smoke_run(seconds, recognize, silence_ms);
  voice_capture_deinit();
  return ret;
}
