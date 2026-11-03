# 首页天气图标（最终 128px）

分支 `feat/v9-weather-icon`。四轮迭代：32px → 64px → 96px → **128px**，同时移除家庭模式。

| 文件 | 内容 |
| --- | --- |
| `compare.png` | **主图**：四版纵向对比 |
| `home-weather-128.png` | 当前整页 |
| `top-128.png` | 当前顶部 |
| `home-weather-96/64.png`、`home-before.png` | 历史版本 |

## 当前布局

| 元素 | 位置 | 规格 |
| --- | --- | --- |
| 天气图标 | (872,58) | **128×128** |
| 演示数据标签 | 图标下方，同宽居中 | 11px `#718797` |
| 家庭模式 | — | **已移除** |
| 房间栏 | (28,204) | 未被图标遮挡 |

实测：图标可见轮廓 108×115，底部到 y=196，房间栏 y=204 无重叠；标签中心 x=936 与图标中心 935.5 对齐。

## 为什么是整数倍

v9 原生最大位图是 32px。放大到 128 是 **4 倍**，NEAREST 缩放时一个源像素对应规整的 4×4 方块，线条宽度均匀。非整数倍（如 100 或 120）会出现 3px 与 4px 交替，像素画质感被破坏。

如果还要更大，正确做法是补一张 `res/icons/v9/128/sun.png` 原生位图；继续放大只会成倍变糊。

## 生成规则（本轮修改）

```python
WEATHER_SIZE = 128
WEATHER_ICONS = {"sun", "cloud-rain", "cloud", "cloud-cog"}
```

彩色判定收紧为**只对天气 128px 生效**：

```python
weather_upscale = stem in WEATHER_ICONS and size == WEATHER_SIZE
is_color = (stem in COLOR_ICONS or is_pixel or
            (weather_upscale and has_v9_pixels))
```

这样 `camera_48` / `cloud-rain_48` / `home_48` 保持 A8，不会被误升成 ARGB8888（体积会涨 4 倍）。

## 验证

- Host CTest 13/13（`camera_dialog_check` 因主分支缺符号无法构建）。
- 断言：徽标 128px、源图 header.w = 128、图标与徽标坐标重合、`inner_align == STRETCH`。

## 已知阻塞（非本次改动）

主分支 `eea702d` 编译不过：`smart_home_lvgl_camera_dialog.c` 与 `test_camera_dialog.c` 引用 `SMART_HOME_BOARD_CAMERA_KEY`、`SMART_HOME_V9_SOURCE_BOARD`、`smart_home_v9_board_camera_view`，三个符号全仓库无定义。

## 复现

```sh
cmake -S demos/smart_home/tests/lvgl -B /tmp/x0 -DOPENVELA_ROOT=/home/arongw/openvela
cmake --build /tmp/x0 --target panel_check -j8
SMART_HOME_V9_CAPTURE=/tmp/home.ppm /tmp/x0/panel_check
```
