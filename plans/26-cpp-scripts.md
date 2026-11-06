# 26 · C/C++ 脚本（编译型 guest：native / static / wasm）

> 上游：`docs/14`（脚本）、`docs/02`（模块 ABI）、`docs/05`（WASM/平台）、`docs/20`（资源模型）、
> `docs/21`（声明与绑定生成）、`docs/34`（模块/实例）、`plans/03`（module host）、`plans/16`（script）。
> 状态：**P0 已落地（桌面 native 动态 guest + 桌面 static E2E）**；
> Android/iOS/WASM 平台级 static 集成与 wasm guest 见 §9。
> Owner：`agent-script` + `agent-runtime`。
>
> 一句话：**C/C++ 不是"再加一个解释器"，而是同一套脚本 ABI 的"编译型 guest"**——
> 源码仍是资源，产物按目标平台编译，运行时经 `dlopen` / 静态注册接入现有 VM、总线、事件与热更。

## 0. 结论（先看这一页）

1. **复用现有脚本抽象，不另起炉灶**：新增 `ScriptLanguage::Native` + `native` 后端；
   `mq_script_vm` / `mq_script_bus` / 生命周期 / 热重载**语义不变**，C/C++ 只是第三个后端。
2. **API 面继续"声明即真相"**：从 `schema/ api_list` 生成 **C 函数表** `mq_script_api_v1` + C++ 包装；
   从 `callbacks` 生成 C++ 虚函数基类。与 JS/Lua 同一份声明、同一组名字，新增引擎 API 自动出现在 C++。
3. **加载复用 module host**：guest 导出 `mq_module_query`，宿主 `mq_host_load/query` 完成 dlopen、
   `struct_size`/`abi_version` 校验、Windows "不 FreeLibrary" 策略；guest vtable 是版本化 C ABI。
4. **编译/分发分三层**：
   - **native 动态**（Linux/Windows/macOS）：`.so`/`.dll`/`.dylib`，P0；
   - **static 注册**（Android/iOS/WASM/CI）：构建期编进宿主，P1；
   - **wasm guest**（远程/浏览器沙箱）：`.wasm` + 解释器，P2。
   同一份 `.cpp`、同一套 SDK、同一组脚本名，按平台选一种产物。
5. **信任边界必须写死**：**native 只允许本地可信内容**（`MAQUE_NATIVE=local` 默认）；
   远程游戏/浏览器不得加载 native，走 static 或 wasm。native 本来就能执行任意本机代码，
   不要假装 API 表是沙箱。
6. **不做** TCC/JIT、不做 C++ 解释器：C++ 解释器体积/ABI/性能都不可接受；TCC 只支持 C，
   且同样没有沙箱。C 在 ABI 层被支持，P0 的作者层先做 C++。

## 1. 现状盘点：哪些机制可以直接复用

| 现有机制 | 位置 | C/C++ 复用方式 |
|---|---|---|
| 统一后端 ABI | `modules/script/include/core/script/mq_script.h`、`src/script.cpp`、`src/vm_base.h` | 新增 `native` 实现；把 `is_lua()` 二选一重构为 backend ops 表 |
| JS/Lua 后端 | `src/js_backend.cpp`、`src/lua_backend.cpp` | 作为 `ScriptOps` 的两个实现，行为零变化 |
| 全部脚本 API | `schema/ api_list`（328 个，5 个命名空间） | 生成 C 函数表 + C++ 包装（唯一真相不变） |
| 事件回调 | `schema/ callbacks`（10 个） | 生成 C++ 虚函数基类与 `invoke()` 派发 |
| 模块宿主 | `modules/core/module/src/host.cpp` + `abi/module.h` | guest 复用 `mq_module_query` / `mq_host_load/query` |
| VM / 总线 / 热重载 | `src/bus.cpp`、`runtime/src/script/reload.cpp` | 不改；native VM 与 JS/Lua 同槽位、同顺序、同原子替换 |
| 包/模块/实例 | `docs/34`、`runtime/src/script/module.cpp` | P0 走旧"每脚本一个 VM"路径；P1 接入单 VM + 模块实例 |
| 资源/包清单 | `scenes/*/maque.package.json`、`runtime/src/script/module.cpp` | 扩展 `backends: ["native"]` 与 `native.targets` |
| 资产管线 | `docs/08`、`modules/asset` | "文本是开发期真相，二进制是产物"同样适用于 `.cpp → .so` |
| 模块热重载 | `docs/02 §6`、`plans/03` | 同一套"新代际成功才换入、失败保留旧代际" |
| 平台矩阵 | `CMakeLists.txt`、`docs/plan-wasm.md`、`android/` | 动态/静态/wasm 三种产物，逻辑 ABI 一致 |

**关键观察**：`api_list` 只有 328 个函数，参数/返回类型只有
`bool / int32_t / float / double / string / bytes / void`（`modules/core/data/schema/**` 实测），
全部是 C 兼容的 POD 或 `const char*`。这意味着**可以直接生成一张 C 函数指针表**，
不需要像 Lua/JS 那样做逐语言的编组 trampoline。

## 2. 方案对比

| 方案 | 开发体验 | 性能 | 跨平台/分发 | 远程安全 | 热更 | 结论 |
|---|---|---|---|---|---|---|
| TCC/libtcc 运行时编译 C | 只支持 C | 中 | 有限（x86/arm） | 否 | 是 | 不做：C++ 不支持，仍无沙箱 |
| C++ 解释器（Cling 等） | 好 | 低 | 差 | 否 | 是 | 不做：体积/ABI 灾难 |
| **native `.so`/`.dll`** | 好（本机工具链） | 100% | 每平台构建 | **否（可信）** | 重建 + 换入 | **P0** |
| **static 注册** | 差（重编宿主） | 100% | 全平台（含 Android/iOS/WASM） | 构建期信任 | 需重启 | **P1（桌面 E2E 已落地）** |
| **wasm guest** | 好（一次编译） | 30–80% | 全平台（含浏览器） | **是（沙箱）** | 载入字节 | **P2** |

