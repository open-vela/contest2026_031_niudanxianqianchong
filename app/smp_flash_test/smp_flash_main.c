/****************************************************************************
 * app/smp_flash_test/smp_flash_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define FLASH_MOUNT_POINT   "/data"
#define FLASH_DEVICE        "/dev/espflash"
#define FLASH_FSTYPE        "littlefs"
#define SHARED_PATH         FLASH_MOUNT_POINT "/smp_shared.bin"
#define SHARED_SLOT_SIZE    512
#define MAX_CHUNK_SIZE      (16 * 1024)
#define DEFAULT_CHUNK_SIZE  (16 * 1024)
#define DEFAULT_ROUNDS      8

#define REC_MAGIC           0x534d5046u  /* "SMPF" */

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct flash_record_s
{
  uint32_t magic;
  uint32_t cpu;
  uint32_t round;
  uint32_t length;
  uint32_t checksum;
  uint8_t  payload[SHARED_SLOT_SIZE - 5 * sizeof(uint32_t)];
};

struct flash_worker_s
{
  unsigned int cpu;
  unsigned int rounds;
  size_t chunk;
  cpu_set_t cpuset;
  pthread_barrier_t *write_barrier;
  pthread_barrier_t *verify_barrier;
  unsigned int errors;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Keep the I/O buffers in internal SRAM: flash erase/write temporarily
 * disables the cache, and the HAL expects internal-memory source buffers.
 */

static uint8_t g_buffers[2][MAX_CHUNK_SIZE];

/* LittleFS serializes VFS calls internally, but an O_APPEND update made
 * through two separate file descriptions computes its offset from each
 * object's cached position.  Same-file updates therefore need an
 * application-level lock; the test uses one to model the supported
 * contract and to keep the shared slot checks meaningful.
 */

static pthread_mutex_t g_shared_lock = PTHREAD_MUTEX_INITIALIZER;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t flash_pattern(unsigned int cpu, unsigned int round,
                              size_t index)
{
  return (uint32_t)index ^ (0x9e3779b9u * (round + 1)) ^
         (0x13579bdfu << cpu);
}

static void flash_fill(uint8_t *buffer, size_t length, unsigned int cpu,
                       unsigned int round)
{
  size_t i;

  for (i = 0; i < length; i++)
    {
      buffer[i] = (uint8_t)(flash_pattern(cpu, round, i) >> ((i & 3) * 8));
    }
}

static int flash_verify(const uint8_t *buffer, size_t length,
                        unsigned int cpu, unsigned int round,
                        unsigned int *errors)
{
  size_t i;

  for (i = 0; i < length; i++)
    {
      uint8_t expected =
        (uint8_t)(flash_pattern(cpu, round, i) >> ((i & 3) * 8));

      if (buffer[i] != expected)
        {
          if (*errors < 4)
            {
              _err("mismatch cpu%u round%u offset=%zu got=%02x want=%02x\n",
                   cpu, round, i, buffer[i], expected);
            }

          (*errors)++;
          return -EIO;
        }
    }

  return OK;
}

static ssize_t flash_write_all(int fd, const uint8_t *buffer, size_t length)
{
  size_t done = 0;

  while (done < length)
    {
      ssize_t ret = write(fd, buffer + done, length - done);

      if (ret < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return ret;
        }

      if (ret == 0)
        {
          return -EIO;
        }

      done += ret;
    }

  return (ssize_t)done;
}

static ssize_t flash_read_all(int fd, uint8_t *buffer, size_t length)
{
  size_t done = 0;

  while (done < length)
    {
      ssize_t ret = read(fd, buffer + done, length - done);

      if (ret < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return ret;
        }

      if (ret == 0)
        {
          return -EIO;
        }

      done += ret;
    }

  return (ssize_t)done;
}

static void flash_record_fill(FAR struct flash_record_s *record,
                              unsigned int cpu, unsigned int round)
{
  uint32_t sum = 0x811c9dc5u;
  size_t i;

  memset(record, 0, sizeof(*record));
  record->magic = REC_MAGIC;
  record->cpu = cpu;
  record->round = round;
  record->length = sizeof(record->payload);

  for (i = 0; i < sizeof(record->payload); i++)
    {
      record->payload[i] = (uint8_t)(flash_pattern(cpu, round, i) & 0xff);
      sum = (sum ^ record->payload[i]) * 0x01000193u;
    }

  record->checksum = sum;
}

static int flash_record_check(FAR const struct flash_record_s *record,
                              unsigned int cpu, unsigned int round)
{
  uint32_t sum = 0x811c9dc5u;
  size_t i;

  if (record->magic != REC_MAGIC || record->cpu != cpu ||
      record->round != round || record->length != sizeof(record->payload))
    {
      _err("rec hdr want cpu%u round%u got magic=%08" PRIx32
           " cpu=%" PRIu32 " round=%" PRIu32 " len=%" PRIu32 "\n",
           cpu, round, record->magic, record->cpu, record->round,
           record->length);
      return -EINVAL;
    }

  for (i = 0; i < sizeof(record->payload); i++)
    {
      uint8_t expected =
        (uint8_t)(flash_pattern(cpu, round, i) & 0xff);

      if (record->payload[i] != expected)
        {
          return -EIO;
        }

      sum = (sum ^ record->payload[i]) * 0x01000193u;
    }

  return record->checksum == sum ? OK : -EIO;
}

static int flash_private_round(FAR struct flash_worker_s *worker,
                               unsigned int round)
{
  FAR uint8_t *buffer = g_buffers[worker->cpu];
  char path[32];
  int fd;
  ssize_t ret;

  snprintf(path, sizeof(path), FLASH_MOUNT_POINT "/smp_cpu%u.bin",
           worker->cpu);

  flash_fill(buffer, worker->chunk, worker->cpu, round);

  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0)
    {
      _err("open(%s, w) failed: %d\n", path, errno);
      return -errno;
    }

