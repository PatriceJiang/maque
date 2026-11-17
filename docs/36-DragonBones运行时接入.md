# 36 · DragonBones 2D 运行时接入

> 目标：让 maque 能直接播放 **DragonBones 5.x 导出资源**（`*_ske.json` + `*_tex.json` + `*_tex.png`），
> 与 Spine（[docs/27](27-Spine运行时接入.md)）并列成为 2D 骨骼动画的另一条运行时。
> 状态：**P0 已完成** —— 模块 + C ABI + `anim.DragonBonesSkeleton` 组件 + `dragonbones_*` 脚本 API +
> 示例工程 + 单测/e2e 全绿。能力边界见 §7（如实记录）。

## 0. 为什么是"自研 runtime"而不是 vendor 官方运行时

Spine 走的是 vendored `spine-cpp`（引擎无关、活跃维护、4.2 公开面干净）。DragonBones 不一样：

| 候选 | 结论 |
|---|---|
| 官方 `DragonBonesCPP`（C++） | **已停更（2018）、C++03 风格，且与 cocos2d-x 绑定**（`CCFactory`/`CCSlot` 等引擎胶水）。核心库本身能独立编译，但没有"引擎无关的可复用层"，移植成本高于按格式重写，且长期维护风险大。 |
| 官方 `DragonBonesJS`（TS） | 权威参考实现（本实现逐段对照它），但跑 JS 需要 embedder/VM，发热且与引擎 C ABI/确定性模型不匹配。 |
| 官方二进制 `.dbbin` | 格式无公开规范（只有解析器逆向），P0 不做；遇到 `DBDT` magic 会**明确拒绝**而不是瞎解析。 |

于是：**按官方 JSON 规范（`DragonBones/Tools/doc/dragonbones_json_format_5.5.md`）自研**，
语义逐条对照 DragonBonesJS（`ObjectDataParser` / `BaseTimelineState` / `Bone` / `Slot` / `Transform`）
与 DragonBonesCPP（`Bone.cpp` / `Slot.cpp` / `SFMLSlot.cpp`），**并用官方 C++ 运行时逐骨骼数值对拍**
（见 §6.1）。

## 1. 分层（与 Spine 完全同构）

```
*_ske.json + *_tex.json ──► maque::dragonbones（本模块：解析 + 求值 + 绘制数据）
                                   │  不碰 GPU、不解码纹理（docs/02 模块分层）
                                   ▼
                       C ABI（generated/abi/dragonbones.h，schema 单一真相）
                                   ▼
runtime + maque::gfx ──► 图集页纹理（VFS 读 → image 解码 → gfx 上传，回调回填句柄/尺寸）
                      └─► 每帧绘制数据 → mq_renderer_mesh（网格图元，与 Spine/ImGui 同一条通道）
```

| 项 | 位置 |
|---|---|
| schema（单一真相） | `modules/core/data/schema/world/dragonbones.yaml`（`module_abi`）+ `world/anim.yaml`（`DragonBonesSkeleton` 组件、`DbAnimState`） |
| C ABI | `modules/core/data/generated/abi/dragonbones.h`（53 个函数） |
| 模块实现 | `modules/anim/dragonbones/src/dragonbones.cpp`（解析/采样/姿势/导出，约 1.6k 行） |
| 模块单测 | `modules/anim/dragonbones/tests/dragonbones_test.cpp`（真实素材 + 内联合成素材） |
| runtime 接线 | `modules/runtime/src/anim/dragonbones.cpp`（setup/step/extract）+ `src/api/dragonbones.cpp`（32 个脚本 API） |
| 示例 | `samples/dragonbones/`（`mecha.scn` + `dragonbones.js` + `art/`） |
| 测试 | `ctest -R dragonbones`（单测 + smoke/switch/determinism 三个 e2e） |

## 2. 数据流（装配期 vs 帧内）

- **装配期**（`Engine::dragonbones_setup`，做 IO，不在模拟帧内）：
  VFS 读骨架/图集 → `mq_db_data_create`（回调里按页名读 PNG → `load_texture_raw` 上传 → 回填
  `句柄 = gfx 纹理 id + 1` 与**像素尺寸**）→ 选 armature（组件 `armature` 名）→ 建实例 →
  记动画/皮肤/页名清单。
