# V9 三页视觉对齐

分支：`feat/p4x-lvgl-v9-ui`，基于 `39467db` 及提交前的视觉对齐工作区。图像来自实际 LVGL Host RGB565 framebuffer，不是浏览器原型或 AI 效果图。

[实际渲染总览](overview.png) · [设计参考与实际渲染并排](comparison.png)

| 页面 | 原尺寸渲染 | 本轮对齐 |
| --- | --- | --- |
| 首页 | [home.png](home.png) | 补齐固定三项底栏；保留卡片尺寸、原像素插画与右上独立开关；开关轨道54×30、圆点直径22 |
| 聊天 | [chat.png](chat.png) | 智能管家标题、用户/助手头像、回复上方工具状态、输入区和发送按钮 |
| 设置默认页 | [settings.png](settings.png) | 六类入口的像素图标、公共顶底栏、显示与声音内容位置 |
| 设置模型页 | [model.png](model.png) | 服务336px/标识344px同排、密钥704px整排、44px输入框、按钮及帮助文字位置 |
| 设置网络页 | [network.png](network.png) | 中文输入与密码遮罩、公共样式；继续保留真实的手动网络连接表单 |

## 对齐依据

- 使用已确认的 V9 `layout-spec.json`、原SVG/PNG与 MiSans 字体约定。
- 三页底栏固定 `(0,536,1024,64)`，三项选中底色 `216×52`。
- 聊天输入区 `(24,464,976,56)`，输入框704px，发送按钮168×40。
- 模型输入框起点分别为 `(272,228)`、`(632,228)`、`(272,306)`；修正父容器边框导致的1px偏移和右侧裁切。
- 正文、输入、占位提示、下拉列表使用 MiSans；下拉箭头使用原像素图，不依赖字体中的私用符号。
- 21类图标的20/32px原生PNG共42张，直接转换为ARGB8888，保留画布、颜色、透明度；不裁切、不缩放、不强制重着色。来源与Hash见 `demos/smart_home/res/icons/v9/manifest.json`。

## 与参考图的明确差异

- 原参考仍使用 Noto/DejaVu，并包含500字重；实际页面采用已确认的 MiSans Normal（400），没有用600冒充500，也没有额外预载整套粗体。字形、字宽和标题粗细存在可见差异，不能宣称逐像素一致。
- RGB565量化会使背景、暖白和雾蓝与原RGB888参考出现颜色近似差异。
- 首页标注演示设备，时钟/天气与聊天内容是Host固定测试数据，不表示板端服务已连接。
- 保留“新对话”功能，将其移至聊天标题区，避免挤占底部输入布局。
- 未开启ASR的配置显示禁用麦克风。显示亮度、音量、自动屏保和设备动画没有可用控制链路，保留不可用/禁用状态，不显示参考图的示例80%/60%或虚假开启动效。
- 聊天回复中的设备摘要条需要结构化真实数据；当前不根据回复文字生成虚假的设备状态。
- 网络扫描列表、设备弹窗、房间管理、其他设置子页的完整控件仍属后续功能补齐，不在本轮三页基础视觉验收范围。

## 验证

- Host CTest：11/11通过。
- ASan/UBSan：`panel_check`、`settings_v9_check`、`chat_check` 3/3通过；关闭退出时的泄漏扫描，不构成全工程无泄漏证明。
- `panel_check` 现在链接真实聊天页、顶栏与导航；验证三个底栏的坐标、可见绘制、选中底色、全部导航点击、模型字段尺寸及不被父容器裁切。
- 设置测试验证输入/下拉/占位文字字体与中文字符存在；像素资源测试对42张PNG和生成C数组逐字节核对。
- 开启ASR宏的聊天源码通过Host对象编译；这不是ASR服务运行验证。
- 没有执行固件构建、烧录或板端流畅度/内存测试。

## 资源变化

图标位图常量从107,392 B增加到204,400 B，增加97,008 B（约94.7 KiB）；PNG源文件合计11,842 B。该统计不等于目标ELF或LittleFS镜像增量，最终链接与打包仍需测量。没有新增整套字体预载。

## 复现

在开发worktree根目录执行：

```sh
cmake -S demos/smart_home/tests/lvgl \
  -B /tmp/p4x-v9-review-host-20261031 \
  -DOPENVELA_ROOT=/home/arongw/openvela
cmake --build /tmp/p4x-v9-review-host-20261031 -j8
ctest --test-dir /tmp/p4x-v9-review-host-20261031 --output-on-failure
python3 scripts/render_v9_review.py \
  --binary /tmp/p4x-v9-review-host-20261031/panel_check \
  --out docs/审查/2026-10-31-v9-aligned \
  --design-root /home/arongw/openvela/out/smart-home-ui-design-v9-card-layout-20261030
```

`provenance.json`记录源码Hash；`host-output.txt`记录实际渲染用例输出。测试仍会输出旧代码中未使用函数、符号比较和`mallinfo`弃用警告，本轮未将其扩展为无关清理。