  ret = flash_write_all(fd, buffer, worker->chunk);
  if (ret < 0)
    {
      _err("write(%s) failed: %d\n", path, errno);
    }
  else if (fsync(fd) < 0)
    {
      _err("fsync(%s) failed: %d\n", path, errno);
      ret = -errno;
    }

  close(fd);

  if (ret < 0)
    {
      return (int)ret;
    }

  memset(buffer, 0, worker->chunk);

  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      _err("open(%s, r) failed: %d\n", path, errno);
      return -errno;
    }

  ret = flash_read_all(fd, buffer, worker->chunk);
  close(fd);

  if (ret < 0)
    {
      _err("read(%s) failed: %d\n", path, errno);
      return (int)ret;
    }

  return flash_verify(buffer, worker->chunk, worker->cpu, round,
                      &worker->errors);
}

static int flash_shared_round(FAR struct flash_worker_s *worker,
                              unsigned int round)
{
  struct flash_record_s record;
  int fd;
  ssize_t ret;

  flash_record_fill(&record, worker->cpu, round);

  /* NuttX littlefs resolves O_APPEND only once, at open time.  Reopen the
   * file for every record so each append starts from the current EOF.
   */

  fd = open(SHARED_PATH, O_WRONLY | O_APPEND);
  if (fd < 0)
    {
      _err("open shared cpu%u round%u failed: %d\n",
           worker->cpu, round, errno);
      return -EIO;
    }

  ret = write(fd, &record, sizeof(record));
  if (ret != sizeof(record))
    {
      _err("append shared cpu%u round%u failed: ret=%zd errno=%d\n",
           worker->cpu, round, ret, errno);
      close(fd);
      return -EIO;
    }

  if (fsync(fd) < 0)
    {
      _err("fsync shared cpu%u failed: %d\n", worker->cpu, errno);
      close(fd);
      return -EIO;
    }

  close(fd);
  return OK;
}

