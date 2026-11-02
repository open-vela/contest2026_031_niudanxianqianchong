/****************************************************************************
 * demos/smart_home/tests/vad/vad_selftest.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 宿主侧 VAD 自测：用合成 PCM 验证端点判决，不依赖板子、不依赖模型。
 *
 *   gcc -O2 -Wall -Wextra -Iinclude -I../../src/voice \
 *       vad_selftest.c ../../src/voice/smart_home_vad.c -o vad_selftest
 *   ./vad_selftest
 *
 * 覆盖四个场景：数字静音、稳定底噪、噪声中的人声、长静音后的弱人声。
 ****************************************************************************/

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "smart_home_vad.h"

#define RATE 16000

struct counter_s
{
  int start;
  int end;
};

static struct counter_s g_count;

/* 推进 n 毫秒，按 10 ms 一块喂给 VAD（模拟采集缓冲）。 */
static void feed(int ms, const int16_t *pcm)
{
  int block = RATE / 100;          /* 10 ms */
  int total = RATE * ms / 1000;
  int done = 0;

  while (done < total)
    {
      int n = total - done;

      if (n > block)
        {
          n = block;
        }

      smart_home_vad_event_t evt = smart_home_vad_process(pcm + done, n);

      if (evt == SMART_HOME_VAD_EVENT_SPEECH_START)
        {
          g_count.start++;
        }
      else if (evt == SMART_HOME_VAD_EVENT_SPEECH_END)
        {
          g_count.end++;
        }

      done += n;
    }
}

static void fill_silence(int16_t *pcm, int ms)
{
  memset(pcm, 0, (size_t)RATE * ms / 1000 * sizeof(int16_t));
}

static void fill_tone(int16_t *pcm, int ms, double hz, double amp)
{
  int n = RATE * ms / 1000;
  int i;

  for (i = 0; i < n; i++)
    {
      pcm[i] = (int16_t)(amp * sin(2.0 * M_PI * hz * i / RATE));
    }
}

static void fill_noise(int16_t *pcm, int ms, int amp, unsigned int seed)
{
  int n = RATE * ms / 1000;
  int i;

  srand(seed);
  for (i = 0; i < n; i++)
    {
      int v = (rand() % (2 * amp + 1)) - amp;

      pcm[i] = (int16_t)v;
    }
}

static int check(const char *name, int want_start, int want_end)
{
  int ok = g_count.start == want_start && g_count.end == want_end;

  printf("%-28s start=%d/%d end=%d/%d  %s\n", name,
         g_count.start, want_start, g_count.end, want_end,
         ok ? "OK" : "FAIL");
  return ok;
}

int main(void)
{
  int n = RATE * 6;
  int16_t *pcm = malloc((size_t)n * sizeof(int16_t));
  int failures = 0;

  if (pcm == NULL)
    {
      return 2;
    }

  /* 1. 数字静音：绝不应触发。 */

  smart_home_vad_reset();
  memset(&g_count, 0, sizeof(g_count));
  fill_silence(pcm, 2000);
  feed(2000, pcm);
  failures += !check("digital silence", 0, 0);

  /* 2. 稳定底噪：噪声底自适应后不应触发。 */

  smart_home_vad_reset();
  memset(&g_count, 0, sizeof(g_count));
  fill_noise(pcm, 3000, 200, 7);
  feed(3000, pcm);
  failures += !check("steady noise", 0, 0);

  /* 3. 噪声中的人声：一段 500 ms 300 Hz 语音后回落，应恰好一次起止。 */

  smart_home_vad_reset();
  memset(&g_count, 0, sizeof(g_count));
  fill_noise(pcm, 1000, 200, 11);
  feed(1000, pcm);
  fill_tone(pcm, 500, 300.0, 4000.0);
  feed(500, pcm);
  fill_noise(pcm, 1500, 200, 13);
  feed(1500, pcm);
  failures += !check("speech in noise", 1, 1);

  /* 4. 长静音后的弱人声：同样应被检出（相对门限而非绝对门限）。 */

  smart_home_vad_reset();
  memset(&g_count, 0, sizeof(g_count));
  fill_silence(pcm, 1000);
  feed(1000, pcm);
  fill_tone(pcm, 400, 440.0, 1200.0);
  feed(400, pcm);
  fill_silence(pcm, 1500);
  feed(1500, pcm);
  failures += !check("weak speech after silence", 1, 1);

  printf("\nlast energy=%d noise floor=%d\n",
         smart_home_vad_last_energy(), smart_home_vad_noise_floor());

  free(pcm);
  return failures == 0 ? 0 : 1;
}
