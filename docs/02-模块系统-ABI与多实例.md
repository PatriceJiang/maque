# 02 · 模块系统、跨 `.so` ABI 与多实例

> 上游：`00`（§6 三层解耦、ADR-0007）、`01`（模块划分）。
> 本文**正面解决 OD-4**：跨 `.so` 的 ABI 与内存所有权。这是所有行为模块的地基。

## 1. 目标

- 模块编译为独立 `.so`，**强边界、可插件化、可热重载、可多实例**。
- 边界必须**稳定、可版本化、跨编译器/跨版本安全**。
- 不牺牲能力：仍然满足 A1 可自省、A4 可操作。

## 2. 核心决策（OD-4 结论）

跨 `.so` 边界**只允许 C ABI**。以下规则为**强制**：

| # | 规则 | 说明 |
|---|------|------|
| R1 | **只暴露 `extern "C"`** | 边界上不出现 C++ symbol、类、模板、重载 |
| R2 | **禁用跨边界异常** | 所有入口 `noexcept`；错误用返回码 / 错误句柄表达 |
| R3 | **禁用跨边界 RTTI** | 不依赖 `dynamic_cast`/`typeid` 跨模块 |
| R4 | **句柄而非指针** | 不透明句柄 `{index, generation}` / `struct mq_*`，带世代防悬垂 |
| R5 | **分配者释放** | 谁分配谁释放；或全部经传入的 allocator vtable 分配 |
| R6 | **POD 化参数** | 固定宽度整数、UTF-8 `char*`；禁用 `std::string/std::vector/shared_ptr` 跨边界 |
| R7 | **接口版本化** | 每个接口带 `struct_size` + `abi_version`，支持前向兼容 |
| R8 | **符号可见性收敛** | 默认 `-fvisibility=hidden`，只导出模块查询入口 |

## 3. 接口形态：C vtable + struct-size 版本化

参照 Vulkan/WebGPU/Win32 的成熟做法：每个模块导出**一个** C 入口，返回一张函数指针表。

```c
/* maque 公共 ABI 头（纯 C） */
typedef struct mq_allocator {
    uint32_t struct_size;
    void* (*alloc)(void* self, size_t size, size_t align);
    void  (*free)(void* self, void* ptr);
} mq_allocator;

typedef struct mq_renderer_v1 {
    uint32_t struct_size;      /* = sizeof(mq_renderer_v1)，前向兼容 */
    uint32_t abi_version;      /* 主版本不匹配即拒绝 */
    int      (*create)(void* host, mq_allocator*, /* out */ void** out);
    void     (*destroy)(void* self);
    int      (*submit)(void* self, const void* draw_list, size_t n);
} mq_renderer_v1;

/* 每个 .so 仅导出这一个入口 */
void* mq_module_query(const char* interface_name, uint32_t min_version);
```

约定：

- **`struct_size` 只增不改**：新版本在表尾追加函数，旧调用方按自身 `struct_size` 截断使用。
- **`abi_version` 主版本不兼容即拒绝加载**，给出可读错误（服务 A2）。
- **接口名用稳定字符串 ID**（如 `"maque.renderer.v1"`），而非 C++ 名字。

> **落地注记**：渲染器已按本设计落地为真 `.so` 插件（`maque_gfx.so`，`mq_module_query` + DSL 生成的
> `mq_renderer_v2`，ABI 名称 `maque.renderer.v2`），运行时经 `maque::module`(dlopen) 加载。其余模块仍为静态链接，逐步 `.so` 化。
>
> **平台注记**：以上为原生（`.so` + `dlopen`）形态。WASM/嵌入式无法任意 `dlopen`，
> 物理加载改为静态链接或独立 `.wasm` 实例化；但**逻辑边界（C ABI vtable + registry）不变**。
> 详见文档 05 §3。

## 4. 模块生命周期

```
discover → dlopen → mq_module_query → 版本/依赖校验 → init(host) → run → shutdown → dlclose
```

