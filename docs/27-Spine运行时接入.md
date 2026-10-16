# 27 · Spine 2D 运行时接入（已完成基础闭环）

> 目标：集成官方 **spine-runtime**（`spine-cpp`），为后续在 `samples/` 下播放官方样例动画打底。
> 状态：**模块已参与默认构建、单测通过**；绘制数据导出打通；样例场景待做（见文末）。
> 单一真相：`modules/core/data/schema/world/anim.yaml` 的 `module_abi`（设计来源 `docs/assets/06 §2`）。

## 版本选择（重要，含取舍理由）

用 **4.2 分支**，并**打开 C++ RTTI**。

- **为什么不用 4.3**：4.3 把附件内部结构收成 friend-only —— `RegionAttachment` 不再有
  `getRegion()/getUVs()`、`computeWorldVertices` 要外部传 vertexOffsets；姿势数值全部搬到
  `*Pose` 类；约束收进统一 `getConstraints()`（`getIkConstraints()` 没了）；RTTI 只有
  `getClassName()`。这些与 `docs/assets/06 §2` 描述的接法不符，胶水会非常多。
- **4.2 的公开面与设计文档一致**：`RegionAttachment::getRegion()/getUVs()`、
  `computeWorldVertices(slot, verts, offset, stride)`、`Skeleton::setToSetupPose()`、
  `getData()` 返回指针、`getIkConstraints()`、`IkConstraint::setMix()`、`Atlas(内存)` 全都在。
- **打开 RTTI**：附件类型判别用 `dynamic_cast`（spine 自研 RTTI 只给类名字符串，热路径比较
  字符串不如 dynamic_cast 直接）。异常仍关闭（spine 用 `getError()` 返回错误）。
  标志设在 `third_party/spine-cpp/CMakeLists.txt`（target 级，不污染其它目标）。

## 落地内容

| 项 | 位置 | 说明 |
|---|---|---|
| 官方运行时 vendored | `third_party/spine-cpp/` | 4.2 分支、160 文件、0.84 MiB，随附上游 LICENSE |
| 构建封装 | `third_party/spine-cpp/CMakeLists.txt` | target 级 C++11 + `-fno-exceptions`（**不**关 RTTI）+ 静默第三方告警 |
| 宿主扩展点 | `modules/anim/spine2d/src/spine_extension.cpp` | spine 要求宿主实现 `spine::getDefaultExtension()` |
| C ABI 声明 | `schema/world/anim.yaml` → `generated/abi/spine2d.h` | 骨架数据/实例/图集页/绘制批次/绘制数据 + 40 余函数 |
| 模块实现 | `modules/anim/spine2d/src/spine2d.cpp` | 加载（内存）、实例、动画/皮肤、IK/变换约束、**绘制数据导出** |
| 单测 | `modules/anim/spine2d/tests/spine2d_test.cpp` | 用官方 spineboy/dragon 素材做真实解析与求值断言 |
| 官方素材 | `tests/data/spine/` | spineboy-pro（1 页图集）、dragon-ess（**6 页**，测多页批处理） |

**许可**：Spine Runtimes License 允许集成，但再分发必须随附其许可与版权声明，且使用者需自备
Spine Editor 许可。`third_party/spine-cpp/LICENSE` 与 `tests/data/spine/LICENSE-spine-runtimes.txt`
**不要删**。

## 分层取舍

本模块**只产出绘制数据**（顶点/UV/颜色/索引 + 页号 + 批次），**不碰 GPU、不解码纹理**：

- 骨架与图集从**内存**进（调用方用 VFS 读）→ 数据可来自本地/远程/内存（docs/04）；
- 图集页纹理经 `mq_spine_texture_cb` 回调交给**引擎侧**读文件 + 解码 + 上传，再回填句柄；
- 因此 `spine2d` 不依赖 `gfx` / `image`，跨模块只有 C ABI（docs/02）。

