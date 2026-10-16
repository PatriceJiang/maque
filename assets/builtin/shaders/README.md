# 内建 shader（assets/builtin/shaders/）

渲染插件（`modules/render/gfx`）用的**内建 WGSL**。这里是**唯一真相**：引擎不再内联 shader 字面量。

## 布局：一个 shader 一个目录

```
assets/builtin/shaders/
  solid/shader.wgsl        # 无贴图图元（矩形/圆/方块）
  textured/shader.wgsl     # 贴图图元（图集子矩形 + 采样器）；uniform 布局与 solid 一致（48B）
  <new-name>/shader.wgsl   # 新增：目录名 = shader 名（gfx 按名字取）
```

- **目录名 = shader 名**（`mq_builtin_shader_text("<name>")`、CMake 开关 `MAQUE_SHADER_<NAME>`）。
- 目录内可放**多个** `*.wgsl`：按文件名排序拼接（要拆段时用 `01_uniforms.wgsl`、`02_vs.wgsl`…）。
- 每个 shader 至少要有一个 `@vertex` 与一个 `@fragment`（打包器会告警）。

## 构建流水线（CMake 配置期）

`cmake/shader_pack.cmake` + `tools/shader_pack.js` 在**配置期**把资产打包成构建目录里的压缩源码
（生成物**不入库**，改 shader 会自动重新配置 → 重新打包）：

```
build/generated/builtin_shaders/
  solid.cpp            # 每个 shader 一个源文件：zstd 压缩 Blob + 取源码的函数
  textured.cpp
  builtin_shaders.h    # 注册表类型 / 表 / count / mq_builtin_shader_text()
  builtin_shaders.cpp  # 注册表（只含**编入**的 shader）
```

- 压缩：`zstd -19`（node 的 `zlib.zstdCompressSync`，退回 `zstd` CLI），运行时由 **libzstd** 解压
  （`modules/render/gfx/src/shader_decompress.cpp`，首次取用时解压并缓存）。
  没链接 libzstd 时流水线自动**直嵌原文**（功能不变，仅体积略大）。
- 手动重打包：`cmake --build build --target maque_shaders`。
- 当前体积（参考）：`solid` 2421 → 1279 字节（53%）、`textured` 2759 → 1420 字节（51%）。

## 剔除（默认全部编入）

每个 shader 一个缓存开关，默认 ON；关掉即**不生成、不链接、不进二进制**：

```bash
cmake -S . -B build -DMAQUE_SHADER_TEXTURED=OFF      # 不编入 textured
cmake -S . -B build -DMAQUE_BUILTIN_SHADER_COMPRESS=OFF   # 关闭 zstd（直嵌原文）
```

被剔除的 shader 在运行时取不到（`mq_builtin_shader_text()` 返回 NULL）：gfx 会在初始化时打印
一行原因，并**跳过**对应批次（不崩、不静默）。

## 验证

- `ctest -R builtin_shaders`：注册表内容与开关一致、解压文本与**磁盘资产逐字节一致**（漂移检查）、
  解压有缓存（重复取用同一指针）、未编入名字 → NULL。
- `ctest -R 'gfx|text_pixel'`：真实渲染路径（含 1:1 像素精确的文字）走的就是这些 shader。
