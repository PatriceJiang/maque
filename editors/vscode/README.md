# Maque .scn 预览（本地 VSCode 扩展）

把 `.scn` 场景**渲染成图片**并在编辑器旁预览 —— 用的就是引擎自己的截图能力：

```
maque_player --shot <out.png> --shot-frame N <scene.scn>      # 引擎侧能力（AGENTS.md）
```

## 用法

1. 右键资源管理器里的 `.scn`（或打开 `.scn` 后点编辑器标题栏图标 / 命令面板 `Maque: 预览场景`）
2. 渲染在侧边打开：图片 + 场景路径 + 帧号 + 尺寸；`刷新` 重渲、`打开 .scn` 回到 JSON
3. 首次使用若找不到播放器，会提示你指定：命令 `Maque: 指定 maque_player 路径`

## 设置（Settings → Maque）

| 设置 | 默认 | 说明 |
|------|------|------|
| `maque.playerPath` | 空 | 播放器路径；留空则找 `<工作区>/build/samples/player/maque_player` → `PATH` |
| `maque.shotFrame` | `1` | 渲染到第几帧再截图（脚本照常运行：物理/动画/文本会推进到该帧） |
| `maque.shotArgs` | `[]` | 追加给播放器的参数 |
| `maque.env` | `{}` | 额外环境变量，例如 `{"MAQUE_WINDOW":"640x480","MAQUE_STORAGE":"mem://"}` |

另外本扩展默认把 `*.scn` 关联到 JSON（有语法高亮/校验）：`files.associations`。

## 安装（本地，不使用 marketplace）

```bash
editors/vscode/install.sh          # 软链到 VSCode 扩展目录（远端 server 优先）
# 然后：命令面板 → Developer: Reload Window
```

也可以直接在 VSCode 里开发调试：打开 `editors/vscode/` → F5（`.vscode/launch.json` 已配好 Extension Host）。

## 依赖与排障

- 需要**已构建**的 `maque_player`（`cmake --build build`）与 `third_party/wgpu-native/lib/libwgpu_native.so`（二进制不入库）。
- 渲染走**离屏**（插件自动设 `MAQUE_PRESENT=off`，无需窗口/显示）；失败时看输出面板 `Maque`。
- 画面尺寸 = 渲染目标尺寸（设计分辨率 × 视图数 × 超采样 `MAQUE_SS`）；需要更大图就在 `maque.env` 里给 `MAQUE_SS`。
- 场景里的脚本会运行（第 1 帧就截图时只跑了 `on_start`）；要看到后续状态就调大 `maque.shotFrame`。
- 不想开 VSCode 也能得到同样的图：
  ```bash
  MAQUE_PRESENT=off ./build/samples/player/maque_player --shot /tmp/shot.png --shot-frame 60 scenes/ball.scn
  ```