结论：**native 先解决"桌面 C++ 玩法不重编引擎"，static 解决"受限平台"，wasm 解决"远程/浏览器"。**
三者共用 guest ABI 与源码级 API，但**不承诺同一个二进制跨层通用**。

## 3. 总体架构

```
                    ┌──────────────────────────────┐
                    │  scripts/*.cpp（唯一源码，资源）│
                    └──────────────┬───────────────┘
                                   │ CMake / maque watch
          ┌────────────────────────┼────────────────────────┐
          ▼                        ▼                        ▼
  native/<target>/*.so      static registry TU        native/<target>/*.wasm
  （P0 桌面）                （P1 Android/iOS/WASM）     （P2 远程/浏览器）
          │                        │                        │
          ▼                        ▼                        ▼
  mq_host_load/query        mq_script_static_guests()   wasm 解释器 + imports
          └────────────────────────┴────────────────────────┘
                                   ▼
                     same guest ABI / same lifecycle / same event names
                                   ▼
              mq_script_vm（native）→ mq_script_bus → 场景/组件/渲染
```

三种产物共享：

- **guest vtable**：`create / destroy / instantiate / instance_call / instance_has`；
- **host API 表**：`mq_script_api_v1`（static/wasm 也用同一张源码级 API）；
- **生命周期与事件名**：`on_start / update / on_* / on_destroy`；
- **错误与热更语义**：失败保留旧代际、`on_start` 重派、`props` 回灌。

## 4. 接口设计

### 4.1 语言识别

```yaml
# schema/script.yaml
- enum: ScriptLanguage
  values:
    - { name: JS, value: 0 }
    - { name: Lua, value: 1 }
    - { name: Python, value: 2 }
    - { name: V8, value: 3 }
    - { name: Native, value: 4 }   # 新增：C/C++ 编译型 guest
```

- `mq_script_lang_for_path`：
  - `.lua` → `lua`；
  - `.c` / `.cc` / `.cpp` / `.cxx` / `.c++` → `native`；
  - `.so` / `.dll` / `.dylib` → `native`（开发/测试的直接产物入口）；
  - `.wasm` → `wasm`（P2；未编入时创建失败并明确报错，不静默降级）；
  - 其余 → `js`。
- `mq_script_vm_create_lang("native")`；`mq_script_backends()` 在编入时列出 `native`。
- `.scn` **零改动**：`"script": "scripts/enemy.cpp"` 或 `"scripts": ["game.js", "ai.cpp"]`。
  扩展名决定语言；枚举 `ScriptLanguage::Native` 留给 `script.Script` 组件（尚未在 runtime 落地）。
- `modules/asset/src/asset.cpp` 的 `detect_kind` 把 `.c/.cc/.cpp/.cxx` 识别为 `MQ_ASSET_KIND_SCRIPT`
  （资产管线可跟踪源码依赖；产物 `.so` 不作为源资产入库）。

### 4.2 Host API 表（生成，不手写）

```c
/* 生成：modules/core/data/generated/maque_guest_api.h
 * 由 schema/ api_list 生成；与 maque_api.h 的签名逐项一致。 */
#define MQ_SCRIPT_API_VERSION 1u
#define MQ_SCRIPT_API_HASH    0x9e3779b9u   /* 有序签名的 FNV-1a；任何增删改都会变 */

typedef struct mq_script_api_v1 {
    mq_api_header header;       /* struct_size / abi_version */
    uint32_t      api_hash;
    const char*   target;       /* "linux-x86_64" / "windows-x86_64" / ... */
    const char*   engine_version;

    /* math */
    float (*math_clamp)(float value, float lo, float hi);
    float (*math_lerp)(float a, float b, float t);
    float (*math_saturate)(float x);

    /* storage（ctx 与 maque_api.h 一致） */
    const char* (*storage_get)(void* ctx, const char* key);
    float       (*storage_set)(void* ctx, const char* key, const char* value);

    /* host：313 个；命名空间前缀避免冲突 */
    void  (*host_log)(void* ctx, const char* msg);
    float (*host_entity_x)(void* ctx, float id);
    /* ... 全部 328 个 ... */
} mq_script_api_v1;
```

- 生成物：
  - `maque_guest_api.h`：C 类型 + `MQ_SCRIPT_API_HASH`；
  - `maque_guest_api_gen.cpp`：`extern "C" const mq_script_api_v1* mq_script_api_v1_get(void)`，
    聚合初始化全部字段，并 `static_assert(all_set(kApi))`（缺字段/空指针编译期失败）。
- 表是**进程级常量**，不放进任何全局可变状态；每个 VM 在 `configure` 时拿到同一指针。
- **字符串所有权**：表返回的 `const char*` 是"借用"，只在本次调用/下一次覆盖前有效
  （`storage.get` 复用 `Engine::script_ret`）。**C++ SDK 的包装一律立即复制成 `std::string`**，
  裸 C 作者必须遵守"下一次 API 调用前复制"。
- `bytes` 用现有 `mq_script_bytes_result`（带 `release`），C++ SDK 提供 RAII `Bytes`。
- 表头里的 `target` 由 CMake 的 `MQ_TARGET_KEY` 注入；guest 编译期也带同一个字符串，
  `create` 时比对，防止"Android arm64 的 .so 被放到 x86 桌面"这类错误。

### 4.3 Guest vtable（生成声明，guest 实现）