static int flash_shared_verify(FAR struct flash_worker_s *worker,
                               unsigned int round)
{
  struct flash_record_s records[2];
  off_t expected = (off_t)(round + 1) * 2 * SHARED_SLOT_SIZE;
  unsigned int seen = 0;
  int fd;
  ssize_t ret;
  int i;

  fd = open(SHARED_PATH, O_RDONLY);
  if (fd < 0)
    {
      _err("open shared ro cpu%u failed: %d\n", worker->cpu, errno);
      return -EIO;
    }

  {
    off_t size = lseek(fd, 0, SEEK_END);

    if (size != expected)
      {
        _err("shared size cpu%u round%u is %ld, want %ld\n",
             worker->cpu, round, (long)size, (long)expected);
        close(fd);
        worker->errors++;
        return -EIO;
      }
  }

  if (lseek(fd, expected - 2 * SHARED_SLOT_SIZE, SEEK_SET) < 0)
    {
      close(fd);
      return -EIO;
    }

  ret = flash_read_all(fd, (uint8_t *)records, sizeof(records));
  close(fd);

  if (ret != sizeof(records))
    {
      _err("read tail cpu%u round%u failed: ret=%zd errno=%d\n",
           worker->cpu, round, ret, errno);
      return -EIO;
    }

  for (i = 0; i < 2; i++)
    {
      unsigned int cpu = records[i].cpu;

      if (cpu < 2 && flash_record_check(&records[i], cpu, round) == OK)
        {
          seen |= 1u << cpu;
        }
    }

  if (seen != 0x3)
    {
      _err("tail records cpu%u round%u seen=%02x\n",
           worker->cpu, round, seen);
      worker->errors++;
      return -EIO;
    }

  return OK;
}

static int flash_dir_churn(FAR struct flash_worker_s *worker,
                           unsigned int round)
{
  char path[40];
  uint8_t marker[32];
  int fd;

  snprintf(path, sizeof(path), FLASH_MOUNT_POINT "/tmp_%u_%u",
           worker->cpu, round);
  memset(marker, (int)(worker->cpu * 16 + round), sizeof(marker));

  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0)
    {
      _err("create(%s) failed: %d\n", path, errno);
      return -errno;
    }

  if (write(fd, marker, sizeof(marker)) != sizeof(marker))
    {
      _err("write(%s) failed: %d\n", path, errno);
      close(fd);
      return -EIO;
    }

  close(fd);

  if (unlink(path) < 0)
    {
      _err("unlink(%s) failed: %d\n", path, errno);
      return -errno;
    }

  return OK;
}

static FAR void *flash_worker(FAR void *arg)
{
  FAR struct flash_worker_s *worker = arg;
  unsigned int round;
  int ret;

  for (round = 0; round < worker->rounds; round++)
    {
      ret = flash_private_round(worker, round);
      if (ret < 0)
        {
          worker->errors++;
        }

      pthread_mutex_lock(&g_shared_lock);
      ret = flash_shared_round(worker, round);
      pthread_mutex_unlock(&g_shared_lock);
      if (ret < 0)
        {
          worker->errors++;
        }

      pthread_barrier_wait(worker->write_barrier);

      /* Both slots for this round are on flash now; verify both. */

      pthread_mutex_lock(&g_shared_lock);
      ret = flash_shared_verify(worker, round);
      pthread_mutex_unlock(&g_shared_lock);
      if (ret < 0)
        {
          worker->errors++;
        }

      ret = flash_dir_churn(worker, round);
      if (ret < 0)
        {
          worker->errors++;
        }

      pthread_barrier_wait(worker->verify_barrier);

      if ((round & 3) == 3)
        {
          printf("smp_flash: cpu%u round %u/%u errors=%u\n",
                 worker->cpu, round + 1, worker->rounds, worker->errors);
        }
    }

  return NULL;
}