- **帧内**（`Engine::dragonbones_step`，固定 `dt`，单线程、无异步 → 确定性）：
  组件字段（`animation`/`play_mode`/`speed`/`flip`/`color`/`z_index`）→ `mq_db_instance_step` →
  求值姿势 → `mq_db_instance_draw` 导出 CPU 绘制数据 → 顶点从 armature 空间映射到渲染目标像素
  （**不翻 y**，见 §3）→ transient 回写 `state`。
- **提取期**（`Engine::dragonbones_extract`）：每个批次（同图集页 + 同混合模式）一条
  `mq_renderer_mesh`，与 Spine/ImGui 网格**同一条排序/绘制通道**（layer/z 统一排序）。

实例是**派生数据**：不进快照/校验和（docs/10）。唯一真相 = 组件字段 + 资源文件；
切场景/热重载/退出统一 `dragonbones_release()`（幂等）。

## 3. 坐标系：DragonBones 是 **y 向下**，与 Spine 相反（**关键**）

| | Spine | DragonBones |
|---|---|---|
| 骨架空间 y | **向上**（脚在 0，头在 +y） | **向下**（头在 -y，脚在 0 附近） |
| 映射到渲染目标 | `screen_y = H - world_y - local_y`（**翻一次**） | `screen_y = origin_y + local_y`（**不翻**） |

> 踩过的坑：照着 spine 的写法翻 y → 整具骨架上下颠倒。DragonBones 是 flash 系坐标系，
> 导出顶点已经是"屏幕向下为正"。本模块 `mq_db_draw` 的文档与 runtime 注释都写死了这条约定。

节点变换（`node_set_angle/scale`、父链累积）仍由 runtime 做：先按累计角度/缩放旋转，
再平移到实体世界位置（`screen_y = H - world_y`，与引擎 2D 图元一致）。

## 4. 已验证的语义（每条都有官方源码依据 + 测试）

| 语义 | 依据 | 我们的落地 |
|---|---|---|
| `skX`/`skY`（度）→ `rotation = skY`、`skew = skX - skY`，两者 normalize 到 (-π, π] | `ObjectDataParser::_parseTransform` | `jtransform()` |
| 骨骼动画帧是**相对 setup 的增量**：x/y 相加、rot/skew 相加、scale 相乘 | `Bone::_updateGlobalTransformMatrix`（Additive） | `evaluate()` |
| 矩阵 `{a,b,c,d,tx,ty}`，行向量；`global = local * parent` | `Transform::toMatrix` / `Matrix::concat` | `to_matrix()` / `concat()` |
| 缺 `tweenEasing`（或 null）= **不补间**；0 = 线性；<0 = QuadIn；0..1 = QuadOut；>1 = QuadInOut | `ObjectDataParser::_parseTweenFrame` | `jtween()` / `ease()` |
| `curve` = 三次贝塞尔（控制点 `[x1,y1,x2,y2]`） | 同上（`_samplingEasingCurve`） | `bezier_y()`（二分求解 x→t） |
| 最后一帧的补间**回绕到第 0 帧**；rotate 回绕时差值按最短路径规范化 | `BoneRotateTimelineState::_onArriveAtFrame` | `sample_frames(..., wrapped)` |
| 导出器常在末尾放 `duration: 0` 的帧携带闭合值 | 真实素材（`mecha_*`） | `find_frame()` 跳过零时长帧、只把它们当"下一帧" |
| `clockwise` 把跨圈旋转摊平成连续值 | `_parseBoneRotateFrame` | `parse_rot_frames()` |
| 图像附件只画**裁剪后的区域**（`region.w × region.h`），整块减 pivot；pivot 用**未裁剪帧**尺寸算：`pivotX = pivot.x*frameW + frameX` | `SFMLSlot::_updateFrame` + `Slot::_updateTransform` | `image_quad()` |
| 网格 uv 是**子纹理内**归一化坐标；旋转图集 `u=(x+(1-v)*w)/W, v=(y+u*h)/H` | `PixiSlot::_updateFrame` / `SFMLSlot::_updateFrame` | `mesh_uv()` |
| `slot.color`：`aM/rM/gM/bM` 是乘子（/100）；**颜色 offset（aO/rO/…）P0 不参与** | `_parseColorTransform` | 见 §7 |
| **`inheritScale=false` 仍然继承父骨骼的平移与旋转**，只是不乘父缩放 | `Bone.cpp` 的 `else` 分支 | `evaluate()`（★ 见 §5） |
| 槽的显示顺序 = `slot[]` 声明序；`displayFrame.value` 缺省 0 | `_parseSlotDisplayFrame` | `SlotState.display` |