```c
/* 生成：modules/core/data/generated/abi/script_guest.h（新 schema: script_guest.yaml） */
#define MQ_SCRIPT_GUEST_ABI_VERSION 1u
#define MQ_SCRIPT_GUEST_IFACE_NAME "maque.script.guest.v1"

typedef struct mq_script_guest mq_script_guest;
typedef struct mq_script_guest_instance mq_script_guest_instance;
/* mq_script_prop：PropertyVariant 的 C POD 投影（name/type/4×double/string），P1 定义；
 * P0 的 mount 里传 NULL/0 即可。 */
typedef struct mq_script_prop mq_script_prop;

typedef struct mq_script_guest_mount {
    uint32_t    struct_size;
    int32_t     node;          /* -1 = 场景级 */
    int32_t     scene;         /* SceneInstance id */
    const char* module_key;    /* "pkg@ver/scripts/enemy.cpp" */
    const char* params;
    const mq_script_prop* props;   /* P1；PropertyVariant 的 C POD 投影 */
    uint32_t    prop_count;
} mq_script_guest_mount;

typedef mq_script_guest* (*mq_guest_create_fn)(
    const mq_script_api_v1* api, void* host_ctx, char* err, int err_cap);
typedef void (*mq_guest_destroy_fn)(mq_script_guest*);
typedef mq_script_guest_instance* (*mq_guest_instantiate_fn)(
    mq_script_guest*, const mq_script_guest_mount*, char* err, int err_cap);
typedef void (*mq_guest_instance_destroy_fn)(mq_script_guest_instance*);
typedef int  (*mq_guest_instance_has_fn)(mq_script_guest_instance*, const char* fn);
typedef int  (*mq_guest_instance_call_fn)(
    mq_script_guest_instance*, const char* fn,
    int nnum, const double* nums, const char* text,
    double* ret, char* err, int err_cap);

typedef struct mq_script_guest_vt {
    mq_api_header header;       /* struct_size / abi_version，供 mq_host_query 校验 */
    mq_guest_create_fn           create;
    mq_guest_destroy_fn          destroy;
    mq_guest_instantiate_fn      instantiate;
    mq_guest_instance_destroy_fn instance_destroy;
    mq_guest_instance_has_fn     instance_has;
    mq_guest_instance_call_fn    instance_call;
    /* 可选：mq_guest_last_error_fn last_error; 尾部追加，旧 host 按 struct_size 判空 */
} mq_script_guest_vt;
```

- guest 导出 **`mq_module_query`**（与渲染插件同一符号），返回 `&vt`：
  ```c
  extern "C" MQ_MODULE_EXPORT void* mq_module_query(const char* name, uint32_t min_version) {
      if (strcmp(name, MQ_SCRIPT_GUEST_IFACE_NAME) != 0) return nullptr;
      if (min_version > MQ_SCRIPT_GUEST_ABI_VERSION) return nullptr;
      return &kGuestVt;   /* header.struct_size = sizeof(mq_script_guest_vt) */
  }
  ```
- host 侧直接复用 `mq_host_load` + `mq_host_query`：dlopen 失败/符号缺失/版本不足都有现成错误码；
  Windows 的"不 FreeLibrary"策略自动继承。
- `create` 必须校验：`api->header.abi_version`、`api->header.struct_size >= sizeof(mq_script_api_v1)`、
  `api->api_hash == MQ_SCRIPT_API_HASH`、`strcmp(api->target, MQ_TARGET_KEY) == 0`；
  不匹配 → 写 `err` 返回 NULL，**明确失败**。
- 所有函数指针字段**尾部只能追加**；`struct_size` 守卫旧调用方。

### 4.4 C++ SDK（从 schema 生成）

```cpp
// 生成：generated/maque/script.hpp（C++17 起可用；不需要 C++23）
namespace maque {

class Context {
public:
    void bind(const mq_script_api_v1*, void* host, const mq_script_guest_mount*);
    int node() const; int scene() const;
    std::string_view module_key() const;

    // 每个 api 命名空间一个代理对象：ctx.host.log(...) / ctx.math.clamp(...) / ctx.storage.get(...)
    struct HostApi {
        Context& c;
        void  log(std::string_view msg) const { auto s = std::string(msg); c.api_->host_log(c.host_, s.c_str()); }
        float entity_x(float id) const { return c.api_->host_entity_x(c.host_, id); }
        std::string entity_name(float id) const { return c.api_->host_entity_name(c.host_, id); } // 立即复制
        // ... 313 个
    } host{*this};

    struct MathApi { float clamp(float v, float lo, float hi) const; /* ... */ } math{*this};
    struct StorageApi { std::string get(std::string_view k) const; /* ... */ } storage{*this};
    // zstd / zlib ...
};

class Script : public Context {
public:
    virtual ~Script() = default;
    virtual void on_start() {}
    virtual void update(double dt) {}                    // 数值用 double，与总线一致
    virtual void on_pointer_down(double target, double x, double y) {}
    virtual void on_text(double target, std::string_view text) {}
    virtual void on_destroy() {}
    // 其余 callback 由 callbacks 生成

    int invoke(const char* fn, int nnum, const double* nums, const char* text, double* ret);
    int has(const char* fn) const;
};

} // namespace maque

// 一个 .so 一个脚本类；P0 单类，P1 支持 #Name 多导出
MAQUE_SCRIPT(MyEnemy)
```

- `Script::invoke` 由生成器按 `schema callbacks` 生成（名字、参数个数、string 参数位置全部来自 DSL）；
  未知名字返回 1，参数不足返回 2。
- `MAQUE_SCRIPT(Class)` 展开为 `mq_module_query` + `create/instantiate/instance_call` 的模板胶水：
  - `create` 校验 API 表并 `new` 一个内部 `Guest<Class>`；
  - `instantiate` 创建 `Class`，绑定 mount（node/scene/module_key/props）；
  - `instance_call` 用 `try/catch(...)` 包住 `invoke`，**异常绝不跨 .so 边界**；
  - 所有字符串在进入 guest 时复制、离开 guest 时由 host 立即复制。
- C 作者直接用 4.3 的 C vtable；`MAQUE_GUEST_C(...)` 宏是 P1 的糖，不影响 ABI。
- `props`：P1 用 `mq_script_prop` POD 数组（type + 4 个 double + string），不传 `std::map`/JSON 对象。

### 4.5 后端派发重构（P0 前置）

现状 `modules/script/src/script.cpp` 到处是 `is_lua(vm) ? lua::... : js::...`。加第三个后端前先收敛：

