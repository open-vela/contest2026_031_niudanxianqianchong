# 首页天气图标放大到 96px，移除家庭模式

分支 `feat/v9-weather-icon`，基线 `eea702d`。三轮演进的最终形态。

| 文件 | 内容 |
| --- | --- |
| `compare.png` | **主图**：三版纵向对比（改前 / 64px / 96px） |
| `home-weather-96.png` | 本版整页 |
| `top-96.png` | 本版顶部 |
| `home-weather-64.png` | 上一版（64px + 小家庭模式） |
| `home-before.png` | 原始（32px + 大按钮） |

## 本版改动

| 项 | 上一版 | 本版 |
| --- | --- | --- |
| 天气图标 | 64×64 @ (936,72) | **96×96 @ (904,68)** |
| 家庭模式 | 112×28 胶囊，保留 | **已移除** |
| 演示数据标签 | (952,182) | (944,172)，图标正下方 |
| 页面对象数 | 232 | 230（少了按钮与标签） |

## 为什么用 96px

天气图标的原生最大位图是 v9 的 32px，96 正好是 **3 倍整数放大**，用 NEAREST 缩放时每个源像素对应 3×3 方块，不会出现非整数倍导致的线条宽窄不均。64px 是 2 倍，同样可行；96 只是更醒目。

如果以后要更清晰，正确的路径是补一张 `res/icons/v9/96/sun.png` 原生位图，而不是继续放大。

## 家庭模式去哪了

按钮从首页移除，但场景功能本身没删：

- `smart_home_lvgl_open_scene_popup()` 仍保留（`home.c:77`），旧首页的 `HOME_ACTION_SCENES` 入口还能打开它；
- 相关的 `g_scene_modes`（回家/观影/睡眠/离家）和场景弹窗代码都还在；
- 只删掉了 `v9_family_cb` 这个已无引用的回调。

也就是说：**V9 首页不再有场景入口，但场景能力仍在代码里**。后续要恢复，只需在新位置重新挂一个 `lv_obj_add_event_cb(..., open_scene_popup, ...)`。

## 验证

- Host CTest 13/13（`camera_dialog_check` 因主分支缺符号无法构建，见下）。
- 断言检查：徽标 96px、源图 header.w = 96、图标坐标与徽标完全重合、`inner_align == STRETCH`。
- 对象树 232 → 230，与「移除一个按钮 + 一个标签」吻合。

## 已知阻塞（非本次改动）

主分支 `eea702d` 本身编译不过：`smart_home_lvgl_camera_dialog.c` 与 `test_camera_dialog.c` 引用了 `SMART_HOME_BOARD_CAMERA_KEY`、`SMART_HOME_V9_SOURCE_BOARD`、`smart_home_v9_board_camera_view`，这三个符号在整个仓库里没有任何定义。

## 复现

```sh
cmake -S demos/smart_home/tests/lvgl -B /tmp/wy -DOPENVELA_ROOT=/home/arongw/openvela
cmake --build /tmp/wy --target panel_check -j8
SMART_HOME_V9_CAPTURE=/tmp/home.ppm /tmp/wy/panel_check
```
