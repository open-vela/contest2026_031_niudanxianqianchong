# SmartHome 内嵌 LVGL 图标库

产品图标的唯一视觉源为：

```text
quickapp/smart_home_ui/prototype-web/assets/icons/*.png
```

`scripts/generate_smart_home_lvgl_icons.py` 会裁去透明边距、统一留白并生成
LVGL v9 的 `LV_COLOR_FORMAT_A8` C 图片。生成结果位于 `generated/`，直接链接
进固件，不依赖 LittleFS 或 PNG 解码器。

品牌 Logo `openvela.png` 例外：保留完整画布、蓝白颜色和透明圆角，生成
36 px 的 `LV_COLOR_FORMAT_ARGB8888` 图片，不经过单色图标的裁边和 80% 留白处理。
其字节顺序为 BGRA，适用于 ESP32-P4 的小端布局。

## 尺寸策略

| 尺寸 | 范围 | 用途 |
| --- | --- | --- |
| 32 px | 单色源图标 | 设备、场景、更多页面的标准卡片图标 |
| 20 px | 顶栏、导航、传感器等 16 个高频图标 | 小尺寸状态与导航 |
| 48 px | 首页的天气、空调、摄像头、Agent 等 8 个强调图标 | 首页视觉锚点 |
| 36 px | `openvela.png` 彩色 Logo | 各页面共用顶栏左侧品牌标志 |

图标调用使用稳定的逻辑名称而不是 C 符号：

```c
smart_home_lvgl_icon_create(parent, "asset:camera", 32, 32);
```

工厂会按目标尺寸自动选择最接近的原生 A8 资源。A8 图像仅保存透明度，颜色由
LVGL 的 `image_recolor` 样式决定，因此同一图标可用于普通、选中、禁用及告警状态。
彩色 Logo 使用同一查询表与工厂，通过 `asset:openvela` 接入，工厂保留原始颜色。

> ESP32-P4 当前使用 LVGL 软件渲染器。尽管 A4 更省 Flash，该后端未实现 A4
> 绘制，故单色资源固定使用 A8，确保真机可见；彩色 Logo 使用 ARGB8888。

## 更新流程

```bash
python3 contest2026_031_niudanxianqianchong/scripts/generate_smart_home_lvgl_icons.py
git diff --check
```

必须提交以下三类文件：源 PNG、生成的 `generated/*.c` 以及
`smart_home_lvgl_png_icons.[ch]`。`Makefile` 和 `CMakeLists.txt` 已自动收录
`generated/*.c`，新增源 PNG 后不需手工维护编译源清单。

旧的 Font Awesome 单 glyph 图标源可保留作历史参考，但不再被 SmartHome 产品 UI
链接或使用，避免与此 PNG 图标集混用。