```cpp
// src/vm_base.h
struct ScriptOps {
    const char* id;
    mq_script_vm* (*create)();
    void (*destroy)(mq_script_vm*);
    int  (*eval)(mq_script_vm*, const char* src, const char* filename);
    int  (*has)(mq_script_vm*, const char* fn);
    int  (*call)(mq_script_vm*, const char* fn, int nnum, const double* nums,
                 const char* text, double* ret);
    int  (*last_error)(mq_script_vm*, char* out, int cap);
    void (*set_rng)(mq_script_vm*, mq_script_rng_fn, void*);
    void (*frame_boundary)(mq_script_vm*);
    int  (*register_fn)(/* ... */);   // 可共用 no-op 实现
};

struct mq_script_vm {           // 内部布局，ABI 仍是不透明指针
    const char* lang;
    const ScriptOps* ops;
};
```

`script.cpp` 的每个导出函数改成 `vm->ops->...`；JS/Lua 各填一张 `ScriptOps`。
**行为零变化**由现有 `script` / `lua` / `multilang` / `bindings_e2e` / `hot_reload` 守住。
这是"加语言"最容易被低估的一步：不做的话，后面每个调用点都要 `is_lua || is_native`，必然漏。

### 4.6 脚本 ABI v4：native 的配置面

`abi/script.h` 从 v3 升到 v4，只做**追加**：

```c
typedef int (*mq_script_artifact_resolver_fn)(
    void* user, const char* source_path, char* out_spec, int cap);

typedef struct mq_script_vm_native_config {
    uint32_t struct_size;
    const struct mq_script_api_v1* api;   /* 前向声明，避免与 maque_guest_api.h 形成包含环 */
    void* host_ctx;                       /* Engine*，guest 只当不透明句柄 */
    mq_script_artifact_resolver_fn resolver;
    void* resolver_user;
} mq_script_vm_native_config;

int mq_script_vm_configure_native(mq_script_vm* vm, const mq_script_vm_native_config* cfg);
```

- JS/Lua 实现返回 1（不支持），runtime 忽略；native 实现保存并用于 `eval`。
- 不引入进程级可变的 API 表指针；`api` 随 VM 配置传入，多 Engine 实例互不影响。
- `resolver` 返回的 spec 前缀：
  - `so:<绝对路径>` / `soname:<库名>`（`mq_host_load` 直接吃）；
  - `static:<key>`（静态注册表）；
  - `wasm:<路径>`（P2）。
- `source_path` 是 VFS 路径（如 `/scenes/native/scripts/ball.cpp`）；resolver 由 runtime 实现，
  负责包根/场景目录/目标平台/manifest 的解析，native 后端不碰文件布局。

## 5. 运行时集成

### 5.1 创建与加载

`load_instance_scripts`（`runtime/src/scene/instance.cpp`）对 native 只需多三步：

```cpp
mq_script_vm* one = mq_script_vm_create_lang(want);
mq_script_vm_native_config cfg{};
cfg.struct_size = sizeof cfg;
cfg.api         = mq_script_api_v1_get();
cfg.host_ctx    = this;
cfg.resolver    = native_resolve_cb;   // Engine 实现
cfg.resolver_user = this;
(void)mq_script_vm_configure_native(one, &cfg);
mq_script_set_rng(one, ...);           // native 不用；保留调用，语义一致
mq_bind_generated(one, this);          // native 的 register_* 全是 no-op
mq_script_eval(one, src.c_str(), vfs_path.c_str());
```

- `mq_bind_generated` 仍然调用：native 的 `register_*` 一律返回 0 即可，不改生成代码。
- **源码读取**：native 仍读 `.cpp` 字节用于内容哈希/诊断；但**允许源码缺失**（只发布二进制的包），
  此时 `src` 传空，由 resolver 成功与否决定。现有 `if (src.empty()) error` 分支需按语言区分。
- `global.cpp` / `overlay.cpp` / `reload.cpp` 各有一份"创建 + 绑定 + eval"，抽成一个
  `Engine::make_script_vm(lang, source_path)`，避免四处漏掉 configure（这是当前 JS/Lua 路径的既有重复）。

### 5.2 manifest 与路径解析

`native` 的产物不进 VFS 字节流（`.so` 必须能被 `dlopen`，zip/http 里的字节不能直接当共享库）。
P0 只支持**本地文件系统**；zip/asset/http 要 native 时给出明确错误（P1 再做"解压到缓存 + 校验"）。

解析顺序（`Engine::native_resolve`）：

1. `MAQUE_NATIVE_MANIFEST`（测试/开发显式指定）；
2. `<包根>/native/<target>/manifest.json`；
3. `<场景目录>/native/<target>/manifest.json`；
4. `<包根>/native/manifest.json`（平台无关的 static 表）。

`<target>` 由 CMake 注入的 `MQ_TARGET_KEY` 决定：
`linux-x86_64` / `linux-aarch64` / `windows-x86_64` / `macos-arm64` /
`android-arm64-v8a` / `wasm32`。

manifest（**生成物**，不是手写真相）：

```json
{
  "version": 1,
  "api": 1,
  "api_hash": "0x9e3779b9",
  "target": "linux-x86_64",
  "scripts": {
    "scripts/ball.cpp": {
      "kind": "shared",
      "artifact": "native/linux-x86_64/libball-3f2a91c0-1a2b3c4d.so",
      "source_hash": "0x...",
      "artifact_hash": "0x..."
    },
    "scripts/enemy.cpp": {
      "kind": "static",
      "key": "td/scripts/enemy.cpp"
    }
  }
}
```

- `source_hash` 用于检测"源码改了但产物没重编"（开发期警告）；
- `artifact_hash` 用于完整性校验（远程 P2 必须；本地可选）；
- `api`/`api_hash` 与宿主不符 → 明确报错，不尝试加载；
- 路径一律相对 manifest 所在目录，使用 `/`，与包根无关的绝对路径禁止入库。

### 5.3 热重载

