/* SPDX-License-Identifier: Apache-2.0 */

/****************************************************************************
 * demos/smart_home/src/camera/smart_home_snapshot_store.c
 *
 * 两槽滚动快照存储（详见头文件说明）。
 * 调用者：camera 工具 handler（agent worker 串行），本模块不自带锁。
 ****************************************************************************/

#include "smart_home_snapshot_store.h"

#include <errno.h>
#include <fcntl.h>
#include <nuttx/config.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../config/cjson_compat.h"

#ifndef CONFIG_SMART_HOME_DEMO_DATA_ROOT
#  define CONFIG_SMART_HOME_DEMO_DATA_ROOT "/data"
#endif

#define SNAP_DIR      CONFIG_SMART_HOME_DEMO_DATA_ROOT "/smart_home/snapshots"
#define SNAP_INDEX    SNAP_DIR "/latest.json"
#define SNAP_TMP_SUFFIX ".tmp"

static const char *snapshot_dir(void)
{
  return SNAP_DIR;
}

const char *smart_home_snapshot_store_dir(void)
{
  return snapshot_dir();
}

static int ensure_dir(const char *path)
{
  struct stat info;

  if (mkdir(path, 0755) == 0)
    {
      return 0;
    }

  if (errno == EEXIST && stat(path, &info) == 0 && S_ISDIR(info.st_mode))
    {
      return 0;
    }

  return -errno;
}

static int write_all(int fd, const void *data, size_t size)
{
  const uint8_t *cursor = data;

  while (size > 0)
    {
      ssize_t written = write(fd, cursor, size);

      if (written < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }
          return -errno;
        }

      cursor += written;
      size -= (size_t)written;
    }

  return 0;
}

/* tmp + rename，保证 latest.json 原子可见。 */
static int write_file_atomic(const char *path, const void *data, size_t size)
{
  char tmp[SMART_HOME_SNAPSHOT_PATH_MAX + 8];
  struct stat info;
  int fd;
  int ret;
  int n;

  n = snprintf(tmp, sizeof(tmp), "%s%s", path, SNAP_TMP_SUFFIX);
  if (n < 0 || (size_t)n >= sizeof(tmp))
    {
      return -ENAMETOOLONG;
    }

  fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    {
      return -errno;
    }

  ret = write_all(fd, data, size);
  if (ret == 0 && fsync(fd) != 0)
    {
      ret = -errno;
    }

  close(fd);
  if (ret != 0)
    {
      unlink(tmp);
      return ret;
    }

  if (rename(tmp, path) != 0 || stat(path, &info) != 0)
    {
      unlink(tmp);
      return -errno;
    }

  return 0;
}

static void snapshot_path(char *out, size_t size, uint32_t slot)
{
  snprintf(out, size, "%s/shot-%u.rgb565", snapshot_dir(), (unsigned)slot);
}

static uint32_t latest_seq(void)
{
  struct smart_home_snapshot_info_s info;

  if (smart_home_snapshot_store_latest(&info) != 0)
    {
      return 0;
    }

  return info.seq;
}

int smart_home_snapshot_store_write(const uint8_t *frame, size_t bytes,
                                    uint32_t width, uint32_t height,
                                    uint64_t ts, uint32_t brightness,
                                    uint32_t motion,
                                    struct smart_home_snapshot_info_s *out)
{
  struct smart_home_snapshot_info_s info;
  char json[320];
  int ret;

  if (frame == NULL || bytes == 0)
    {
      return -EINVAL;
    }

  ret = ensure_dir(CONFIG_SMART_HOME_DEMO_DATA_ROOT "/smart_home");
  if (ret == 0)
    {
      ret = ensure_dir(SNAP_DIR);
    }
  if (ret != 0)
    {
      return ret;
    }

  memset(&info, 0, sizeof(info));
  info.seq = latest_seq() + 1u;
  info.slot = info.seq % SMART_HOME_SNAPSHOT_SLOTS;
  info.ts = ts;
  info.brightness = brightness;
  info.motion = motion;
  info.bytes = (uint32_t)bytes;
  info.width = width;
  info.height = height;
  snapshot_path(info.path, sizeof(info.path), info.slot);

  ret = write_file_atomic(info.path, frame, bytes);
  if (ret != 0)
    {
      return ret;
    }

  snprintf(json, sizeof(json),
           "{\"version\":1,\"seq\":%lu,\"slot\":%u,\"ts\":%llu,"
           "\"brightness\":%u,\"motion\":%u,\"bytes\":%u,"
           "\"width\":%u,\"height\":%u,\"format\":\"rgb565\","
           "\"path\":\"%s\"}",
           (unsigned long)info.seq, (unsigned)info.slot,
           (unsigned long long)info.ts, (unsigned)info.brightness,
           (unsigned)info.motion, (unsigned)info.bytes,
           (unsigned)info.width, (unsigned)info.height, info.path);

  ret = write_file_atomic(SNAP_INDEX, json, strlen(json));
  if (ret != 0)
    {
      return ret;
    }

  if (out != NULL)
    {
      *out = info;
    }

  return 0;
}

int smart_home_snapshot_store_latest(struct smart_home_snapshot_info_s *out)
{
  char buffer[320];
  cJSON *root;
  const cJSON *item;
  ssize_t got;
  int fd;

  if (out == NULL)
    {
      return -EINVAL;
    }

  fd = open(SNAP_INDEX, O_RDONLY);
  if (fd < 0)
    {
      return -errno;
    }

  got = read(fd, buffer, sizeof(buffer) - 1u);
  close(fd);
  if (got <= 0)
    {
      return got < 0 ? -errno : -EIO;
    }

  buffer[got] = '\0';
  root = cJSON_Parse(buffer);
  if (root == NULL)
    {
      return -EIO;
    }

  memset(out, 0, sizeof(*out));
  item = cJSON_GetObjectItemCaseSensitive(root, "seq");
  out->seq = cJSON_IsNumber(item) ? (uint32_t)item->valuedouble : 0u;
  item = cJSON_GetObjectItemCaseSensitive(root, "slot");
  out->slot = cJSON_IsNumber(item) ? (uint32_t)item->valuedouble : 0u;
  item = cJSON_GetObjectItemCaseSensitive(root, "ts");
  out->ts = cJSON_IsNumber(item) ? (uint64_t)item->valuedouble : 0u;
  item = cJSON_GetObjectItemCaseSensitive(root, "brightness");
  out->brightness = cJSON_IsNumber(item) ? (uint32_t)item->valuedouble : 0u;
  item = cJSON_GetObjectItemCaseSensitive(root, "motion");
  out->motion = cJSON_IsNumber(item) ? (uint32_t)item->valuedouble : 0u;
  item = cJSON_GetObjectItemCaseSensitive(root, "bytes");
  out->bytes = cJSON_IsNumber(item) ? (uint32_t)item->valuedouble : 0u;
  item = cJSON_GetObjectItemCaseSensitive(root, "width");
  out->width = cJSON_IsNumber(item) ? (uint32_t)item->valuedouble : 0u;
  item = cJSON_GetObjectItemCaseSensitive(root, "height");
  out->height = cJSON_IsNumber(item) ? (uint32_t)item->valuedouble : 0u;
  item = cJSON_GetObjectItemCaseSensitive(root, "path");
  if (cJSON_IsString(item) && item->valuestring[0])
    {
      strncpy(out->path, item->valuestring, sizeof(out->path) - 1u);
    }

  cJSON_Delete(root);
  return 0;
}
