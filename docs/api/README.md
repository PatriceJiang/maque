# API 参考（自动生成，随仓库提交）

本目录索引**由 DSL 生成**的接口与数据协议文档 —— 生成物已入库，可直接阅读，无需构建。

| 文档 | 内容 |
|------|------|
| [API.md](../../modules/core/data/generated/API.md) | 脚本 API（102）· 控制面方法（27）· 事件回调（10）· 插件接口 vtable · 模块 C ABI（20） |
| [TYPES.md](../../modules/core/data/generated/TYPES.md) | 240 个数据类型（字段/默认值/attrs/role）—— C++ 类型头与 C ABI 描述符的来源 |
| [maque_schema.json](../../modules/core/data/generated/maque_schema.json) | 同一份声明的机器可读形式（`types`/`apis`/`controls`/`callbacks`/`modules`，供 AI 与工具消费） |
| [maque_ifaces.h](../../modules/core/data/generated/maque_ifaces.h) | 插件接口 C vtable（含 `MQ_*_API_NAME` 宏） |
| `abi/<module>.h` | 20 个模块的公开 C ABI 头：[vfs](../../modules/core/data/generated/abi/vfs.h) · [asset](../../modules/core/data/generated/abi/asset.h) · [nav](../../modules/core/data/generated/abi/nav.h) · [physics](../../modules/core/data/generated/abi/physics.h) · [platform](../../modules/core/data/generated/abi/platform.h) · [input](../../modules/core/data/generated/abi/input.h) · [script](../../modules/core/data/generated/abi/script.h) · [handle](../../modules/core/data/generated/abi/handle.h) · [module](../../modules/core/data/generated/abi/module.h) · [reflect](../../modules/core/data/generated/abi/reflect.h) · [obs](../../modules/core/data/generated/abi/obs.h) · [persist](../../modules/core/data/generated/abi/persist.h) · [persist_binary](../../modules/core/data/generated/abi/persist_binary.h) · [watcher](../../modules/core/data/generated/abi/watcher.h) · [image](../../modules/core/data/generated/abi/image.h) · [net](../../modules/core/data/generated/abi/net.h) · [control](../../modules/core/data/generated/abi/control.h) · [host_scene](../../modules/core/data/generated/abi/host_scene.h) · [runtime](../../modules/core/data/generated/abi/runtime.h) · [math](../../modules/core/data/generated/abi/math.h) |

## 唯一真相与重新生成

- **唯一真相**：`modules/core/data/schema/`（**目录**：一个文件一个模块 + `imports` 依赖，
  入口 `schema/schema.yaml`；规范见 [`SCHEMA.md`](../../modules/core/data/SCHEMA.md) §1）
  （`class_list` 数据类型 / `api_list` 函数接口 / `interface` 插件接口 /
  `control_list` 控制面方法 / `callbacks` 事件回调 / `module_abi` 模块 C ABI）。
- **生成**：构建配置期自动生成；手动重生成
  `cmake --build build --target maque_codegen`。
- **入库**：生成物是公开接口的交付物，随仓库提交
  （`modules/core/data/generated/` **不再被 gitignore**）。
- **漂移校验**：`ctest -R codegen_drift` —— 把 schema 重新生成到临时目录并与入库版本逐字节比对，
  防止"改了 schema 忘记提交生成物"。
- **不要手工编辑生成物**：改接口请改 `schema/`；各模块原先手写的公开头已退化为
  `#include "abi/<module>.h"` 的 shim。

机制说明见 [docs/21 接口声明与绑定生成](../21-接口声明与绑定生成.md)。