复用现有"新 VM 成功才换入"的原子语义（`runtime/src/script/reload.cpp`、`src/bus.cpp`）：

1. 开发工具 `maque watch` 监视 `.cpp`/头文件，调用 CMake 重建，成功后**原子替换 manifest**
   （临时文件 + rename），产物文件名带新内容哈希；
2. 引擎的 watcher（`MAQUE_WATCH=1`）监视 manifest / 产物；
3. 帧边界：resolver 重新解析 → 新 artifact 的 VM 创建 + eval → 成功才 `mq_script_bus_swap`；
   失败销毁新 VM、保留旧 VM；
4. 换入后对新 VM 只派发一次 `on_start`，并先摘掉旧槽位的 `node_on` 注册（与 JS/Lua 完全一致）。

要点：

- **文件名内容寻址**（`libenemy-<source_hash>-<api_hash>.so`）：Windows 不能覆盖正在加载的 DLL，
  新代际必须是新文件；旧文件保留到进程退出。
- Windows 上 `mq_host` 默认**不 `FreeLibrary`**（`modules/core/module/src/host.cpp` 顶部注释），
  热更会累积地址空间；长会话建议重启，或按 `MAQUE_MODULE_FORCE_UNLOAD=1` 自行承担风险。
- Linux 可在旧实例销毁后 `dlclose`，但不要依赖"一定卸载"（有线程/TLS 的库可能被保留）。
- **状态语义与 JS/Lua 相同**：实例状态不跨重载保留；`props`/组件字段是持久真相。
- 引擎**不调用编译器**（不在运行时执行外部命令）；编译由开发者工具/CI 完成，避免把游戏包变成
  "可执行任意构建命令"的入口。

### 5.4 静态注册（Android/iOS/WASM/CI）

无 `dlopen` 的平台用构建期注册：

桌面已落地（`cmake/maque_script.cmake` 的 `maque_add_native_script_static`）：

```cpp
/* 生成：<target>_static_registry.cpp（全局构造期注册） */
extern "C" const mq_script_guest_vt* mq_guest_vt_tank_native_game(void);
namespace {
struct StaticGuestRegistrar {
    StaticGuestRegistrar() {
        (void)mq_script_guest_register_static("tank_native_game", mq_guest_vt_tank_native_game());
    }
};
StaticGuestRegistrar g_registrar;
}
```

- guest 源码用 `MAQUE_SCRIPT_STATIC(Class, UniqueId)` 生成**唯一符号**的 vtable
  （避免多个 guest 同时定义 `mq_module_query` 造成重复符号）；动态构建才导出 `mq_module_query`。
- 调用方把 `$<TARGET_OBJECTS:<guest>>` 加进宿主；`maque_player` 的 static 变体
  `tank_native_static_player` 即此路径（不产生/加载 `.so`）。
- `native` 后端在 `static:` spec 下线性查表；找不到 → 明确报错（列出声明的 key）。
- 同一份 `.cpp` 在 static 与动态下走同一 vtable、同一 API 表，行为一致。
- Android/iOS/WASM 的生产宿主只需复用同一 OBJECT 库；Gradle/Xcode/WASM 集成仍是 P1。

## 6. 编译与分发

### 6.1 CMake helper

```cmake
# cmake/maque_script.cmake（现状）
maque_add_native_script(enemy SOURCE scripts/enemy.cpp)          # .so/.dll 动态 guest
maque_add_native_script_static(enemy_static
    SOURCE scripts/enemy.cpp
    KEY    td/enemy.cpp             # manifest 的 "key"
    ID     td_enemy)                # MAQUE_SCRIPT_STATIC 的唯一符号后缀
target_sources(host PRIVATE $<TARGET_OBJECTS:enemy_static>)
```

职责：

- 动态 helper 建 `MODULE` 库；静态 helper 建 `OBJECT` 库并生成注册 TU（`MAQUE_NATIVE_STATIC`）；
- 设 `CXX_STANDARD 17`（SDK 不要求 C++23）、`POSITION_INDEPENDENT_CODE ON`；
- 继承引擎的 `-ffp-contract=off`、`-fvisibility=hidden`（D8/R8）；
- 只加 `${MAQUE_GEN_DIR}` + SDK include，**不链接 `maque_runtime`/`maque_schema`**
  （guest 只能经 API 表访问引擎；链接引擎会绕过 ABI 边界并制造重复符号）；
- 生成 `maque_script_stamp.h`：`MQ_SCRIPT_API_HASH`、`MQ_TARGET_KEY`、`MQ_SCRIPT_GUEST_ABI_VERSION`、
  编译器/构建 ID；
- 动态 POST_BUILD：按 `source_hash + api_hash` 复制成内容寻址文件名，并写 manifest 片段；
  静态路径没有产物复制，manifest 只写 `kind=static,key=...`。

样例（in-tree）：

```cmake
maque_add_native_script(scenes_native_ball SOURCE scenes/native/scripts/ball.cpp PACKAGE native)
```

外部游戏工程：把 `cmake/maque_script.cmake` + `generated/` + `include/maque/` 作为 SDK 安装/复制，
`add_subdirectory(maque)` 或 `find_package(maque)`（P1 补 install/export）。

### 6.2 ABI 与编译参数（必须与宿主一致）

| 项 | 要求 |
|---|---|
| 编译器族 | 与宿主同族/同 ABI：Linux GCC/Clang 皆可（libstdc++ ABI），Windows clang-cl/MSVC（MSVC ABI） |
| C++ 标准 | ≥ C++17（SDK）；引擎 C++23 不要求 guest 跟随 |
| 浮点 | `-ffp-contract=off`（D8），禁 `-ffast-math`；禁 `-march=native` |
| 可见性 | `-fvisibility=hidden`，只经 `MQ_MODULE_EXPORT` 导出 `mq_module_query` |
| CRT/STL | Windows `/MD`（与宿主一致）；Linux release 建议 `-static-libstdc++ -static-libgcc`；Android 用 NDK 同款 `c++_shared` |
| Sanitizer | 宿主开 ASan 时 guest 同开；Windows 需把 ASan 运行时 DLL 放到 guest 旁（复用 `maque_deploy_asan`） |
| 异常/RTTI | guest 内部可用；**不得跨 vtable 边界**；`instance_call` 在 guest 侧 `catch(...)` |

