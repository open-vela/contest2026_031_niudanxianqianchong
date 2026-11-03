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
默认共 19 项 CTest（含文件测试准备）。

语音设置覆盖 ASR/TTS 同时启用、仅 ASR、仅 TTS，以及全未启用时的提示。
通过实际分类与页签回调验证切换、草稿保留、连接设置返回和无滚动布局；
保存按钮下沿不得超出设置面板，音色输入框必须保持在键盘上方。
`voice_settings_check` 可用 `SMART_HOME_VOICE_ASR_CAPTURE` 和
`SMART_HOME_VOICE_TTS_CAPTURE` 导出右栏预览；该测试的顶栏与底栏为替身，
完整页面外壳以 `panel_check` 截图为准。

首页回归包含单灯默认值、板载摄像头保留、米家绑定成功返回合并首页，
聊天输入与文本垂直居中，以及模拟指针按下「稍后再说」时弹窗位置和滚动量保持不变。
另覆盖加号随房间增删移动、首页独立时钟字号、卡片开关状态精简，以及当前 Wi-Fi
名称不随输入草稿改变；网络主机测试以替身验证连接确认、DNS 失败、断线与恢复。

增加 `-DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -g'`
并使用独立构建目录，可运行 ASan/UBSan 回归。

当前 V2 整合保持 `SMART_HOME_TEST_NODE_GATEWAY=OFF`：Node 附加模块仍依赖 V1
的 `cagent/types.h`，开启此选项会在配置阶段明确报错，需单独完成附加模块迁移。
