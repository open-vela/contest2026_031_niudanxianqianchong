# LVGL Host 回归

使用实际 LVGL 和界面代码，设备服务由内存快照替代，不连接网关或硬件。

```sh
cmake -S demos/smart_home/tests/lvgl -B /tmp/smart-home-lvgl-tests -DOPENVELA_ROOT=/home/arongw/openvela
cmake --build /tmp/smart-home-lvgl-tests -j8
ctest --test-dir /tmp/smart-home-lvgl-tests --output-on-failure
```

Host 测试验证筛选、状态和交互逻辑，不代表固件链接、板端帧率或触摸验收。
