# 首次配置引导：实现记录（2026-11-01）

## 范围

按设计稿的第一版建议，**只实现 01（总览浮层）**，中间步骤跳转到现有设置面板，
不重写表单。设计稿里的 02-09 仍是设计参考，未实现。

## 已实现

| 内容 | 位置 |
| --- | --- |
| 状态查询（三项 + pending 计数） | `smart_home_lvgl_onboarding_status()` |
| 总览浮层（三行并列 + 稍后再说） | `smart_home_lvgl_onboarding_present()` |
| 行点击跳转设置页对应分类 | `ob_row_click_cb()` |
| 首页「演示数据」标签兼作入口 | `v9_source_label_cb()` |
| 运行期跳过标记 | `g_ob.skipped` |

## 关键设计决策

**不做强制线性向导。** 三项并列，各自独立跳转，底部始终有「稍后再说」。
理由：三项依赖不同，强制顺序会让只用本地设备的用户被迫绑米家；比赛现场
评委也会先看到配置页而不是首页。

**跳过状态不落盘。** LVGL 主线程直接写 LittleFS 会挂死系统
（`smart_home_lvgl_pages.c:977` 有既有记录）。因此 `g_ob.skipped` 只在
内存里，每次开机重新评估。代价只是重启后再提示一次。

**复用现有面板，不重复实现表单。** 点击行跳转到：
- 家庭网络 → 设置分类 1「网络连接」
- 米家网关 → 设置分类 4「设备与服务」
- 模型服务 → 设置分类 2「模型服务」

**入口不占用顶栏。** 顶栏右侧已被时钟 + 4 个状态图标占满，未改动。改用
已有的「演示数据」标签（11px）加 `ext_click_area` 扩到 56px 触控。

## 布局规格

| 对象 | 值 |
| --- | --- |
| 遮罩 | 全屏，黑色 130/255 不透明 |
| 面板 | (92, 62) 840×476，圆角 18，内边距 32 |
| 行 | 776×84，间距 12，圆角 12，底色 SURFACE_SOFT |
| 行内图标底座 | 48×48，圆角 12，底色 SURFACE，图标 26px |
| 状态胶囊 | 92×26，圆角 13，圆点 6px |
| 行按钮 | 118×40，圆角 10 |

## 验证

| 项 | 结果 |
| --- | --- |
| Host 回归 | **15/15 通过** |
| 新增用例 | `test_v9_onboarding`：pending 计数、浮层几何、行跳转、跳过记忆 |
| 固件构建 | `smart_home` 配置 **通过**，日志含 `CC: src/ui/lvgl/smart_home_lvgl_onboarding.c` |
| 板端验收 | **未执行** |

## 构建注意

`packages/demos/smart_home` 是指向**主检出**的符号链接，因此
`scripts/build_p4x.sh` 默认编译主检出代码，**不会**编译 worktree 的改动。
本次验证是临时把链接指向 worktree 后再构建的，验证完已还原：

    # 验证前
    ln -sfn ../../worktrees/onboarding-codebuddy/demos/smart_home \
        /home/arongw/openvela/packages/demos/smart_home
    # 验证后还原
    ln -sfn ../../contest2026_031_niudanxianqianchong/demos/smart_home \
        /home/arongw/openvela/packages/demos/smart_home

过程中遇到一次 `esp-hal-3rdparty` 克隆瞬态失败（`tmp_pack_*` 读取失败），
重试即通过，与本次改动无关。

## 未实现（设计稿 02-09）

- Wi-Fi 扫描列表（需要 NuttX `wpa_ctrl` 封装，项目无现成实现）
- 连接中四步进度、连接失败排查页
- 米家地址/扫码/成功的独立向导页（现有弹窗已够用）
- 模型连通性测试页
- 完成页（10）

## 图片

- `impl-onboarding-overlay.png`：实现后的实际截图（Host 渲染）
- 设计稿（10 张）见 `docs/审查/2026-11-01-onboarding-design/`

## 补充：没有改动的东西

- `smart_home_wifi.c` 一行未改，Wi-Fi 扫描仍未实现（设计稿 02 未落地）；
- 米家扫码弹窗、网络面板、模型配置面板全部复用，未重写；
- 顶栏未动（右侧已被时钟和 4 个状态图标占满）。

## Kconfig 开关

`CONFIG_SMART_HOME_ONBOARDING`（`demos/smart_home/Kconfig`）：

    config SMART_HOME_ONBOARDING
        bool "First-run configuration guide"
        depends on SMART_HOME_DEMO_UI_LVGL
        default y

关掉后：
- `smart_home_lvgl_onboarding.c` 不参与编译（Makefile 与 CMakeLists 都有条件）；
- `home_v9.c` 不再 include 头文件、不调用 `present()`；
- 首页「演示数据」标签不再设 CLICKABLE，回到纯展示。

四个 board 配置都显式写了 `CONFIG_SMART_HOME_ONBOARDING=y`：
`smart_home`、`smart_home_service_lab`、`smart_home_voice`、`smp_smart_home`。

Host 测试用独立的 `SMART_HOME_TEST_ONBOARDING` 选项（默认 ON）对应，
`-DSMART_HOME_TEST_ONBOARDING=OFF` 可验证关闭态。

## 两种状态都验证过

| 状态 | Host 回归 | 固件构建 |
| --- | --- | --- |
| ON | 15/15 | 通过，日志含 `CC: src/ui/lvgl/smart_home_lvgl_onboarding.c` |
| OFF | 15/15 | 通过，日志无该行 |

## 一个踩到的坑：不能用 smart_home_wifi_is_connected()

第一版直接用 `smart_home_wifi_is_connected()` 判断联网，结果 `smart_home`
配置固件链接失败：

    undefined reference to `smart_home_wifi_is_connected'

原因：`ESP_HOSTED` 板级配置下 `smart_home_wifi.c` 不参与编译（走 hosted
网络栈），该函数根本不存在。改用 `app->system_status.network_status.online`，
两种配网后端下都存在。
