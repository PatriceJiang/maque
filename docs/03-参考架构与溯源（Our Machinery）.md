# 03 · 参考架构与溯源（Our Machinery）

> 本文是**对照参考 + 差异声明**。
> maque 的骨架大量沿用 **The Machinery**（Niklas Gray，"Our Machinery"博客）的哲学。
> 记录它有三个目的：① 溯源，避免误以为原创；② 借它踩过的坑；③ 明确 maque 的差异与创新。

## 1. 为什么是 The Machinery

我们前几篇文档自发推出来的结论——**插件即 `.so`、接口为带版本的 C 函数指针表、
按 name+version 查注册表、无全局可变状态、数据即真相、句柄带 generation**——
与 The Machinery 的核心主张高度重合，甚至术语都对应。与其重新发明，不如显式继承并差异化。

## 2. The Machinery 的六条主张

### 2.1 一切皆插件（Everything is a plugin）
引擎不是单体库，而是一组 `.dll/.so`。宿主只负责**加载、注册、生命周期**。
渲染、实体、脚本、资产编译器，全都是插件。

### 2.2 接口 = API struct（C ABI 函数指针表）
每个插件暴露若干 `tm_xxx_api`（如 `tm_renderer_api`、`tm_the_truth_api`、`tm_entity_api`、
`tm_allocator_api`），结构体头部带 **name + version**。
跨插件调用通过 `tm_api_registry_api->get(name, version)` 查找：

- **不**靠 C++ 链接、**不**共享类定义；
- 版本不匹配即拒绝或降级。

> maque 对应：文档 02 §3 的 `mq_module_query` + C vtable（我们把版本头做成 `struct_size` + `abi_version`，
> 更接近 Vulkan 的前向兼容风格）。

### 2.3 没有全局状态
没有单例、没有"引擎对象"。插件通过注册表拿到别的插件的 API，而非直接链接。
状态属于**数据**，不属于某个全局引擎实例。

> maque 对应：ADR-0014「多实例靠零全局可变状态」。

### 2.4 数据即真相（The truth is in the data）
`tm_the_truth_api` 是一个**类型化对象数据库**：对象类型由"type 资产"定义，
属性可被脚本读写，被实体系统 / 渲染 / 序列化共享。**GUI 只是它的一个视图。**

> maque 对应：ADR-0011「序列化中枢 + 反射即 canonical IR」。文档 04（反射）是它的核心实现。

### 2.5 实体系统是数据导向的扁平模型
`tm_entity_id` 是 `{id, generation}` 的**句柄**；组件是数据；父子层级是数据，不是对象继承。

> **关键历史事实**：The Machinery 的实体系统**取代**了更早的 OOP 场景树。
> 换句话说，你提出的"ECS vs 场景树该选哪个"——正是 The Machinery 真实经历过的那次迁移。
> maque 把它升级为**可切换的双模式评测**（ADR-0003），比它更激进。

### 2.6 数据编译器（Data Compiler）
一个**独立工具**：把源资产编译成二进制、打包、导出 dump，可脱离运行时执行。

> maque 对应：文档 01 §4 的 `ai` 模块（独立加载 `.scn` → 依赖 → binary → 压缩 → dump）。

## 3. 映射总表

| Our Machinery | maque 落点 | 状态 |
|---|---|---|
| 一切皆插件 `.so` | ADR-0007 / 文档 02 | ✅ |
| API struct + name/version 注册表 | 文档 02 §3（Vulkan 式版本头） | ✅ |
| 无全局状态、registry 取依赖 | ADR-0014 / 文档 02 §4-5 | ✅ |
| the truth = 类型化对象数据库 | ADR-0011 + 文档 04（反射） | ⏳ |
| 实体 = `{id,generation}` 句柄 | ADR-0002 / 文档 00 §5 | ✅ |
| 数据编译器 | `ai` 模块 / ADR-0009 | ✅ |
| 实体系统取代场景树 | ADR-0003 双模式评测 | ⏳ |

## 4. 差异声明（maque 不是复刻）

| 维度 | The Machinery | maque | 取舍 |
|---|---|---|---|
| 渲染 | 自研 Apollo（自建 GPU 抽象 + render graph） | **直接 WebGPU** | 少一层抽象、省力；受 WebGPU 能力边界约束（OD-3） |
| 脚本 | 自有 / C# 为主，较重 | **quickjs 为主** + refcount，其余可选 | 嵌入式友好；重型 VM 后置 |
| 控制平面 | 无独立 AI 控制面 | **observability + control plane + MCP**（A1–A5） | **maque 的真创新** |
| 场景模型 | 一次性从场景树迁移到实体系统 | **双模式并存、可切换评测** | 用规范数据协议支撑，成本更高但信息更全 |
| 标准 | C++ / 更保守 | **C++23**（`std::stacktrace` 利崩溃采集） | 工具链要求更高 |

## 5. 向它借的"血泪教训"

1. **"数据即真相"是全局重构，不是局部模块。** 一旦数据模型定了，GUI/实体/渲染/序列化都围绕它转。
   → 因此 maque 把**反射（文档 04）排在最前**，先定中枢再定外围。
2. **API 版本混乱是长期债。** name+version + 拒绝不兼容是刚需，越早越好。
   → 文档 02 已强制。
3. **无全局状态一开始就要铁腕。** 事后清理单例极痛苦。
   → ADR-0014 从第一天禁止可变全局。
4. **数据编译器独立化**能极大加速迭代与 CI，也让 AI/CI 无需启动完整引擎。
   → `ai` 模块独立运行。

## 6. 反模式固化清单

- ❌ 用 C++ 类/模板/异常做跨插件边界（应回到 C ABI）。
- ❌ 把"引擎实例"做成全局单例（应做成 instance context）。
- ❌ 让某子系统拥有自己的数据真相，与中枢并行（应统一进 truth/反射模型）。
- ❌ 把数据编译器耦合进运行时（应独立可执行）。

## 7. 参考资料

- Our Machinery 博客：<https://ourmachinery.com/>（Niklas Gray 关于插件、API、数据模型、实体系统的系列文章）
- 术语对照：`tm_api_registry_api` ↔ maque 的 host registry；`tm_the_truth_api` ↔ maque 反射/持久化中枢；
  `tm_entity_api` ↔ maque `scene`；`data compiler` ↔ maque `ai` 模块。