## 接入时踩到的坑（都已在代码注释里写清成因）

1. **`page->texture` 是"渲染器的贴图对象"，不是页对象**。spine 4.2 `Atlas.cpp:290` 会做
   `region->rendererObject = page->texture;` —— 所以往它里面塞引擎纹理句柄**整数**是对的，
   但**不能**在渲染侧 `static_cast<AtlasRegion*>(region->rendererObject)` 再解引用
   （实测 ASan `access-violation` on `0x41`：那是句柄 0x41 当地址用）。本模块把句柄当不透明
   贴图对象，页号另用 `AtlasPage::index`，并维护"句柄 → 页号"表供批次划分。
2. **`AtlasAttachmentLoader` 必须活到骨架数据销毁**。附件在解析期留下它的指针；用栈上局部
   对象会导致绘制期读到已释放内存（ASan 报 `freed` 填充 `0xf2f2...`）。已改堆分配并由
   `mq_spine_data` 持有，销毁顺序：骨架数据 → attachmentLoader → atlas → loader。
3. **`Vector` 扩容后指针失效**。`worldVerts.setSize()` 可能重分配，必须先扩容再取 `buffer()`
   （ASan 抓到过越界读）。
4. **`set_time()` 不会重算姿势**，改完必须 `step(0)` 才按新时间求值。
5. **spine 自己的 JSON 解析器对垃圾 atlas 文本会断言中止**（`Json.cpp:124`），不返回错误。
   所以引擎侧必须在把数据交给 spine **之前**校验；上层若要防御远程脏数据，应先做格式/长度
   校验（或沙箱子进程解析）。单测因此只覆盖"我们能控的"边界（空指针/零长/空图集）。

## 测试与验证

```bash
ctest -R spine2d        # 用官方素材：动画/皮肤清单、播放步进、时间语义、多页图集、批处理覆盖、坏输入不崩
```

实测（本机）：`spine2d_test: all checks passed`。断言的事实包括：

- spineboy 动画清单与 4.2 导出物一致（`idle` 下标 = 3，实测得到并据此修正了断言）；
- 图集页数/页名/尺寸正确，纹理回调被逐页调用（spineboy 1 次、dragon 6 次）；
- 默认姿势与播放后的绘制数据非空；**批次完整覆盖索引**、`first_index` 递增不重叠、
  索引全在顶点范围内、UV 全在 [0,1]；
- 时间语义：`set_time` 后 `time()` 精确等于设定值；换时间点包围盒确实变化（证明姿势随时间变）；
  顶点数不随时间变化（拓扑稳定）；
- 缩放 2 倍 → 包围盒明显变大；
- 坏输入返回错误码/false 而不崩。

注：曾经写过的"往返同一时刻逐字节一致"断言**已删除** —— 它要求 `AnimationState::apply`
在 dt==0 时幂等，而 4.2 实测不满足。**断言比库的保证更强**属于测试写过头，不是封装 bug。

## 已知限制

- **裁剪附件（mask）不产生几何**：`ClippingAttachment` 被跳过，所以带遮罩的角色不会被裁切。
  不静默假装支持（代码里有注释，这里明文记录）。
- 绘制数据是 CPU 侧顶点/索引，**尚未接入 runtime 的渲染管线**（下一步）。

## 下一步

1. **接 runtime**：把 `SpineSkeleton` 组件（已在 `anim.yaml` 声明）在装配期实例化、每帧 step、
   提取绘制数据；纹理页经 `image` 解码 + `gfx` 上传。
2. **`samples/spine/`**：照 `samples/flappy` 的解耦模式（薄宿主 + 资源）放几个官方样例场景：
   spineboy（`idle/walk/run/jump/aim/death` 等动画切换）、dragon（多页图集、大量网格附件）、
   一个 IK 跟随（上游 `ik-following` 的用法）、一个换皮肤。
3. 逐场景 `--shot` 截图核对 + `ctest -R spine` 回归。
