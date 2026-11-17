# samples/dragonbones —— DragonBones 2D 演示

用 `maque_player` 直接打开 `mecha.scn`：

```bash
./build/samples/player/maque_player samples/dragonbones/mecha.scn
# 无窗口截图（CI/预览）
MAQUE_HEADLESS=1 MAQUE_PRESENT=off ./build/samples/player/maque_player \
  --shot out.png --shot-frame 30 samples/dragonbones/mecha.scn
```

## 内容

| 文件 | 说明 |
|---|---|
| `mecha.scn` | 480×320 场景：背景 + 地面 + 一台 `anim.DragonBonesSkeleton` 机甲 + HUD/提示 |
| `dragonbones.js` | 玩法全在脚本里：列动画、`↑/↓` 切动画、`空格` 暂停、自适应缩放、HUD |
| `art/mecha_1004d_*` | 官方 demo 素材（10 个动画：idle/walk/death/hit/attack_01/skill_01/03/04/05/victory） |

**素材许可**：运行时/工具是 MIT，但**这套演示美术是 CC BY-NC-SA 4.0**（仅示例/测试用，
商用请替换）。详见 [`art/LICENSE-dragonbones-art.txt`](art/LICENSE-dragonbones-art.txt)。

## 这个示例验证了什么

- 装配：`*_ske.json` + `*_tex.json` + `*_tex.png` → VFS 读 → 解码 → gfx 上传 → 回填页纹理句柄/尺寸。
- 求值：骨骼世界矩阵（含 `inheritScale=false` 语义）、槽显示/颜色、动画循环。
- 绘制：14 个可见 image 槽 → 56 顶点 / 84 索引 / 1 个批次（2 个槽 setup 隐藏、2 个槽是嵌套子骨架，
  P0 未支持 → 装配期会打印计数）。
- 脚本 API：`dragonbones_animation_*` / `dragonbones_play` / `dragonbones_bounds` / `dragonbones_bone_*` 等。

## 诊断

```bash
# 每 20 帧打印 顶点/索引/批次/槽数/包围盒/UV 跨度/屏幕包围盒/播放头
MAQUE_TRACE_DRAGONBONES=1 ./build/samples/player/maque_player samples/dragonbones/mecha.scn

# 指定起始动画 / 定格时间 / 缩放 / 节点角（截图确定性用）
MAQUE_DRAGONBONES_ANIM=attack_01 MAQUE_DRAGONBONES_TIME=0.4 \
MAQUE_DRAGONBONES_FIT=0.8 MAQUE_DRAGONBONES_ANGLE=0 \
./build/samples/player/maque_player --shot attack.png --shot-frame 20 samples/dragonbones/mecha.scn
```

测试：`ctest --test-dir build -R dragonbones`（模块单测 + smoke/switch/determinism 三个 e2e）。
设计/能力边界/踩坑见 [docs/36](../../docs/36-DragonBones运行时接入.md)。
