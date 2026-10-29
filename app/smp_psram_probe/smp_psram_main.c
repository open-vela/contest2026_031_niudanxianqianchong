/****************************************************************************
 * app/smp_psram_probe/smp_psram_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <inttypes.h>
#include <malloc.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* ESP32-P4 PSRAM virtual window, from the HAL's soc/esp32p4/soc.h. */

#define PSRAM_LOW   0x48000000ul
#define PSRAM_HIGH  0x4c000000ul
#define PROBE_MIB   (1024 * 1024)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct psram_probe_s
{
  FAR volatile uint32_t *buffer;
  size_t words;
  unsigned int loops;
  sem_t start;
  pthread_barrier_t barrier;
  bool cancel;
};

struct psram_worker_s
{
  FAR struct psram_probe_s *probe;
  unsigned int cpu;
  unsigned int errors;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t psram_pattern(size_t index, unsigned int loop,
                              unsigned int cpu)
{
  return (uint32_t)index ^ (0x9e3779b9u * (loop + 1)) ^
         (0x13579bdfu << cpu);
}

static void psram_verify(FAR struct psram_worker_s *worker,
                          unsigned int loop, unsigned int producer)
{
  FAR struct psram_probe_s *probe = worker->probe;
  size_t base = producer * probe->words;
  uint32_t expected;
  uint32_t actual;
  size_t i;

  for (i = 0; i < probe->words; i++)
    {
      expected = psram_pattern(i, loop, producer);
      actual = probe->buffer[base + i];
      if (actual != expected)
        {
          if (worker->errors == 0)
            {
              printf("smp_psram: CPU%u mismatch producer=%u loop=%u "
                     "word=%zu got=%08" PRIx32 " expected=%08" PRIx32 "\n",
                     worker->cpu, producer, loop, i, actual, expected);
            }

          worker->errors++;
          break;
        }
    }
}

static FAR void *psram_worker(FAR void *arg)
{
  FAR struct psram_worker_s *worker = arg;
  FAR struct psram_probe_s *probe = worker->probe;
  size_t base = worker->cpu * probe->words;
  unsigned int loop;
  size_t i;
  int cpu;

  while (sem_wait(&probe->start) < 0)
    {
      if (errno != EINTR)
        {
          abort();
        }
    }

  if (__atomic_load_n(&probe->cancel, __ATOMIC_ACQUIRE))
    {
      return NULL;
    }

  cpu = sched_getcpu();
  printf("smp_psram: worker CPU%u running on CPU%d\n", worker->cpu, cpu);
  if (cpu != worker->cpu)
    {
      worker->errors++;
    }

  for (loop = 0; loop < probe->loops; loop++)
    {
      for (i = 0; i < probe->words; i++)
        {
          probe->buffer[base + i] = psram_pattern(i, loop, worker->cpu);
        }

      psram_verify(worker, loop, worker->cpu);
      pthread_barrier_wait(&probe->barrier);

      /* Both producers have stopped writing. Verify the other CPU's data
       * without cache maintenance, then wait before starting the next round.
       */

      psram_verify(worker, loop, 1 - worker->cpu);
      pthread_barrier_wait(&probe->barrier);
    }

  printf("smp_psram: CPU%u completed %u rounds errors=%u\n",
         worker->cpu, probe->loops, worker->errors);
  return NULL;
}

static unsigned long psram_argument(FAR const char *text, unsigned long max)
{
  FAR char *end;
  unsigned long value;

  errno = 0;
  value = strtoul(text, &end, 10);
  if (errno != 0 || *text == '\0' || *end != '\0' ||
      value == 0 || value > max)
    {
      return 0;
    }

  return value;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct psram_probe_s probe;
  struct psram_worker_s workers[2];
  pthread_t threads[2];
  pthread_attr_t attr;
  cpu_set_t cpuset;
  FAR void *buffer;
  size_t bytes;
  unsigned long mib = 8;
  unsigned long loops = 16;
  unsigned int created = 0;
  unsigned int cpu;
  int ret;

  if (argc > 1)
    {
      mib = psram_argument(argv[1], 16);
    }

  if (argc > 2)
    {
      loops = psram_argument(argv[2], 1024);
    }

  if (argc > 3 || mib == 0 || loops == 0)
    {
      fprintf(stderr, "Usage: smp_psram [MiB:1..16] [rounds:1..1024]\n");
      return EXIT_FAILURE;
    }

  bytes = mib * PROBE_MIB;
  buffer = memalign(64, bytes);
  if (buffer == NULL)
    {
      perror("smp_psram: allocate");
      return EXIT_FAILURE;
    }

  if ((uintptr_t)buffer < PSRAM_LOW ||
      (uintptr_t)buffer + bytes > PSRAM_HIGH)
    {
      fprintf(stderr, "smp_psram: buffer %p is outside PSRAM\n", buffer);
      free(buffer);
      return EXIT_FAILURE;
    }

  memset(&probe, 0, sizeof(probe));
  memset(workers, 0, sizeof(workers));
  probe.buffer = buffer;
  probe.words = bytes / (2 * sizeof(uint32_t));
  probe.loops = loops;
  if (sem_init(&probe.start, 0, 0) < 0)
    {
      free(buffer);
      return EXIT_FAILURE;
    }

  if (sem_setprotocol(&probe.start, SEM_PRIO_NONE) < 0)
    {
      sem_destroy(&probe.start);
      free(buffer);
      return EXIT_FAILURE;
    }

  ret = pthread_barrier_init(&probe.barrier, NULL, 2);
  if (ret != 0)
    {
      sem_destroy(&probe.start);
      free(buffer);
      return EXIT_FAILURE;
    }

  printf("smp_psram: buffer=%p bytes=%zu rounds=%lu\n",
         buffer, bytes, loops);
  for (cpu = 0; cpu < 2; cpu++)
    {
      workers[cpu].probe = &probe;
      workers[cpu].cpu = cpu;
      ret = pthread_attr_init(&attr);
      if (ret != 0)
        {
          break;
        }

      CPU_ZERO(&cpuset);
      CPU_SET(cpu, &cpuset);
      ret = pthread_attr_setaffinity_np(&attr, sizeof(cpuset), &cpuset);
      if (ret == 0)
        {
          ret = pthread_attr_setstacksize(&attr, 4096);
        }

      if (ret == 0)
        {
          ret = pthread_create(&threads[cpu], &attr, psram_worker,
                               &workers[cpu]);
        }

      pthread_attr_destroy(&attr);
      if (ret != 0)
        {
          break;
        }

      created++;
    }

  if (created != 2)
    {
      fprintf(stderr, "smp_psram: create CPU%u worker failed: %d\n",
              cpu, ret);
      __atomic_store_n(&probe.cancel, true, __ATOMIC_RELEASE);
    }

  for (cpu = 0; cpu < created; cpu++)
    {
      sem_post(&probe.start);
    }

  for (cpu = 0; cpu < created; cpu++)
    {
      pthread_join(threads[cpu], NULL);
    }

  pthread_barrier_destroy(&probe.barrier);
  sem_destroy(&probe.start);
  free(buffer);
  if (created != 2 || workers[0].errors != 0 || workers[1].errors != 0)
    {
      puts("smp_psram: FAIL");
      return EXIT_FAILURE;
    }

  puts("smp_psram: PASS concurrent writes and cross-core reads");
  return EXIT_SUCCESS;
}