## 5. 三个真 bug（都是"日志正常但画面错"，都带回归）

### 5.1 `inheritScale=false` 被当成"什么都不继承" → 整具骨架旋转丢失

第一版按"只继承父平移"近似实现（以为 `inheritScale=false` 就是"不继承父变换"）。
实测：所有 mecha 素材**大量骨骼**带 `inheritScale: false`（官方编辑器默认导出），
结果每根骨骼都丢掉了父的 90° 级旋转 → 机甲看起来"躺倒/散架"，但：

- 骨骼**世界位置**看起来"没崩"（平移还在，只是父旋转没作用到位置上）；
- 单测全绿（当时的断言只覆盖顶点数/UV 范围，看不出姿势错）；
- 直到把官方 demo 参考图（站立机甲 + "CORE ELEMENT" logo）与我们的渲染对照才发现。

修法（逐行对照 `Bone.cpp`/`Bone.ts`）：

```
if (!inheritScale) {
    位置  = parentMatrix * 局部位置            // 父旋转/缩放/平移都作用在位置上
    旋转  = 自身旋转 + 父绝对旋转               // ★ 关键：仍然继承旋转
    矩阵  = toMatrix(自身位置, 该绝对旋转, 自身缩放)   // 不乘父缩放
}
```

回归：单测 `test_synthetic_inherit`（root 转 90°、child 在 (100,0)，断言 child 世界位置
`(0,100)` 且**图元包围盒是 20×20 的旋转后矩形** `x∈[-10,10] y∈[90,110]`）。

> 教训：DragonBones 的 `inheritXxx` 三个标志是**独立**的；`inheritScale=false` 只影响缩放。
> 这类"语义近似"必须拿真实素材和官方参考图对，不能只靠自洽的单测。

### 5.2 动画帧的 `rotate`/`skew` 是**角度制**（按弧度读 → 关节反向甩）

DragonBones 的 JSON 里，**setup transform** 的 `skX/skY` 是度，**动画帧**的 `rotate`/`skew` **也是度**
（官方 `JSONDataParser::_parseBoneRotateFrame`：`rotation = ROTATE * DEG_RAD`，skew 同理）。
第一版只对 setup 做了 `deg→rad`，动画帧直接当弧度用 → 每根骨骼的旋转被放大约 **57 倍**（再按 2π 回绕）：

- 静态首帧看着"差不多"（idle 首帧的 delta 很小，0.25° vs 0.25 rad 的差别不显眼）；
- 一播动画就原形毕露：关节朝**反方向**甩、四肢扭曲、机甲像在抽搐（用户报的就是这个"旋转反了"）；
- 单测当时只断言顶点数/UV/包围盒**范围**，抓不到（又是"自洽的测试没有鉴别力"）。

修法：`parse_rot_frames()` 里 `rotate/skew * DEG_RAD`（`clockwise` 的跨圈摊平在弧度域做，
与官方顺序一致：先换算再累加）。回归：单测 `test_synthetic_deg_rotate`
（`rotateFrame.rotate=90` → 子骨骼世界位置必须是 `(0,100)`，按弧度会得到 `(-43,90)` 之类）。

> 这条同时修正了"运动方向反了"的表象：并不是符号取反，而是量纲错误导致角度落到完全不同的象限。

### 5.3 图像裁剪的 pivot 算错（多减了一次 `frameX`）

第一版按"可见矩形 = 未裁剪帧里的裁剪区位置"写：`left = -pivotX - frameX`。
官方 `SFMLSlot` 的画法是 **quad 只覆盖裁剪区**（`0..regionW`），整块再减 pivot，
即 `left = -pivotX`，裁剪量只进 pivot。差异 = `-frameX`（通常几个像素），
在 UI 级素材上不明显、在拼装机甲上会让零件错位。已修正，单测 `test_synthetic_trim`
锁定 `x∈[-12,12] y∈[-10,10]`（`frameX=-4, frameY=-2, 32×24 帧 / 24×20 区域`）。

## 6. 验证

### 6.1 与官方 C++ 运行时**逐骨骼数值对拍**（最强证据）

本地用官方 `DragonBonesCPP`（MIT）的核心库编译了一个探针（不 vendored、不入库），
对同一份 `_ske.json` 解析 + `advanceTime(0)` 后打印每根骨骼的
`globalTransformMatrix`（位置 + 旋转 + 缩放）。本模块的矩阵与它**逐骨骼一致**，例如
`mecha_1002_101d` setup 姿势：

