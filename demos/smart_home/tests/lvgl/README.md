# LVGL Host 回归

使用实际 LVGL 和界面代码，设备服务、模型和会话读取由测试替身提供，不连接网关或硬件。
`session_file_check` 单独使用真实文件和仓库内的 JSONL 存储实现。

```sh
cmake -S demos/smart_home/tests/lvgl -B /tmp/smart-home-lvgl-tests -DOPENVELA_ROOT=/home/arongw/openvela
cmake --build /tmp/smart-home-lvgl-tests -j8
ctest --test-dir /tmp/smart-home-lvgl-tests --output-on-failure
```

Host 测试验证筛选、状态和交互逻辑，不代表固件链接、板端帧率或触摸验收。

回归覆盖房间筛选、操作失败回滚、状态占位、跨日时钟、卡片/抽屉对象复用、
滚动保持、设备移除与等待回读超时。设置 `SMART_HOME_LVGL_CAPTURE=/tmp/home.ppm`
运行 `panel_check` 可导出 RGB565 Host 首页渲染。

按压回归使用 LVGL 模拟指针输入，检查导航点击与手势取消、RGB565 按压/释放像素、
选中状态恢复和禁用态；设置回归检查中文状态文案。

聊天回归覆盖 V2 事件借用数据复制、完成通知、单次错误反馈、截断回复、历史保存失败、
操作卡片、忙碌时草稿保护、新对话及历史恢复。文件回归覆盖容量轮转和写入/同步失败；
CTest 会在每次运行前清空构建目录内的 `session-data` 测试数据。
共 8 项 CTest（含 1 项文件测试准备）。

增加 `-DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -g'`
并使用独立构建目录，可运行 ASan/UBSan 回归。

当前 V2 整合保持 `SMART_HOME_TEST_NODE_GATEWAY=OFF`：Node 附加模块仍依赖 V1
的 `cagent/types.h`，开启此选项会在配置阶段明确报错，需单独完成附加模块迁移。
