# KWS 推理与特征前端宿主测试

本目录的测试在 PC 上运行，不依赖开发板、不需要烧录，用于在板测前锁定
KWS 链路的行为契约。板测只回答「目标上跑多快、唤醒率多少」，
这里回答「逻辑对不对」。

## 前置

- 真实 TFLM 源码：`apps/mlearning/tflite-micro/tflite-micro`
- 真实模型数组：`demos/smart_home/src/voice/kws/model.cc`
- flatbuffers / gemmlowp / ruy 源码（apps 树内已具备）

## 1. 特征前端自测（纯 C，秒级）

检查输出长度、取值范围、静音下界、单频落带、差分关系、可重复性与
参数校验，共 15 项。

```bash
cd demos/smart_home/tests/kws_frontend
gcc -O2 -Wall -Wextra -I../../src/voice \
    frontend_selftest.c ../../src/voice/smart_home_kws_frontend.c \
    -lm -o /tmp/frontend_selftest
/tmp/frontend_selftest
```

期望最后一行：`PASS (0 failures)`。

## 2. 推理生命周期自测（C++ + TFLM + ASan）

覆盖：

- arena 不足时 init 返回 `-ENOMEM` 且内部状态干净，可重新初始化
- 未对齐 arena 可用
- 其他 arena 接管返回 `-EBUSY`，`deinit` 也会拒绝
- 10 次 init / 20 次推理 / deinit 循环，200 次输出逐位一致
- 失败路径无泄漏（ASan + LeakSanitizer）

```bash
cd /home/arongw/openvela
python3 out/voice-gates-20261102/build-host-kws.py   # 见下方说明
out/kws-chain-host/kws-host
```

若脚本路径变化，按下面的最小命令手工编译（需要把 59 个 TFLM 源文件
一并编入，脚本已处理过滤）：

```bash
g++ -std=c++17 -O1 -g -fsanitize=address -fno-omit-frame-pointer \
    -ffunction-sections -fdata-sections \
    -DTFLITE_EMULATE_FLOAT -DTF_LITE_DISABLE_X86_NEON \
    -I apps/mlearning/tflite-micro/tflite-micro \
    -I apps/system/flatbuffers/include \
    -I apps/system/flatbuffers/flatbuffers/include \
    -I apps/math/gemmlowp/gemmlowp -I apps/math/ruy/ruy \
    -I demos/smart_home/src/voice/kws \
    <TFLM 源文件...> \
    demos/smart_home/src/voice/kws/kws_infer.cc \
    demos/smart_home/src/voice/kws/model.cc \
    demos/smart_home/tests/kws_infer/kws_infer_restart.cc \
    -o /tmp/kws_restart
/tmp/kws_restart
```

## 3. 重启语义自测（对应常驻服务的 stop → start）

旧实现中 `kws_service_stop()` 只释放 arena 而不销毁解释器，下次
`kws_infer_init()` 命中「幂等」分支直接返回 0，解释器继续指向已释放内存。
`kws_infer_restart.cc` 用 `malloc/free` 复刻这条路径，配合 ASan 会在
释放后立刻报 use-after-free。

```bash
out/kws-chain-host/kws-host   # 由 build-host-kws.py 与 restart 源码构建
```

期望：`PASS: restart semantics, deinit(NULL), 10 alloc/init/run/deinit/free cycles`

## 与板测的分工

| 项目 | 宿主（本目录） | 板端 |
| --- | --- | --- |
| 前端算法正确性 | ✅ 全量 | 只做一致性抽查 |
| 推理生命周期 | ✅ ASan | 10 次重建 + 冷启动 |
| Invoke 延迟 | ❌ 无意义 | ✅ `kws_smoke` P95 |
| 唤醒率 | ❌ 无麦克风 | ✅ 真实房间 10 次 |

宿主通过不代表板端可用；板端指标也不代表算法对齐。两者都要过，
且顺序应是先宿主、后板端——否则板端失败时无法区分是环境还是逻辑。
