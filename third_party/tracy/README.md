# Tracy Profiler（vendored 客户端）

- 版本：**v0.11.1**（https://github.com/wolfpld/tracy）
- 入库内容：仅 `public/`（客户端库源码）+ `LICENSE` + `NEWS`。
  **profiler GUI / capture 命令行工具是二进制，不入库** —— 用
  `tools/tracy/get-profiler.ps1` 下载到 `.deps/tracy/`（F3 启动时自动搜索）。
- 构建：`third_party/tracy/CMakeLists.txt` 只编译 `public/TracyClient.cpp`，
  配置为 `TRACY_ON_DEMAND + TRACY_DELAYED_INIT + TRACY_MANUAL_LIFETIME`
  （手动启动：F3 / `MAQUE_TRACY=1`；未启动时封装层全部 no-op）。
- 使用方式见 `docs/29-Tracy性能分析.md`。
