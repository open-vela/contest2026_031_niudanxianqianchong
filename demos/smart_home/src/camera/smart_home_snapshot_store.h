/* SPDX-License-Identifier: Apache-2.0 */

#ifndef SMART_HOME_SNAPSHOT_STORE_H
#define SMART_HOME_SNAPSHOT_STORE_H

#include <stddef.h>
#include <stdint.h>

/* 两槽滚动快照存储。
 *
 * /data 分区余量有限（10 MiB 内 7.9 MB 已被完整字体占用），因此快照
 * 只保留两张：shot-0.rgb565 / shot-1.rgb565（各 512x300x2 = 307200 B），
 * 按 seq % 2 覆盖旧槽，外加 latest.json 记录最新一张的元数据。
 * 所有写入走 tmp + rename，断电时要么旧文件要么新文件。 */

#define SMART_HOME_SNAPSHOT_SLOTS     2u
#define SMART_HOME_SNAPSHOT_TS_MAX    20u
#define SMART_HOME_SNAPSHOT_PATH_MAX  96

struct smart_home_snapshot_info_s
{
  uint32_t seq;
  uint32_t slot;
  uint64_t ts;          /* epoch seconds；0 表示墙钟不可用 */
  uint32_t brightness;  /* 0..255 */
  uint32_t motion;      /* 相对上一张的变化量，permille */
  uint32_t bytes;
  uint32_t width;
  uint32_t height;
  char path[SMART_HOME_SNAPSHOT_PATH_MAX];
};

/* 写入一帧原始 RGB565 并更新 latest.json。
 * frame/bytes 必须是 512x300x2；返回 0 或负 errno。 */
int smart_home_snapshot_store_write(const uint8_t *frame, size_t bytes,
                                    uint32_t width, uint32_t height,
                                    uint64_t ts, uint32_t brightness,
                                    uint32_t motion,
                                    struct smart_home_snapshot_info_s *out);

/* 读取 latest.json；无快照时返回 -ENOENT。 */
int smart_home_snapshot_store_latest(struct smart_home_snapshot_info_s *out);

const char *smart_home_snapshot_store_dir(void);

#endif /* SMART_HOME_SNAPSHOT_STORE_H */