编译器/标志不一致的典型症状是"能加载、随机崩"；因此 manifest 记录编译器 ID/版本/构建 ID，
加载时只做**诊断输出**（无法可靠校验），CI 用矩阵保证一致。

### 6.3 产物命名与 manifest 生成

```
libenemy-<source_hash8>-<api_hash8>.so
enemy-<source_hash8>-<api_hash8>.dll
enemy-<source_hash8>-<api_hash8>.dylib
```

- 内容寻址：同一份源码 + 同一 API + 同一标志 → 同一文件名（可复现、可缓存）；
- 旧代际保留：热更/多实例不会互相覆盖；
- packer 可清理不再被 manifest 引用的旧文件；
- manifest 是**派生文件**，随包分发；源码可以不随包分发（只发 manifest + 产物）。

### 6.4 平台矩阵

| 平台 | 推荐层 | 产物 | 加载方式 | 备注 |
|---|---|---|---|---|
| Linux x86_64/aarch64 | native 动态 | `.so` | `dlopen`（`mq_host`） | P0 主战场；`dlclose` 可用但不保证 |
| Windows x86_64 | native 动态 | `.dll` | `LoadLibraryA`（`mq_host`） | 默认不 FreeLibrary；内容寻址避免覆盖 |
| macOS arm64/x86_64 | native 动态 | `.dylib` | `dlopen` | 需 codesign/硬化运行时；未来目标 |
| Android arm64-v8a | static 优先 | 链接进 `libmain.so` | 注册表 | P0 不做动态；P1 可 `jniLibs` + `soname` |
| iOS | static | — | 注册表 | App Store 不允许下载代码；仅静态 |
| WASM/浏览器 | static 优先 | 链接进 `maque_web` | 注册表 | P0；P2 才做 `.wasm` guest |
| headless/CI | native 动态或 static | `.so`/注册表 | 同上 | 单测用动态，WASM 门用 static |

### 6.5 包布局

```
td/
  maque.package.json           # { id, version, api, backends: ["js","native"], native: { abi: 1 } }
  scenes/...
  scripts/enemy.cpp            # 源（发布包可裁剪）
  native/
    linux-x86_64/manifest.json
    linux-x86_64/libenemy-<hash>.so
    windows-x86_64/manifest.json
    windows-x86_64/enemy-<hash>.dll
    android-arm64-v8a/manifest.json   # P1（static 时可为空）
```

`maque.package.json` 只做**声明**：

```json
{
  "id": "td",
  "version": "1.0.0",
  "api": 1,
  "backends": ["js", "native"],
  "native": { "abi": 1 }
}
```

- `detect_package` 现在只认 `backends` 里的 `"js"`（`runtime/src/script/module.cpp`），
  必须改成"包含任一当前构建支持的后端（js/lua/native）"，否则 native-only 包不会激活；
- `api` 必须 ≤ `MQ_SCRIPT_API_VERSION`；manifest 的 `api_hash` 必须完全匹配；
- `native` 节点可选；没有它但 `backends` 含 `native` 时按约定目录自动探测。

### 6.6 Android / WASM / Windows 的落点

**Android（P0：static）**
- 现有 `android/app/src/main/cpp/CMakeLists.txt` 只链接 `maque::runtime`；追加一个
  `maque_static_scripts` OBJECT 库 + 生成的注册 TU，链接进 `libmain.so`。
- 场景仍以 `asset://` 从 APK assets 读取；`.cpp` 源码在构建期由 Gradle/CMake 编译，
  不需要把 `.so` 解压到可写目录。
- P1 动态：把 per-script `.so` 放进 `jniLibs/<abi>/`，manifest 写 `"kind": "soname"`，
  `dlopen("libenemy.so")` 由 Android linker 在 app 的 native library dir 找到。

**WASM（P0：static；P2：wasm guest）**
- P0：`samples/web` 把 native guest 源编进 `maque_web`，注册表 key = 包内源码路径；
  scene 仍引用 `.cpp`，resolver 返回 `static:<key>`。这与"不 dlopen"的 P0 约束一致。
- P2：`.wasm` guest 是另一个后端：C++ SDK 编译到 wasm32（wasi-sdk/libc++ 或 `-nostdlib` 子集），
  host 侧嵌入 wasm3/WAMR，API 通过 import 模块 `maque` 暴露（**不是** 4.2 的 64 位函数指针表，
  需要线性内存/32 位指针的编组层）。这才是远程/浏览器 C/C++ 的安全答案；工作量显著大于 native。

**Windows**
- `mq_host_load` 已处理 `LoadLibraryA`；`.dll` 依赖（ASan runtime、libstdc++/MSVC runtime）
  必须与 DLL 同目录或 PATH。复用 `maque_deploy_plugin_deps`。
- 禁止覆盖已加载 DLL：内容寻址文件名是硬要求，不是优化。

## 7. 安全、确定性与性能

### 7.1 信任模型

| 来源 | native 动态 | static | wasm |
|---|---|---|---|
| 本地包（`file://`） | 默认允许（`MAQUE_NATIVE=local`） | 允许 | 允许 |
| zip/asset 包 | P0 拒绝（不能直接 dlopen） | 允许（构建期） | 允许 |
| 远程 `http(s)://` | **默认拒绝** | 不适用 | 允许（P2） |
| 未签名第三方包 | 允许但打警告 | 同左 | 允许 |

- `MAQUE_NATIVE=0|local|any`：`0` 关闭后端；`local`（默认）只允许本地文件；`any` 允许远程
  并打印显著警告（P2 用签名/哈希替代）。
