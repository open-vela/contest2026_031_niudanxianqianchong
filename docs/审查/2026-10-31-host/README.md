# 2026-10-31 Host 截图（供审查）

1024×600，由 `panel_check` 真实构建 V9 界面后导出；颜色为 RGB565 量化后的近似值（导出按 `*255/31`、`*255/63` 换算，236 会显示成 238）。

| 文件 | 内容 | 备注 |
| --- | --- | --- |
| `home.png` | V9 首页（6 张设备卡、房间筛选、家庭模式、顶栏） | **底部导航栏缺失**，见下 |
| `settings.png` | V9 设置 · 第一个分类 | |
| `settings-network.png` | V9 设置 · 网络分类（SSID/密码表单） | |
| `settings-model.png` | V9 设置 · 模型分类（后端/Key/超时/词元） | |
| `legacy-home.png` | 旧首页（已被 V9 替换，仅作对比） | 有底栏，可对照 |

重新生成：

```sh
cmake --build /tmp/sh-lvgl-tests -j8
D=docs/审查/2026-10-31-host
SMART_HOME_V9_CAPTURE=$D/home.ppm \
SMART_HOME_V9_SETTINGS_CAPTURE=$D/settings.ppm \
SMART_HOME_V9_NETWORK_CAPTURE=$D/settings-network.ppm \
SMART_HOME_V9_MODEL_CAPTURE=$D/settings-model.ppm \
SMART_HOME_LVGL_CAPTURE=$D/legacy-home.ppm \
  /tmp/sh-lvgl-tests/panel_check
```

## 已知问题（2026-10-31）

- `home.png` 底部 64px 是纯底色：`build_home_screen_v9()` 没有调用 `smart_home_lvgl_build_nav_bar()`，而 chat 与 settings_v9 都调用了。设计 3.2 要求底栏 `(0,536,1024,64)` 三页一致。
- 导航触区实现为 216×52 @y=542，设计是 216×56 @y=540，差 4px。
- “新增房间”按钮实现 48×40，设计写 48×48 触区。

以上三项尚未修改，等确认范围后一并处理。