| 骨骼 | 官方 `rot/pos` | 本模块 |
|---|---|---|
| root | 89.68° / (-1.35, -233.40) | 同 |
| pelvis | -75.85° / (-1.40, -233.41) | 同 |
| chest | -85.71° / (4.28, -256.05) | 同 |
| head | -91.00° / (-1.38, -320.12) | 同 |
| thigh_l / thigh_r | 72.76° / 123.94° | 同 |

> 这条对拍在修 `inheritScale` 之前就做过——**它只证明了"按我的语义实现的矩阵与官方同语义"**，
> 并没有覆盖"我是否实现了官方语义"。真正发现 5.1 的是**参考图对照**。
> 结论：数值对拍能证明实现一致性，**不能替代"渲染结果与官方观感对照"**。

**动画阶段的对拍**（修完 5.1/5.2 后补）：同一探针在 `play("walk")` 后按 1/60 步进到 0.5s，
逐骨骼打印世界矩阵；本模块（用同一份素材、同一时刻）结果**逐骨骼逐字段一致**：

| 骨骼 | 官方（walk @0.5s） | 本模块 |
|---|---|---|
| root | 90.263° / (-9.625, -95.771) | 同 |
| pelvis | -79.474° / (-10.066, -98.685) | 同 |
| chest | -87.879° / (-3.849, -122.647) | 同 |
| thigh_l / thigh_r | 51.914° / 34.802° | 同 |
| calf_l / calf_r | 103.482° / 128.827° | 同 |
| hand_l | -154.149° / (8.387, -112.268) | 同 |

（这次对拍正是发现 5.2 的方式：setup 全对、一动就全错 → 必然是"动画帧解析"而不是"矩阵/坐标"问题。）

### 6.2 测试

```bash
ctest --test-dir build-win -R dragonbones      # 4/4
#  dragonbones        模块单测（真实素材 + 合成素材：inheritScale / 裁剪 / 皮肤 / 网格 / 错误路径）
#  dragonbones_smoke  装配（页纹理句柄/尺寸）+ 绘制规模（56 顶点 / 84 索引）+ 无报错
#  dragonbones_switch MAQUE_SIM 注入 ↑/↓/空格 → 切换序列 + 引擎读回一致 + 两张截图不同 + 像素断言
#  dragonbones_determinism 同场景跑两次 → 截图逐字节一致 + world_trail 一致
```

像素断言框选刻意**排除 HUD/提示文字**：否则"角色没画出来"也会因为文字像素而误判通过
（与 [docs/27](27-Spine运行时接入.md) 的教训一致）。

### 6.3 参考图对照（人工，但有效）

`mecha_1004d_show` 的图集里带 **"CORE ELEMENT" 文字 logo**：它是骨架里的一个普通 image slot。
文字**正读**是"y 没翻、旋转没错"的强判据（竖直翻转后文字会镜像）。
修复 §5.1 后，同一素材的渲染与官方 demo 展示图一致（站立机甲 + 双剑 + logo）。

## 7. 已支持 / 未支持（如实记录）

### ✅ P0 已支持

- `*_ske.json`（5.5 格式：`armature`/`bone`/`slot`/`skin`/`animation`）
- `*_tex.json` + 单页图集（trim `frameX/Y/W/H`；旋转图集按官方公式实现，**未用真实 rotated 素材验证**）
- 显示类型：`image`、**非蒙皮** `mesh`（含 FFD 顶点变形时间轴）
- 动画时间轴：bone translate/rotate/scale（含 `clockwise`、curve、四种补间）、slot display/color、ffd
- 播放：`play/stop/set_time/step`、循环（`playTimes=0`）与非循环完成、`play_times` 计数、逐音符速度
- 皮肤：按名切换（`set_skin`），空皮肤/缺槽安全
- 槽混合模式 → `mq_renderer_mesh.blend`（normal/additive；multiply/screen 与 spine 同样受后端因子限制）
- 骨骼查询（名字→下标、世界位置）、槽显示查询/手动切换（换装/换武器）
- 实例 tint（与槽色相乘）、水平/竖直翻转、节点角度/缩放/透明度（父链累积）
- 诊断计数：`mesh/weighted/nested/missing_texture`（装配期打印，不静默）

### ❌ 未支持（P1+）

