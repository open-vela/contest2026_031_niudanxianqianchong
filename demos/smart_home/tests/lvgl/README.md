# LVGL Host 回归

使用实际 LVGL 和界面代码，设备服务由内存快照替代，不连接网关或硬件。

```sh
cmake -S demos/smart_home/tests/lvgl -B /tmp/smart-home-lvgl-tests -DOPENVELA_ROOT=/home/arongw/openvela
cmake --build /tmp/smart-home-lvgl-tests -j8
ctest --test-dir /tmp/smart-home-lvgl-tests --output-on-failure
```

Host 测试验证筛选、状态和交互逻辑，不代表固件链接、板端帧率或触摸验收。

增加 `-DSMART_HOME_TEST_NODE_GATEWAY=ON` 可验证 Node 卡片的复用与筛选。
回归覆盖房间筛选、操作失败回滚、状态占位、跨日时钟、卡片/抽屉对象复用、
滚动保持、设备移除与等待回读超时。设置 `SMART_HOME_LVGL_CAPTURE=/tmp/home.ppm`
运行 `panel_check` 可导出 RGB565 Host 首页渲染。