- **依赖注入**：`init(host)` 传入 **host context**，内含 allocator、logger、registry、executor、其它服务接口。
  模块**绝不**访问全局单例。
- **服务定位**：模块通过 `mq_module_query("<iface>")` 向上层/宿主请求依赖，**而非**硬链接其它 `.so`。
  这样解耦加载顺序（拓扑排序只作为优化）。

## 5. 多实例（Multi-instance）

> 多实例的**唯一**实现方式是：**没有任何全局可变状态**。

- 所有状态存在于 **instance 拥有的 context 结构**中，由 `create(host, ...)` 建立。
- **禁止**可变全局 / 静态单例（`static` 可变变量、函数内 static）。允许 `static const` / `constexpr`。
  用 `.clang-tidy` 自定义规则 + 代码评审强制。
- 每个引擎实例拥有独立的：allocator、RNG（决定性）、registry、executor、日志域。
- 跨实例**完全隔离**：句柄不跨实例传递（`generation` + `instance_id` 校验）。

## 6. 热重载

因为状态在 context 中、接口是数据化的 C vtable，热重载流程为：

```
序列化实例状态 (ADR-0011) → dlclose 旧 .so → dlopen 新 .so
→ mq_module_query → init → 反序列化恢复状态
```

约束：

- 任何模块**不得缓存**另一模块的函数指针跨重载使用（重载后失效）。
- 重载点必须在**帧边界**（与 OD-5 的 GC 时机、ADR-0005 确定性对齐）。

## 7. 边界放置原则（性能）

> 跨 `.so` 调用 = 间接调用 + **无内联**。因此：

- **边界只放在粗粒度操作上**（create/submit/tick/serialize 级别）。
- **热循环留在模块内部**（逐实体迭代、transform 传播等）。
- 这与 `00 §6` 的"冷路径可共享、热路径各自私有"完全一致：**`.so` 边界 ≈ 冷路径边界**。

## 8. 反射数据如何跨边界

反射/schema 是**数据，不是代码**：

- 以 **序列化描述符**（ABI 稳定的数据）共享，而非共享 C++ 类型；
- 或模块在 `init` 时通过 C ABI 把自己描述的类型注册进 host registry。
- 这样 A1（可自省）在"多 `.so`"下依然成立。

## 9. 反模式清单（禁止）

- ❌ 跨边界传 `std::string` / `std::vector` / `shared_ptr` / `std::function`。
- ❌ 让异常逃逸出 `.so`（跨边界 `std::terminate`）。
- ❌ 一个 `.so` 分配、另一个 `.so` `free`。
- ❌ 跨重载缓存函数指针。
- ❌ 可变全局 / 单例（破坏多实例）。
- ❌ 依赖两个模块各自静态链接的同一第三方库产生重复符号（用 `-fvisibility=hidden` + 控制链接作用域）。

## 10. 与仓库现状的映射

- `modules/core/data` 当前是 **INTERFACE（头文件）库**。合理：
  **纯 POD / 固定布局的数据契约**用编译期共享头（保证 layout 一致），**行为模块**才做 `.so`。
  即区分：
  - **编译期 POD 契约**（`math`、`core/data`）：header/interface，固定布局 + `static_assert(size)`。
  - **运行时行为模块**（`scene`、`renderer`、`script`…）：`.so` + C vtable。
- 后续 `core/data` 的 schema codegen 应能**同时产出**：C++ POD 头 + C ABI 描述符。

## 11. 遗留风险

| 风险 | 处理 |
|------|------|
| `.so` 边界无内联 → 性能 | 边界粗粒度化（§7） |
| 模块过细 → ABI/调度开销放大 | 合并宁少勿多，见 `01 §5` |
| 热重载状态迁移不完整 | 状态全部可序列化（ADR-0011），禁止隐藏指针状态 |
| 嵌入式 `dlopen` 限制 | 评估静态链接 + 编译期模块集作为降级方案（与 OD-3 同类） |
