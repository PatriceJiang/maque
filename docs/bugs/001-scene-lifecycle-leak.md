# 001 · 场景生命周期泄漏 / 热重载 bad-free

- **类型**：内存 + 崩溃
- **状态**：fixed（`d517096`）
- **影响面**：所有场景的退出与热重载；`scenes/render_texture/render_texture_3d.scn` 是最小暴露场景（2 个 RenderTexture + 2 个 glTF）
- **发现版本**：`1417147` 附近；用 `Reload.scene` 或泄漏检查复现

## 症状

- `render_texture_3d.scn` 在 inspector / 控制面执行 `Reload.scene` 时进程崩溃：Release 退出码 `0xC0000374`（HEAP CORRUPTION）。
- ASan 构建报：

  ```text
  ERROR: AddressSanitizer: attempting free on address which was not malloc()-ed
    #1 Engine::reload_scene modules/runtime/src/runtime.cpp
  SUMMARY: AddressSanitizer: bad-free ... Engine::reload_scene
  ```

- 即使不崩溃（Release 下 invalid free 被吞掉），也会在退出/切场景时泄漏：
  - 场景组件 blob 里的 `std::string` / 动态数组（RenderTexture URI、glTF 路径、Sprite 贴图路径等）没有析构；
  - 热重载建立的堆上 `Scene` 没有所有者，退出或 `director.load_scene` 后整棵泄漏。

> 注意：**场景正常运行本身不泄漏**——private bytes / GPU 显存 / handle 数在长跑中都是平台期。泄漏只发生在
> **销毁 / 热重载 / 切场景**的生命周期路径。

## 最小复现

```bash
cat > /tmp/reload.jsonl <<'JSON'
{"jsonrpc":"2.0","id":1,"method":"Runtime.step","params":{"frames":2}}
{"jsonrpc":"2.0","id":2,"method":"Reload.scene","params":{}}
JSON

MAQUE_WINDOW=0x0 MAQUE_PRESENT=off MAQUE_CONTROL=- \
  ./build/samples/player/maque_player scenes/render_texture/render_texture_3d.scn < /tmp/reload.jsonl
```

修复前：ASan 在 `Engine::reload_scene` 报 bad-free；Release 在 `Reload.scene` 后堆损坏。
修复后：正常返回 `{"reloaded":true,...}`，退出码 0。

## 根因

三个独立问题叠加：

1. **对内嵌场景调用 `delete`**：根场景默认是 `Engine::scene_store`（成员对象），但
   `reload_scene()` 仍执行 `delete scene`。对非堆地址调用 `operator delete` → bad-free / 堆损坏。
   （`scene_owned` 声明了却从未使用。）
2. **热重载建立的堆场景无人释放**：`reload_scene()` 里 `new Scene()` 后直接赋给 `scene`，
   退出或下一次 `director.load_scene` 时没有所有者释放它。
3. **反射表先于场景销毁**：`teardown()` 先 `mq_reflect_destroy(reflect)`，再在 `delete eng` 时析构场景。
   `Scene::~Scene()` → `destroy_blob()` 依赖反射表取类型描述；反射表为空时只 `blob.clear()`，
   **跳过 `std::string` 析构** → 组件里的路径/名字堆缓冲全部泄漏。

## 修复

- [runtime.cpp:6681](../../modules/runtime/src/runtime.cpp#L6681) 起：根场景是内嵌对象时只
  `scene_store = Scene()`；堆场景交给 `scene_owned`（`unique_ptr`），替换时由它释放。
- `load_scene_into()` 在换回 `scene_store` 前 `scene_owned.reset()`，切场景不泄漏旧堆场景。
- `teardown()` 在 `mq_reflect_destroy` **之前**销毁根场景（`scene_store` 重置 / `scene_owned` 释放），
  保证组件动态成员正常析构。
- 文档不变量：`docs/33-RenderTexture相机离屏纹理.md` §10。

（仓库内路径：`modules/runtime/src/runtime.cpp` 的 `reload_scene` / `load_scene_into` / `teardown`。）

## 回归与证据

- 回归：`render_texture_e2e` 新增“热重载两次 + 退出码 0 + `vram_bytes` 不变”段
  （`tests/render_texture_e2e.sh:130`）。
- ASan：修复前 `Engine::reload_scene` bad-free；修复后连续 5 次 `Reload.scene` + step 全程无报错、exit 0。
- VRAM：两次 reload 前后 `vram_bytes=7598584` 完全一致；`resource_bytes` 回到基线。
- 关联回归：`scene_serialization_e2e`、`scene_instance_e2e`、`td_e2e` 均通过。

## 相关

- [33 · RenderTexture](../33-RenderTexture相机离屏纹理.md)
- [002](002-node-on-hot-reload-leak.md)（同样在热重载路径，但属于脚本侧）
