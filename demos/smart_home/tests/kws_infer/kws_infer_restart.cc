/****************************************************************************
 * demos/smart_home/tests/kws_infer/kws_infer_restart.cc
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * KWS 推理封装的重启语义宿主自测。
 *
 * 复现并锁定「服务 stop 释放 arena 后再次 start」这条路径：
 * 旧实现里 stop() 只 free 不 deinit，下次 init 命中「幂等」分支直接返回 0，
 * 解释器继续指着已释放的 arena。本测试用 malloc/free 模拟该场景，
 * 配合 ASan 可在释放后立即报 use-after-free。
 *
 * 编译见同目录 README；需要真实模型与真实 TFLM。
 ****************************************************************************/

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "kws_infer.h"

#define REQUIRE(x)                                                      \
  do                                                                    \
    {                                                                   \
      if (!(x))                                                         \
        {                                                               \
          std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);      \
          return 1;                                                     \
        }                                                               \
    }                                                                   \
  while (0)

#define ARENA_SIZE 393216

int main()
{
  static int8_t features[KWS_INFER_FEATURE_SIZE];
  float scores[KWS_INFER_CLASS_COUNT];
  float baseline[KWS_INFER_CLASS_COUNT];

  for (unsigned i = 0; i < KWS_INFER_FEATURE_SIZE; i++)
    {
      features[i] = static_cast<int8_t>((i * 37u + 19u) >> 1);
    }

  std::fprintf(stderr, "backend=%d\n", kws_infer_backend());

  /* 第一轮：正常初始化、推理、销毁、释放。 */
  {
    auto *arena = static_cast<uint8_t *>(std::malloc(ARENA_SIZE));

    REQUIRE(arena != nullptr);
    REQUIRE(kws_infer_init(arena, ARENA_SIZE) == 0);
    REQUIRE(kws_infer_ready() == 1);
    REQUIRE(kws_infer_run(features, scores) == 0);
    std::memcpy(baseline, scores, sizeof(baseline));
    REQUIRE(kws_infer_deinit(arena) == 0);
    REQUIRE(kws_infer_ready() == 0);
    std::free(arena);
  }

  /* 释放后再推理必须被拒绝，而不是读野指针。 */
  REQUIRE(kws_infer_run(features, scores) == -EINVAL);

  /* 第二轮：换一块新内存重新初始化，输出必须与第一轮逐位一致。
   * 若第一轮没有正确 deinit，这里会命中残留实例或读到已释放内存。
   */
  {
    auto *arena = static_cast<uint8_t *>(std::malloc(ARENA_SIZE));

    REQUIRE(arena != nullptr);
    REQUIRE(kws_infer_init(arena, ARENA_SIZE) == 0);
    REQUIRE(kws_infer_run(features, scores) == 0);

    for (int c = 0; c < KWS_INFER_CLASS_COUNT; c++)
      {
        REQUIRE(std::isfinite(scores[c]));
        REQUIRE(scores[c] == baseline[c]);
      }

    REQUIRE(kws_infer_deinit(arena) == 0);
    std::free(arena);
  }

  /* 第三轮：deinit(NULL) 也应可用（无条件销毁）。 */
  {
    auto *arena = static_cast<uint8_t *>(std::malloc(ARENA_SIZE));

    REQUIRE(arena != nullptr);
    REQUIRE(kws_infer_init(arena, ARENA_SIZE) == 0);
    REQUIRE(kws_infer_deinit(nullptr) == 0);
    REQUIRE(kws_infer_ready() == 0);
    std::free(arena);
  }

  /* 第四轮：连续 10 次「分配—初始化—推理—销毁—释放」，
   * 检查没有内存增长（配合 ASan 泄漏检查）。
   */
  for (int cycle = 0; cycle < 10; cycle++)
    {
      auto *arena = static_cast<uint8_t *>(std::malloc(ARENA_SIZE));

      REQUIRE(arena != nullptr);
      REQUIRE(kws_infer_init(arena, ARENA_SIZE) == 0);
      REQUIRE(kws_infer_run(features, scores) == 0);
      REQUIRE(kws_infer_deinit(arena) == 0);
      std::free(arena);
    }

  std::fprintf(stderr,
               "PASS: restart semantics, deinit(NULL), 10 alloc/init/run/"
               "deinit/free cycles, outputs bit-identical\n");
  return 0;
}
