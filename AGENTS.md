# AGENTS.md — 面向 AI 协作者

maque 是一个以 **"引擎即浏览器、游戏即资源"** 为核心理念的轻量游戏引擎（C++23）。
本文件给自动化协作者提供**构建/测试命令、约定与架构落点**。人读概览见 `README.md`。

## 构建与测试（务必用这些命令）

```bash
# 首次：codegen 依赖（node + yaml）
cd modules/core/data && npm install && cd -

# 构建（Ninja）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# 全部测试（CTest）
ctest --test-dir build --output-on-failure

# 只跑 codegen 校验（schema 语法/尺寸/引用）
cd modules/core/data && node parser.js --check

# 运行示例
./build/samples/player/maque_player scenes/ball.scn
```

- `-DMAQUE_BUILD_TESTS=ON|OFF` 控制测试（默认 ON）。
- WebGPU 需 `third_party/wgpu-native/lib/libwgpu_native.so`（二进制**不入库**）。
- 系统依赖：SDL2 / libcurl / libwebsockets（`libsdl2-dev libcurl4-openssl-dev libwebsockets-dev`）。
- 已 vendored：`third_party/quickjs-ng`、`third_party/wuffs`。

## 硬约定（改代码前先读）

1. **语言**：C++23（`CXX_STANDARD 23`）；纯 C 模块用 C11。
2. **输出**：一律 `std::print`（`#include <print>`），**不要 `std::printf`**；stderr 用 `std::print(stderr, ...)`；`FILE*` 场景用 `std::print(f, ...)`。
3. **跨模块接口**：只暴露 **C ABI**（`extern "C"` + 版本化 vtable，见 `docs/02`）。
   禁跨边界异常/RTTI/STL；**分配者释放**；引用用**句柄**（`{index,generation}`）而非裸指针。
4. **无全局可变状态**（多实例，不同实例各持一份）。
5. **确定性**：模拟单线程、固定步长、引擎 RNG、帧内不发异步、**脚本 cycle GC 仅帧边界**（`docs/10/14`）。
6. **不做无关注释**；不改与本任务无关的文件。
7. **不提交构建产物**：`build/`、`third_party/wgpu-native/lib/*`、`modules/core/data/generated/` 均已 gitignore。

## 架构核心

- **声明 + 生成（单一真相 = `modules/core/data/schema.yaml`）**：
  - `class_list`（数据类型）→ C++ 类型头 + C ABI 描述符 + JSON。
  - `api_list`（函数接口）→ `maque_api.h`（声明）+ `maque_bindings.cpp`（**JS 绑定**）。
  - 生成物在 `build/modules/core/data/generated/`；`maque_schema` 编译描述符，`maque::genbind` 编译绑定。
  - **MANUAL**：`api_list` 项可带 `manual: "<header>"` → 生成器只 `#include` 该头（实现内联于头，不重复声明）。
- **反射中枢**：`mq::reflect`（id/名/路径查询、标量读写、visitor）；持久化 `maque::persist` 反射驱动。
- **运行时（浏览器）**：`modules/runtime` 的 `mq_run_scene(path)`：加载 `.scn` → 反序列化 → 加载 `.js` → 固定步长循环 → WebGPU 渲染 → 退出。
  宿主 `samples/player` 仅一行；**游戏逻辑全在资源**（`scenes/*.scn`、`scenes/*.js`）。
- **渲染**：`maque::gfx`（wgpu-native）。注意：该构建的 `wgpuInstanceWaitAny` **未实现**，异步统一用 `wgpuInstanceProcessEvents` 轮询。
- **实体/组件**：runtime 内实体表（`maque::handle`）+ 组件；**渲染从组件提取**（只读）。

## 常见改动怎么做

- **加一个数据类型/组件**：编辑 `modules/core/data/schema.yaml` 的 `class_list` → 重构建（自动重生成）。
- **加一个引擎接口**：在 `api_list` 加 `fn/args/returns`（标量/string）；实现放 `modules/api/src/api.cpp`；
  要内联实现则用 `manual: "<header>"` 并把头放到 `modules/api/include/`。
- **加一个模块**：`modules/<area>/<name>/`，含 `include/`、`src/`、`tests/`、`CMakeLists.txt`；
  在根 `CMakeLists.txt` `add_subdirectory`；导出 `maque::<name>` 静态库 + `add_test`。
- **脚本里调用引擎**：用生成绑定（如 `math.clamp(...)`）；宿主 `log/cfg/draw_sprite/entity_*` 由 runtime 注册。

## 测试与代码风格

- 每个模块自带**独立单测**（`tests/`），用简易 `CHECK` 宏，经 `add_test` 挂到 CTest。
- 格式/静态检查：`.clang-format`、`.clang-tidy`（导出 `compile_commands.json`）。
- 经典用法：先加/改接口与测试，再实现。

## 文档地图

- 设计文档：`docs/`（根文档 `docs/00-设计哲学与约束.md`；索引 `docs/README.md`）。
- 接口绑定机制：`docs/21-接口声明与绑定生成.md`。
- 运行时/资源模型：`docs/20-运行时与资源模型.md`。
- 里程碑/路线图：`docs/milestones/00-路线图.md`。
- 各模块开发计划：`plans/`（`plans/README.md`）。
