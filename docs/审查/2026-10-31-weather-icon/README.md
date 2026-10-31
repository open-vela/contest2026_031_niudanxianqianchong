# 首页天气图标放大并移到右上角

分支 `feat/v9-weather-icon`，基线 `eea702d`。

| 文件 | 内容 |
| --- | --- |
| `compare.png` | **主图**：改前 / 改后 上下对比（首页上半部） |
| `home-weather-64.png` | 改后整页 |
| `home-before.png` | 改前整页 |

## 改动

| 项 | 改前 | 改后 |
| --- | --- | --- |
| 天气图标尺寸 | 32×32 | **64×64** |
| 天气图标位置 | (348,124)，温度右侧 | (936,72)，右上角 |
| 家庭模式按钮 | 158×40 实心卡片 @ (838,96) | 112×28 雾蓝胶囊 @ (888,146) |
| 家庭模式位置 | 右上角 | 天气图标下方 |
| 演示数据标签 | (838,142) | (952,182) |

## 三个技术要点（都是实际踩到的）

### 1. 图标资源没有 64px 版本

`smart_home_lvgl_png_icon_get("asset:sun", 64)` 原本回退到 32px，再被拉伸成 64，发虚。在生成脚本里加了 `WEATHER_ICONS` 与 `WEATHER_SIZE = 64` 档位，并在 lookup 表增加第四个槽位 `weather`：

```c
if (requested_size > 32 && requested_size <= 48 && entry->large) {
    return entry->large;
}
if (requested_size > 48 && entry->weather) {
    return entry->weather;
}
```

### 2. 64px 生成成了灰度图

`write_icon` 原逻辑只在 `res/icons/v9/<size>/` 存在时才用彩色（ARGB8888），v9 目录没有 `64/` 子目录，所以 64px 落进 A8 分支，渲染成单色。修正为：只要该图标有任意原生 V9 像素，所有尺寸都保持彩色。

### 3. 图标只画出中间一小块

`lv_image` 默认 `inner_align = _LV_IMAGE_ALIGN_AUTO_TRANSFORM`，缩放时会按原图尺寸居中绘制并裁掉超出部分。显式设置：

```c
lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_STRETCH);
```

这一条最难定位：坐标、尺寸、源图数据全部正确（64×64、16384 字节、1184 个不透明像素），但屏幕上只出现中心 16×16 一块。

## 验证

- 像素级：图标区域从 392 个非背景像素提升到铺满 64×64 徽标。
- Host CTest：13/13 通过（排除 `camera_dialog_check`，见下）。
- 断言新增：徽标宽 64、源图 header.w 64、图标坐标与徽标完全重合、`inner_align == STRETCH`。回退任一项都会失败。

## 已知阻塞（非本次改动）

主分支 `eea702d` **本身编译不过**：`smart_home_lvgl_camera_dialog.c` 与 `test_camera_dialog.c` 引用了 `SMART_HOME_BOARD_CAMERA_KEY`、`SMART_HOME_V9_SOURCE_BOARD`、`smart_home_v9_board_camera_view`，这三个符号在仓库里没有任何定义。因此 `camera_dialog_check` 无法构建，其余 13 项正常。

## 复现

```sh
cmake -S demos/smart_home/tests/lvgl -B /tmp/wx -DOPENVELA_ROOT=/home/arongw/openvela
cmake --build /tmp/wx --target panel_check -j8
SMART_HOME_V9_CAPTURE=/tmp/home.ppm /tmp/wx/panel_check
```