| 项 | 说明 |
|---|---|
| **带权重网格**（`weights` + `bonePose`/`slotPose`） | 跳过该附件并计数（`weighted_count`）。`you_xin/body` 这类"纸娃娃"角色依赖它。 |
| **嵌套子骨架**（display `type: "armature"`） | 跳过并计数（`nested_count`）。`mecha_1004d` 的武器槽属于这类。 |
| **颜色 offset**（`aO/rO/gO/bO`） | P0 只乘 `aM/rM/gM/bM`；offset 需要着色器加法项（当前 mesh shader 只有乘）。 |
| IK 约束 / `zOrder` 时间轴 / 动画 action 事件 | 解析层直接忽略（不参与求值）。 |
| 二进制 `.dbbin` | 明确拒绝（`DBDT` magic → `mq_db_data_create` 返回 NULL）。 |
| 动画混合/过渡（fadeIn、多动画叠加） | 只有"硬切"，无 crossfade。 |
| 图集多页 / 多个 `*_tex.json` | P0 一个骨架只接一个图集页。 |
| 网格 `share` / `inheritDeform` | 未接（共享网格按普通附件名查找，找不到就计数）。 |

## 8. 脚本 API（33 个，`schema/runtime/host.yaml` 的 `api_list`）

按实体 id 寻址，语义与 `spine_*` 一致（越界/无组件 → `0`/`""`/`-1`，不崩）：

```
动画   dragonbones_animation_count/name/index · play · stop · current_animation
       time · set_time · duration · play_times · complete
骨架   dragonbones_armature
皮肤   dragonbones_skin_count/name · set_skin · current_skin
骨骼   dragonbones_bone_count/name/find · bone_x · bone_y
槽     dragonbones_slot_count/name/index · slot_display · set_slot_display
图集   dragonbones_pages · page_texture · page_width · page_height
诊断   dragonbones_vertex_count · batch_count · bounds(which)
```

`dragonbones_play` 会**同步写回组件字段**（组件是唯一真相），否则下一帧 `step` 会按旧值切回去
（与 spine 同一模式）。

## 9. 示例（`samples/dragonbones/`）

```bash
./build/samples/player/maque_player samples/dragonbones/mecha.scn
```

- 素材：官方 demo `mecha_1004d`（10 个动画：idle/walk/death/hit/attack/skill×4/victory；
  一台绿色机甲，随动画站立、走动、挥剑）。**素材许可**见 `art/LICENSE-dragonbones-art.txt`
  （CC BY-NC-SA 4.0，仅示例/测试用）。
- 交互：`↑/↓` 切动画、`空格` 暂停；HUD 显示当前动画；自适应缩放走节点 scale
  （`MAQUE_DRAGONBONES_FIT` 覆盖），`MAQUE_DRAGONBONES_ANGLE` 可加节点角。
- 诊断环境变量：`MAQUE_TRACE_DRAGONBONES=1` 每 20 帧打印
  顶点/索引/批次/槽数/包围盒/UV 跨度/屏幕包围盒。

## 10. 实现边界与后续

1. 带权重网格（LBS + `bonePose` 逆矩阵）——`body`/换装类素材的关键能力。
2. 嵌套子骨架（父骨骼矩阵 + 子 armature 自己的动画）。
3. 颜色 offset（mesh shader 加 `col_offset` 或第二个顶点色通道）。
4. 多页图集 + 多 `*_tex.json` 合并；rotated 图集用真实素材做像素对照。
5. 动画过渡/混合（`fadeInTime`、多轨）。
6. `.dbbin` 二进制格式（需按 `BinaryDataParser` 逆向，工作量大、收益一般）。
7. IK / zOrder / 事件（action frame）：先补事件（脚本回调），再做 IK。

## 11. 与相关文档的关系

- [docs/27](27-Spine运行时接入.md)：另一条 2D 骨骼运行时；**分层/测试/踩坑范式同源**，但坐标轴与运行时不同。
- [docs/15](15-动画物理粒子.md)：动画/物理/粒子总览（2D 骨骼属于动画子系统）。
- [docs/02](02-模块系统-ABI与多实例.md)：为什么跨模块只暴露 C ABI、分配者释放、句柄约定。
- [docs/10](10-确定性快照与回放.md)：为什么实例是派生数据、不进快照/校验和。
- `docs/components/09-skeletal-2d.md` §4：组件的"同构但不合并"约定（`DragonBonesSkeleton` vs `SpineSkeleton`）。