- native guest 可以 `dlopen`/syscall/读文件，**API 表不是安全边界**；真正的隔离只有
  独立进程或 wasm。文档必须直说，不能把 native 宣传成"沙箱脚本"。
- 崩溃上下文（`runtime/src/engine/crash.cpp`）记录 guest 路径、API hash、target、module key。

### 7.2 确定性

- **同二进制同输入**：引擎固定步长 + 总线顺序不变 → 可复现；native 不引入新的调度。
- **guest 合同**（写入 SDK 文档 + 构建 lint 警告）：
  - 只用 `host.rng()`（引擎 RNG），禁 `rand()/std::random_device/time()`；
  - 不创建线程/协程/异步；不阻塞；不做帧内 IO；
  - 不依赖未初始化内存/地址/迭代顺序；
  - 浮点遵循 D8：`-ffp-contract=off`，不做 fast-math。
- **跨语言位级一致**：JS/Lua 全程 double；C++ guest 的回调参数用 double、内部数学用 double、
  只在 API 边界转 float，可与 JS/Lua 对齐。若 guest 用 float 做内部累积，不保证与 JS 位级一致——
  这是"同语言确定"与"跨语言确定"的区别，测试要按后者设计。
- 引擎无法抢占/看门狗一个死循环的 native 调用；只能在文档与评审层约束（信任层）。

### 7.3 性能与可观测

- 调用路径：`instance_call` → 生成的 `invoke`（10 个 `strcmp`）→ 虚函数 → API 表函数指针 → 引擎函数。
  没有 JSON/反射/逐参数编组；预期接近直接 C++ 调用（相对 JS 解释器有数量级优势）。
- Tracy zone：`script.native.load` / `script.native.call` / `script.native.destroy`；
  内存域 `script` 记录已加载模块数/字节（guest 内部堆无法自动统计，除非 SDK 走 host allocator，P2）。
- `MAQUE_NATIVE_INFO=1`：打印 target、artifact、api_hash、module key、加载耗时、guest 编译器 stamp。

## 8. 测试计划

| # | 测试 | 内容 | 期望 |
|---|---|---|---|
| 1 | `native_abi`（单测） | 编译 fixture guest，加载、`has/call`、错误、销毁 | 0 泄漏/0 崩溃；错误码正确 |
| 2 | `native_api`（单测） | guest 调 `host.log`、`math.clamp`、`storage.set/get`、`zstd` bytes | 与 JS/Lua 同结果 |
| 3 | `native_static`（单测） | 静态注册表查找、唯一符号、重复 key | 命中/明确报错 |
| 4 | `native_bad_api`（单测） | guest 用错误 `api_hash`/target | eval 失败，错误信息含版本；旧 VM 不受影响 |
| 5 | `native_missing`（单测） | manifest 缺失/目标平台缺失/产物缺失 | 明确错误路径与提示 |
| 6 | `native_hot_reload`（单测） | v1→v2 新产物、旧 VM 保留/失败回滚 | 新行为生效、旧行为不丢 |
| 7 | `native_determinism`（e2e） | 同一 native 场景跑两遍 | `world_trail` 一致 |
| 8 | `native_cross_lang`（e2e） | `["game.js","ai.cpp"]` 双向事件 | 顺序/状态/错误隔离与 `multilang` 同 |
| 9 | `native_package`（e2e） | 包布局 + manifest + 玩家运行 | 产物被选中；缺 target 报错 |
| 10 | `native_android_static`（构建门） | Gradle/CMake 编入注册表 | 至少编译通过 |
| 11 | `native_wasm_static`（WASM 门） | `maque_web` 编入静态 guest，跑 60 帧 | checksum 非零、可复现 |
| 12 | `native_windows`（CI） | `.dll` 加载 + 热更 + 退出 | 不崩、不覆盖已加载 DLL |

护栏：

- `ctest -R codegen_drift`：新增生成物（`maque_guest_api.*`、`abi/script_guest.h`、`maque/script.hpp`）
  必须入库；
- `ctest -R 'script|lua|multilang|bindings_e2e|hot_reload'`：后端重构的行为零变化门；
- `ctest -R runtime_oracle`：native 样例加入 oracle 场景集（如果 e2e 走玩家）。

## 9. 分阶段实施

### P0 — 桌面 native（建议先做，1–2 周量级）

1. `ScriptOps` 重构（JS/Lua 行为零变化，先跑绿现有测试）。
2. schema：`ScriptLanguage::Native`；`abi/script.h` v4（`configure_native`）；`script_guest.yaml`。
3. parser：生成 `maque_guest_api.h/.cpp`、`abi/script_guest.h`、`maque/script.hpp`；API hash。
4. `native_backend.cpp`：`mq_host` 加载、guest 生命周期、`has/call`、错误、resolver spec 解析。
5. runtime：`Engine::make_script_vm` + `native_resolve`（manifest/target/信任门）；
   `load_instance_scripts`/global/overlay/reload 接入。
6. `cmake/maque_script.cmake` + 样例 `scenes/native/`（ball 的 C++ 版）+ manifest 生成。
7. 单测 1–6 + e2e 7/8；文档 `docs/14`/`docs/21` 更新。

**P0 退出标准**：`maque_player scenes/native/ball.scn` 可跑；同一场景 JS 与 C++ 的
`world_trail` 一致（或明确记录浮点差异）；热更失败保留旧逻辑；Windows/Linux 都能加载。

### P1 — 分发与受限平台（+1–2 周）

1. 包清单 `backends/native` + `detect_package` 修复；packer（manifest 合并、hash、裁剪旧代际）。
2. 模块/实例路径（docs/34）：单 native VM + 每挂载点 `instantiate`，`props` POD 回灌。
3. Android static（Gradle/CMake + 注册表）；WASM static（`samples/web`）。
4. `maque watch` 开发工具（重建 + 原子 manifest；引擎只 watcher，不跑编译器）。
5. `native_package` / Android / WASM 测试门。

### P2 — wasm guest 与远程沙箱（+2–3 周）

