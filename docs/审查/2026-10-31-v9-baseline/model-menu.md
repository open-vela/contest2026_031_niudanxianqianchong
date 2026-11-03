# 模型服务：切换后端的菜单比对（v9 基准）

| 项 | 设计 23-model-menu | Host 实际 |
| --- | --- | --- |
| 截图 | `designs/png/23-model-menu.png` | `set-model-menu.png` |
| 选项数 | 5（DeepSeek / MiMo / Qwen / OpenAI / 自定义） | 5（DeepSeek / MiMo / Qwen / OpenAI / Custom） |
| 菜单形式 | 覆盖在下拉框下方的浮层卡片 | LVGL `lv_dropdown` 列表 |
| 菜单左边界 | x=273（与下拉框对齐） | x=272 |
| 菜单宽度 | 332 | 336 |
| 行高 | 46 | 38 |
| 首项文字行 | 241–257 | 219–235 |
| 第 2 项文字行 | 291–308 | 262–299 |
| 第 5 项文字行 | 475–489 | 426–438 |
| 选中底色 | `#E1EDF0`（雾蓝），324×44，圆角 | **`#2095F6`（LVGL 默认蓝）**，332×38 |
| 列表容器 | 圆角卡片 + 1px 边框 | 无独立容器样式 |

## 一致

- 选项数量与顺序一致。
- 菜单贴下拉框左下，横向位置一致（272 vs 273）。
- 选项文字字号一致（17）。
- 选中项落在正确的位置（DeepSeek 为第 1 项）。

## 不一致

### 1. 选中底色用了 LVGL 默认蓝（最显眼）

实测 `#2095F6`，即 LVGL 主题默认的 `LV_PART_SELECTED` 蓝。设计是 `#E1EDF0` 雾蓝。

原因在 `smart_home_lvgl_style_dropdown()`：

```c
smart_home_lvgl_set_bg(list, SMART_HOME_UI_COLOR_SURFACE);
lv_obj_set_style_bg_color(list, SMART_HOME_UI_COLOR_SURFACE_ON, LV_PART_SELECTED);
```

只设了 `LV_PART_SELECTED` 的底色，没设 `bg_opa`，LVGL 默认 `LV_OPA_COVER` 下仍会叠主题的渐变/边框；而列表容器本身也没有圆角与边框样式。需要在 list 上补齐 `radius`、`border`、`pad`，并在 `LV_PART_SELECTED` 上显式设 `bg_opa = LV_OPA_COVER`。

### 2. 行高 38 vs 46

设计每项 46 px，Host 38 px。五项累计差 40 px：设计菜单底边到 y≈500，Host 到 y≈460。

`lv_dropdown` 的行高由 `list` 的 `pad_ver` 与字体行高决定，当前 `smart_home_lvgl_style_dropdown()` 没设 `pad_ver`，用的是 LVGL 默认 6 px。

### 3. 菜单没有独立容器外观

设计里菜单是一张带圆角（约 12 px）和 1px 边框的卡片，与下拉框之间有 2 px 间隙。Host 的列表直接平铺，没有圆角/边框/阴影。

### 4. 首项偏移

设计首项文字基线在 y=257，Host 在 y=235——因为 Host 列表顶部紧贴下拉框底边（y≈204），设计留了 8 px 内边距。

## 结论

**功能一致、样式不一致**：五个后端、顺序、选中位置都对，但菜单外观仍是 LVGL 默认主题（系统蓝 + 无容器样式 + 行高偏小），与设计的雾蓝卡片差异明显。

修复集中在 `smart_home_lvgl_style_dropdown()` 一个函数：补列表容器的圆角/边框/内边距，显式覆盖 `LV_PART_SELECTED` 的底色与不透明度，并把行高调到 46。改动小、影响面只有设置页的四个下拉（模型服务、请求超时同类控件）。

## 复现

```sh
cmake --build /tmp/main-lvgl -j8
D=docs/审查/2026-10-31-v9-baseline
SMART_HOME_V9_MODEL_MENU_CAPTURE=$D/set-model-menu.ppm /tmp/main-lvgl/panel_check
```

注意：`panel_check` 的后端桩原先只返回 1 个后端，菜单只有一项无法比对；已改为与生产一致的 5 个。
