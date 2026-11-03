# cagent2_chat

基于 `packages/cAGENT`（vendored cAgentV2）的单轮问答 demo，是 V2 内核在
openvela 上的第一条端到端链路验证（webclient transport + mbedTLS 后端的
真网络首跑）。

## 运行（goldfish 仿真，defconfig：`cagent2_chat`）

先改 `src/cagent2_chat_main.c` 顶部的三个宏（key/endpoint/model），编译后：

```
nsh> ifup eth0
nsh> cagent2_chat "用一句话介绍 openvela"
```

不带参数运行时进入单行提示输入。

> 注意：key 编译进固件，仅限本地调试，勿提交真实 key 或随固件分发。

## 范围与已知边界

- 单轮、无工具、无会话历史（`max_history_turns=0`，无需 session storage）。
- 证书校验为 OPTIONAL（链检查结果仅记录日志），见包 README 的债务说明。
- 请求使用 legacy `max_tokens` 字段（DeepSeek 兼容性保守选择）。
- 后续多轮（B）与工具版（C）在此基础上增量扩展。