static int flash_mount_if_needed(void)
{
  struct stat st;

  if (stat(FLASH_MOUNT_POINT, &st) == 0)
    {
      return OK;
    }

  if (mkdir(FLASH_MOUNT_POINT, 0777) < 0 && errno != EEXIST)
    {
      _err("mkdir(%s) failed: %d\n", FLASH_MOUNT_POINT, errno);
      return -errno;
    }

  if (mount(FLASH_DEVICE, FLASH_MOUNT_POINT, FLASH_FSTYPE, 0, NULL) < 0 &&
      errno != EBUSY)
    {
      _err("mount(%s) failed: %d\n", FLASH_MOUNT_POINT, errno);
      return -errno;
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct flash_worker_s workers[2];
  pthread_barrier_t write_barrier;
  pthread_barrier_t verify_barrier;
  pthread_attr_t attr;
  pthread_t threads[2];
  unsigned int rounds = DEFAULT_ROUNDS;
  size_t chunk = DEFAULT_CHUNK_SIZE;
  int shared_fd;
  int errors = 0;
  int cpu;
  int ret;

  if (argc > 1)
    {
      rounds = (unsigned int)strtoul(argv[1], NULL, 0);
    }

  if (argc > 2)
    {
      chunk = (size_t)strtoul(argv[2], NULL, 0) * 1024;
    }

  if (rounds == 0 || rounds > 1024)
    {
      _err("invalid rounds: %u\n", rounds);
      return EXIT_FAILURE;
    }

  if (chunk == 0 || chunk > MAX_CHUNK_SIZE)
    {
      _err("invalid chunk: %zu (max %u)\n", chunk, MAX_CHUNK_SIZE);
      return EXIT_FAILURE;
    }

  ret = flash_mount_if_needed();
  if (ret < 0)
    {
      return EXIT_FAILURE;
    }

  shared_fd = open(SHARED_PATH, O_RDWR | O_CREAT | O_TRUNC, 0666);
  if (shared_fd < 0)
    {
      _err("open(%s) failed: %d\n", SHARED_PATH, errno);
      return EXIT_FAILURE;
    }


  close(shared_fd);
  shared_fd = -1;

  pthread_barrier_init(&write_barrier, NULL, 2);
  pthread_barrier_init(&verify_barrier, NULL, 2);
  memset(workers, 0, sizeof(workers));

  printf("smp_flash: rounds=%u chunk=%zu bytes shared=%s\n",
         rounds, chunk, SHARED_PATH);

  for (cpu = 0; cpu < 2; cpu++)
    {
      workers[cpu].cpu = (unsigned int)cpu;
      workers[cpu].rounds = rounds;
      workers[cpu].chunk = chunk;
      workers[cpu].write_barrier = &write_barrier;
      workers[cpu].verify_barrier = &verify_barrier;

      pthread_attr_init(&attr);
      pthread_attr_setstacksize(&attr, 8192);
      CPU_ZERO(&workers[cpu].cpuset);

      /* Keep CPU0/CPU1 explicit: both CPUs must enter the MTD path. */

      CPU_SET(cpu, &workers[cpu].cpuset);
      pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t),
                                  &workers[cpu].cpuset);

      ret = pthread_create(&threads[cpu], &attr, flash_worker,
                           &workers[cpu]);
      pthread_attr_destroy(&attr);
      if (ret != 0)
        {
          _err("pthread_create cpu%d failed: %d\n", cpu, ret);
          return EXIT_FAILURE;
        }
    }

  for (cpu = 0; cpu < 2; cpu++)
    {
      pthread_join(threads[cpu], NULL);
      errors += workers[cpu].errors;
    }

  pthread_barrier_destroy(&write_barrier);
  pthread_barrier_destroy(&verify_barrier);

  printf("smp_flash: done rounds=%u chunk=%zu errors=%d %s\n",
         rounds, chunk, errors, errors == 0 ? "PASS" : "FAIL");

  return errors == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
