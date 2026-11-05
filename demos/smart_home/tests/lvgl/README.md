# LVGL Host 回归

使用实际 LVGL 和界面代码，设备服务、模型和会话读取由测试替身提供，不连接网关或硬件。
`session_file_check` 单独使用真实文件和仓库内的 JSONL 存储实现。

```sh
cmake -S demos/smart_home/tests/lvgl -B /tmp/smart-home-lvgl-tests -DOPENVELA_ROOT=/home/arongw/openvela
cmake --build /tmp/smart-home-lvgl-tests -j8
ctest --test-dir /tmp/smart-home-lvgl-tests --output-on-failure
```

Host 测试验证筛选、状态和交互逻辑，不代表固件链接、板端帧率或触摸验收。

首页回归只构建 V9 页面，覆盖房间筛选、操作失败回滚、天气占位、跨日时钟、
设备卡片与控制抽屉。设置 `SMART_HOME_V9_CAPTURE=/tmp/home.ppm` 运行
`panel_check` 可导出 RGB565 首页渲染；保留该测试目标名称以兼容已有检查脚本。

交互回归通过真实 LVGL 分类、导航、卡片、场景与设置控件触发回调，检查页面切换、
状态刷新和中文反馈。
工具权限回归验证延后枚举、每次定时器只更新一张卡片、分页与对象复用、缓存不重复分配、
忙碌回退、目录重排后的控制对象、空目录、分配失败和待处理定时器的退出清理。


聊天回归覆盖 V2 事件借用数据复制、完成通知、单次错误反馈、截断回复、历史保存失败、
操作卡片、忙碌时草稿保护、新对话及历史恢复。文件回归覆盖容量轮转和写入/同步失败；
CTest 会在每次运行前清空构建目录内的 `session-data` 测试数据。
默认共 21 项 CTest（含文件测试准备）。

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

设置回归默认覆盖 MCP 和 Node 的启停、状态、配置及节点列表；服务由替身提供。
`-DSMART_HOME_TEST_NODE_GATEWAY=ON` 可额外验证首页集成构建启用 Node 的组合。