1. wasm32 工具链 + SDK 子集（libc++/wasi 或 `-nostdlib`）。
2. host 侧 wasm3/WAMR 嵌入 + import 编组（字符串/bytes/线性内存）。
3. 燃料/内存预算、确定性、远程包签名/哈希；`MAQUE_NATIVE=any` 的替代。
4. 多导出（一个库多个脚本类）、C SDK 宏、进程隔离（可选）。

## 10. 风险与开放问题

| 风险 | 处理 |
|---|---|
| 编译器/CRT/STL 不一致 | 同工具链矩阵；manifest 记录 stamp；release 静态链接 libstdc++/MSVC runtime |
| API 表漂移 | 生成 `api_hash` + `struct_size`；不匹配明确拒绝；`codegen_drift` |
| Windows 不能卸载 DLL | 内容寻址 + 保留旧代际；长会话重启；`MAQUE_MODULE_FORCE_UNLOAD` 仅测试 |
| 静态库重复符号 | `MAQUE_SCRIPT_STATIC` 生成唯一符号；注册 TU 由构建生成 |
| 远程加载 native | 默认拒绝；wasm 才是安全路径；P2 签名 |
| Android 动态打包复杂 | P0 只做 static；P1 再评估 `jniLibs`/`soname` |
| WASM C++ 运行时 | wasi-sdk/libc++ 体积与 import 限制；先做 C 子集/静态 |
| 浮点跨语言不一致 | 回调/内部用 double，API 边界转 float；测试明确"同语言 vs 跨语言" |
| 热更状态丢失 | 与 JS/Lua 相同：props/组件是持久真相；文档写清 |
| iOS/macOS 签名 | 暂不作为目标；iOS 仅 static；macOS 需 codesign/硬化运行时 |
| 死循环/崩溃 | 信任层；崩溃上下文记录 guest；不假装能恢复 |

**需要拍板的开放问题：**

1. `.scn` 引用**源码**（`enemy.cpp`）还是**产物描述**（`.mqn`/`.so`）？
   建议源码：可移植、可 diff、与 JS/Lua 一致；manifest 负责映射。
2. P0 是否允许一个 `.so` 多个脚本类？建议先"一库一类"，P1 再用 `#Export` 扩展。
3. C（非 C++）作者层是否 P0 就要？ABI 已支持；建议 P1 补 `MAQUE_GUEST_C` 宏。
4. Android 是否接受 P0 仅 static？这会失去"下载即玩 native"的能力，但与商店规则一致。
5. Web 是"static 编进引擎"还是等 P2 wasm？取决于是否要"浏览器里下载 C++ 游戏"。
6. native 包是否要求签名后才能从远程加载？建议 P2 与 wasm 一起做，native 永久默认拒绝远程。
7. API 表兼容策略：精确 `api_hash`（简单、需重编）还是前缀兼容（可混版本、复杂）？
   建议 native 精确匹配；wasm 可再评估。

---

**最小可行切片（如果只做一件事）**：P0 的 1–5 步 + 一个 `scenes/native/hello.cpp` 样例。
它证明"同一份 API 声明、同一总线、同一热更语义"能被 C++ 走通；分发矩阵可以后补。

## 11. 落地状态（P0，已实现）

- **schema**：`script.yaml` 新增 `ScriptLanguage::Native`；`module_abi` v4 增加
  `mq_script_vm_native_config` / `mq_script_vm_configure_native`；新增 `schema/script_guest.yaml`
  （`maque.script.guest.v1` vtable）。
- **codegen**：`maque_guest_api.h`（C 函数表 + `api_hash`）、`maque_guest_api_gen.cpp`
  （聚合初始化 + `static_assert` 完整性）、`abi/script_guest.h`、`maque/script.hpp`
  （C++ Context/Script/SDK 宏）；`codegen_drift` 通过。
- **后端**：`ScriptOps` 表重构（JS/Lua 行为零变化）；`native_backend.cpp` 用 `mq_host_load/query`
  加载 guest，处理 create/instantiate/call/错误/静态注册；`script.cpp` 统一派发。
- **runtime**：`Engine::make_script_vm` + `native_resolve`（manifest/target/`MAQUE_NATIVE` 信任门）；
  instance/global/overlay/reload 路径接入；`detect_package` 接受 native backends。
- **编译/分发**：`cmake/maque_script.cmake` 的 `maque_add_native_script`（C++17 guest、不链接引擎）；
  样例 `scenes/native/` + `native_script_e2e`（manifest 在 `build/scenes/native/`），
  以及真实游戏迁移 `samples/tank-native/tank-nativel.scn`（根玩法 C++、HUD 仍 Lua，
  `tank_native_smoke/title/e2e_smoke/input/play/rules`；smoke/play/rules 复用 `tests/tank_e2e.sh`）。
- **静态链接（桌面 E2E）**：`maque_add_native_script_static` 编 OBJECT 库 + 生成全局构造注册 TU；
  `MAQUE_SCRIPT_STATIC(Class, Id)` 导出唯一 `mq_guest_vt_<Id>()`；宿主链接
  `$<TARGET_OBJECTS:...>` 即可，无 `.so`、无 manifest artifact。样例
  `samples/tank-native/tank_native_static_player` + `tank_native_static(_smoke)`。
- **测试**：`native` 单测（ABI/hash/target/派发/重载/错误）+ `native_script_e2e`；
  全量 `ctest` 158/158。
- **未做（P1/P2）**：Android/iOS/WASM 的平台级 static 集成（OBJECT 库/注册 TU 已可复用）、
  wasm guest、native 的模块/实例面（docs/34）、自动监视产物/rebuild 工具、C 作者层宏。
- **已知语义差异**：native SDK 的 `has` 对全部 schema callbacks 返回 true（基类默认空实现），
  所以总线会把这些事件投给 native VM（无副作用）；精确订阅（只投实际 override 的回调）
  需要 P1 的声明式 handler 列表 / 编译期反射。JS/Lua 的 `has` 仍按“是否定义函数”判定。
