# 31 · Khronos glTF 模型批量迁移经验（glTF-Sample-Assets → maque 场景）

> **目标**：把 [KhronosGroup/glTF-Sample-Assets](https://github.com/KhronosGroup/glTF-Sample-Assets)
> `Models/` 下的 **149 个模型**逐个迁移成 maque 场景（`scenes/khronos/<Model>/`），
> 每个模型一份**独立笔记**、一张本引擎渲染预览、一份与官方参考图的定量对比，
> 并汇总成机器可读清单 `scenes/khronos/manifest.json`。
>
> **本文定位**：迁移的**权威操作手册 + 踩坑总账 + 排查索引**。上游设计与审计背景见
> [`plans/18-pbr-3d.md`](../plans/18-pbr-3d.md)、[`plans/19-threejs-materials.md`](../plans/19-threejs-materials.md)、
> [`plans/20-pbr-audit.md`](../plans/20-pbr-audit.md)（审计原始证据在 `build-win/audit/`，属构建产物、不入库）。
> 自动生成的逐模型笔记在 `scenes/khronos/<Model>/README.md`；总索引是 `scenes/khronos/README.md`（脚本生成，勿手改）。

## 0. 结论（TL;DR）

| 事项 | 结论 |
|---|---|
| 取资产 | 本机 shell 的 schannel/wolfSSL 出网被阻断，**但 Node/Python 的 OpenSSL TLS 可用** → 用 Node 下载 codeload zip（约 1.09 GiB）后 `tar -xf` 解包到 `.deps/gltf-assets/`（`.deps/` 完整不入库） |
| 导入 | `python tools/gltf_import.py <gltf> --out scenes/khronos/<Model> --name <Model> --scene --compact`，离线、确定性；PNG/JPEG 贴图原样搬运，不做编解码 |
| 批量 | `python tools/migrate_gltf_models.py`：选主变体 → 导入 → headless 截图 → 定量对比 → 写逐模型 README → 重写总索引 + `manifest.json`；单模型失败不中断整批 |
| 本批结果 | **148/148 成功**（149 − 手调 ToyCar）；0 blocked、0 failed；整批 472.3s（≈7.9 min，平均 3.2s/模型）；派生资产 ≈983 MB（不入库）；差异分布与 Top 见 §10 |
| 入库策略 | 入库：`.scn`、`preview.jpg`（800×450）、`README.md`、`compare.json`、`manifest.json`；**不入库**：`assets/`、`textures/`、`preview.png`（派生资产，从 `.deps` 一条命令重生成；入库体积 ≈18.4 MB，约为派生资产的 1.9%） |
| 对比 | 与官方 `screenshot/` **不是像素级 pass/fail**：官方图多为离线渲染（HDRI studio/软阴影/不同机位/透明背景），maque 有 equirect/cubemap envmap IBL（`.hdr` RGBE / **EXR** / PNG/JPEG，**GGX 粗糙度预过滤 mip + 漫反射 irradiance**、可见天空背景），但无真实 transmission、软阴影。自动指标只用于**筛查**，最终以人工看 preview + 参考图并记录差异归因为准 |
| 当前主要差距 | 真实体积 transmission/volume（现为屏幕空间折射近似）、KTX2/meshopt、`KHR_animation_pointer`（**动画/骨骼/morph 已由第 29 轮支持**，见 §5.4）—— 详见 §5 与 §11 |
| 确定性 | 同一输入两次导入**逐字节一致**；同机同构建、同场景同帧两次 `--shot` 的 PNG **SHA256 相同**（已实测）；流水线固定 `--compact`（与默认输出有 6 位小数舍入差异） |
| 一键复现 | 见 §3；干净 clone 后恢复派生资产的唯一命令：`python tools/migrate_gltf_models.py --force`（会重写 `.scn`，手调模型须排除） |
| 逐模型对比报告 | `reports/khronos-comparison/index.html`（149 个模型官方参考图 **large 优先** vs 迁移预览，左右对比 + 指标 + 差距 + 人工复核；`python tools/make_migration_report.py` 可重生成） |

---

## 1. 范围、前置条件与约定

### 1.1 范围

- **只做**：`scenes/khronos/` 下的迁移产物 + 本文档 + 索引；不改引擎源码、不改 `tools/`、不改其他场景。
- **迁移形态**：glTF 网格 + PBR 材质 → 一个 maque `.scn`（自动生成相机/方向光），按**本引擎**渲染一张
  1280×720 预览。**第 29 轮起**：有 `skins`/`animations`/morph targets 的模型输出**层级场景**并自动播放
  首个动画片段（见 `plans/22`）；其余模型仍是平级实体 + 世界烘焙。
  **不迁移**：`KHR_animation_pointer`（材质/贴图/可见性指针通道）；glTF 自带相机/灯光已支持导入
  （`--use-gltf-camera` 按模型择优使用；灯光见 §5.4）。
- **不承诺**：与官方参考图相似（见 §7 的对比策略）；那属于"新渲染特性排期"，不是迁移缺陷。

### 1.2 前置条件

| 依赖 | 要求/说明 |
|---|---|
| 仓库 | `E:\Github\maque`，所有命令在**仓库根目录**执行 |
| 构建产物 | `build-win/samples/player/maque_player.exe` 已存在（`tools/win/build.cmd` 或 AGENTS.md 的 CMake 命令构建；本迁移不负责构建） |
| Python | Windows `python` 3.11（实测 Anaconda `Py311`）；**`Pillow` + `numpy` 必需**（生成 `preview.jpg` 与对比指标；缺 PIL 时该模型状态只到 `imported`、不生成 `compare.json`） |
| Node | `node`（仅用于下载资产；本机 shell 的 TLS 不可用，Node 的 OpenSSL 可用） |
| 解包 | Windows 自带 `tar.exe`（bsdtar）即可解 zip |
| 资产 | `.deps/gltf-assets/glTF-Sample-Assets-main/Models/`（149 个目录，约 1.34 GiB；不入库，见 §2.3） |

自检：

```powershell
python -c "import PIL, numpy; print('ok', PIL.__version__, numpy.__version__)"
Test-Path build-win/samples/player/maque_player.exe
Test-Path .deps/gltf-assets/glTF-Sample-Assets-main/Models
```

> Windows 下请用 `python`（本机 `python3` 可能是 Microsoft Store 占位程序，会弹安装提示并挂住——
> 与 AGENTS.md 里 e2e 脚本的 `maque_python.sh` 是同一个坑）。

---

## 2. 目录与产物约定

### 2.1 目录结构

```
scenes/khronos/
  README.md                     # 总索引（脚本生成：总览/模型列表/blocked 明细/复现命令）——勿手改
  manifest.json                 # 机器可读清单（全部模型的 status/stats/gaps/issues/compare）——勿手改
  ToyCar/                       # 手工调校的样板（不参与自动批量；默认 --exclude）
    ToyCar.scn                  # 手调相机/灯光/env/材质（Glass 用 alpha BLEND 近似 transmission）
    README.md                   # 手写笔记（保留下游说明）
    preview.png                 # 手调后的 1280×720 预览（不入库，仅本地）
  <Model>/
    <Model>.scn                 # 场景（自动生成；可手调，注意 --force 会覆盖，见 §3.5/§8.2）
    assets/<...>.mesh.json      # 导入网格+材质（派生资产，不入库）
    assets/skin/*.skel.json     # 骨架（joint_names + 列主序 inverse_bind；动画/蒙皮模型才有，派生，不入库）
    assets/anim/*.clip.json     # glTF 动画片段（node/path/STEP|LINEAR/times/values；派生，不入库）
    textures/*.png|*.jpg        # 从 glTF 原样搬运的贴图（派生资产，不入库）
    preview.png                 # 1280×720 原图（派生，不入库）
    preview.jpg                 # 800×450 提交用缩略图（PIL，质量 82）
    compare.json                # 与官方 screenshot 的指标（PIL 可用时生成）
    README.md                   # 逐模型独立笔记（脚本生成；"手动笔记"区保留人工补充）
```

### 2.2 哪些入库、哪些不入库、为什么

| 路径 | 入库 | 理由 |
|---|---|---|
| `scenes/khronos/<Model>/<Model>.scn` | ✅ | 场景是迁移的**交付物**；体积小（多则几十 KB） |
| `scenes/khronos/<Model>/preview.jpg` | ✅ | 800×450、质量 82，单张约 10–40 KB；是"本引擎渲染结果"的证据/画廊素材 |
| `scenes/khronos/<Model>/README.md` | ✅ | 逐模型笔记（含许可/命令/差异归因/踩坑）是**交付物** |
| `scenes/khronos/<Model>/compare.json` | ✅ | 指标可审计（几十行 JSON） |
| `scenes/khronos/README.md`、`manifest.json` | ✅ | 总索引 + 机器可读清单 |
| `scenes/khronos/<Model>/assets/` | ❌ | `.mesh.json` 是**文本 JSON、体积大**（compact 约 53 B/顶点；大模型单个几十 MB），可由源资产确定性重生成 |
| `scenes/khronos/<Model>/textures/` | ❌ | 与源贴图体积相当（原样搬运），全部入库等于把 Khronos 资产再拷一份进 git |
| `scenes/khronos/<Model>/preview.png` | ❌ | 1280×720 PNG 单张约 0.1–2 MB；提交用 `preview.jpg` 已足够 |

**量化依据（本机实测，148 个自动迁移模型 + 手调 ToyCar）**：

- 源：Khronos 全部 `Models/*/glTF` 主变体合计 **597.0 MB**（1019 个文件）；`screenshot/` 参考图另有 126.0 MB；
  zip 1,168,300,967 B → 解包后 `Models/` 1,433,732,917 B（约 1.34 GiB）。
- 派生资产（不入库，仅本地）：**约 983 MB / 13,511 个文件**——其中 `assets/` 570 MB、`textures/` 365 MB、
  `preview.png` 29 MB。全入库会让 git 仓膨胀到 GB 级，且这些产物**完全可由 `.deps` 重生成** → 收益为零、代价极大。
- 入库部分（可提交）：`*.scn` 15.5 MB + `preview.jpg` 2.3 MB + `README.md` 0.5 MB + `compare.json` 0.1 MB
  ≈ **18.4 MB**（再加上 `manifest.json` + 总索引 0.4 MB）——**约为派生资产的 1.9%**。
- `preview.png`（29 MB，1280×720 原图）不入库，用 800×450 的 `preview.jpg` 替代（约 2.3 MB）。

`.gitignore` 中的三条规则（已生效，勿删）：

```gitignore
/scenes/khronos/*/assets/
/scenes/khronos/*/textures/
/scenes/khronos/*/preview.png
```

### 2.3 源资产与 `.deps`（不入库）

- `.deps/gltf-sample-assets.zip` —— 下载的 codeload zip（约 1.09 GiB），**不入库**（`/.deps/` 已忽略）。
- `.deps/gltf-assets/glTF-Sample-Assets-main/Models/` —— 解包后的源资产，**不入库**。
- `.deps/download-gltf-assets.js` —— 下载脚本（也随 `.deps/` 被忽略；内容见 §3.1，可随时重建）。
- 为什么放 `.deps/`：与 `tools/win/fetch-deps.ps1` 的第三方依赖同目录约定，统一"本地缓存、仓库外、可重建"。

> 干净 clone 后恢复派生资产的**唯一命令**（源资产已解包到 `.deps/gltf-assets` 时）：
>
> ```powershell
> python -u tools/migrate_gltf_models.py --force
> ```
>
> `--force` 只清每个模型的派生部分（`assets/`、`textures/`、`<Model>.scn`、`preview.png`、`preview.jpg`、`compare.json`），
> **保留 `README.md`**（含"手动笔记"区）；但**手调过的 `.scn` 会被覆盖**——手调模型必须先排除
> （默认已排除 `ToyCar`，手调新模型时用 `--exclude ToyCar,<其他手调模型>`）。


---

## 3. 一键复现（命令全部可复制执行）

### 3.1 首次：下载并解包 Khronos 资产

```powershell
# 1) 建目录
New-Item -ItemType Directory -Force .deps\gltf-assets | Out-Null

# 2) 下载 codeload zip。本机 shell 的 TLS 栈（schannel/wolfSSL）出网会失败
#    （SEC_E_NO_CREDENTIALS / connection reset），Node 的 TLS 走 OpenSSL，可用：
node .deps/download-gltf-assets.js .deps/gltf-sample-assets.zip
#    若 .deps/download-gltf-assets.js 不存在（.deps 被清理过），用等价 Python 下载：
#    python -c "import urllib.request;open('.deps/gltf-sample-assets.zip','wb').write(urllib.request.urlopen('https://codeload.github.com/KhronosGroup/glTF-Sample-Assets/zip/refs/heads/main',timeout=1800).read())"

# 3) 解包（Windows 自带 bsdtar；zip 根目录是 glTF-Sample-Assets-main/）
tar -xf .deps/gltf-sample-assets.zip -C .deps/gltf-assets

# 4) 自检：应为 149 个模型目录
(Get-ChildItem .deps/gltf-assets/glTF-Sample-Assets-main/Models -Directory).Count
```

`download-gltf-assets.js` 内容（30 行，若丢失可重建）：

```js
// 一次性下载 Khronos glTF-Sample-Assets 主分支 zip（Node TLS=OpenSSL；本机 shell 的 schannel 出网被阻断）
const fs = require('fs');
const https = require('https');
const url = 'https://codeload.github.com/KhronosGroup/glTF-Sample-Assets/zip/refs/heads/main';
const out = process.argv[2] || '.deps/gltf-sample-assets.zip';
function get(u, redirects) {
  redirects = redirects || 0;
  https.get(u, { headers: { 'User-Agent': 'maque-migration' } }, (res) => {
    if (res.statusCode >= 300 && res.statusCode < 400 && res.headers.location && redirects < 8) {
      res.resume(); return get(res.headers.location, redirects + 1);
    }
    if (res.statusCode !== 200) { console.error('download: HTTP ' + res.statusCode); process.exit(1); }
    let total = 0;
    const f = fs.createWriteStream(out);
    res.on('data', (c) => { total += c.length; });
    res.pipe(f);
    f.on('finish', () => console.log('download: done bytes=' + total));
    f.on('error', (e) => { console.error('download: ' + e.message); process.exit(1); });
  }).on('error', (e) => { console.error('download: ' + e.message); process.exit(1); });
}
get(url);
```

### 3.2 全量迁移（导入 + 渲染 + 对比 + 笔记 + 索引）

```powershell
# 仓库根目录；先确保 .deps/gltf-assets 已解包
python -u tools/migrate_gltf_models.py --force
# 推荐同时留一份批量日志（便于事后审计"哪个模型 meshes=0 / mesh=-1"）
python -u tools/migrate_gltf_models.py --force 2>&1 | Tee-Object -FilePath build-win/audit/migrate-batch1.log
```

- 默认**排除 `ToyCar`**（`--exclude ToyCar`：它已手工调校机位/灯光/env/材质，不能被自动生成覆盖）；
  默认处理 **148** 个模型（149 − ToyCar）。
- `-u` 必须加：Python stdout 经管道/重定向时是**块缓冲**，不加会让批量日志长时间不刷新（看起来像卡死），见 §8.1。
- 每个模型的**完整渲染日志**另存 `build-win/audit/migrate-logs/<Model>.render.log`（构建产物，不入库），
  批量日志只保留每模型的 3 条 issue 摘要。
- 结束打印：`migrate: N ok, M blocked, K failed (总秒数)`，并给出 `manifest`/`index` 路径。
- 产物：`scenes/khronos/<Model>/` × 148、`scenes/khronos/README.md`、`scenes/khronos/manifest.json`。

### 3.3 单模型重跑

```powershell
# 全流程重跑一个模型（导入+渲染+对比+笔记；--only 会合并旧 manifest 的其它记录）
python -u tools/migrate_gltf_models.py --only DamagedHelmet --force

# 只导入，不渲染/不写笔记
python -u tools/migrate_gltf_models.py --only DamagedHelmet --force --no-render --no-notes

# 只渲染 + 笔记（复用已导入资产）
python -u tools/migrate_gltf_models.py --only DamagedHelmet

# 不重渲染，复用已有 preview.png 只重算对比/重写笔记（改了对比指标/README 模板后用，秒级）
python -u tools/migrate_gltf_models.py --only DamagedHelmet --reuse-preview
```

> ⚠️ `--only` 是**大小写不敏感的子串匹配**：`--only Box` 会命中 `Box`/`BoxAnimated`/`BoxInterleaved`/
> `BoxTextured`/`Box With Spaces`/`BoxVertexColors` 等所有含 `Box` 的模型。要精确一个模型就写全名。
>
> ⚠️ `--force` 会重写该模型的 `.scn`（手改会丢），但**保留 `README.md`**；只刷新素材/贴图、想保留手调 `.scn` 时，
> 用 §3.4 的纯导入命令。


### 3.4 只刷新派生资产（保留手调 `.scn`）

`gltf_import.py --scene` 会**无条件覆盖** `<out>/<name>.scn`（见 §8.2），所以手调场景的模型**不要**加 `--scene`。
不带 `--scene` 时 importer 把 `*.mesh.json` 写到 `--out` 根目录（不是 `assets/`），因此先导到临时目录再拷贝：

```powershell
# 以 ToyCar 为例（有手调 ToyCar.scn，不能覆盖）
python -u tools/gltf_import.py `
  .deps/gltf-assets/glTF-Sample-Assets-main/Models/ToyCar/glTF/ToyCar.gltf `
  --out build/toycar-regen --name ToyCar --compact
Copy-Item build/toycar-regen/*.mesh.json scenes/khronos/ToyCar/assets/
Copy-Item build/toycar-regen/textures/*   scenes/khronos/ToyCar/textures/
```

### 3.5 常用参数速查

| 参数 | 作用 | 备注 |
|---|---|---|
| `--only a,b` | 只处理名字含 a 或 b 的模型（子串、大小写不敏感） | 与旧 manifest 合并记录 |
| `--exclude a,b` | 排除模型（精确名） | 默认 `ToyCar` |
| `--skip-existing` | 已有 `README.md` 的模型跳过 | 中断恢复的坑见 §8.2 |
| `--force` | 重新导入（重写 `assets/`、`textures/`、`.scn`、`preview.*`、`compare.json`） | **保留 `README.md`**（手动笔记）；手调 `.scn` 会被覆盖 |
| `--no-render` | 跳过截图 + 对比 | 只导入时用 |
| `--reuse-preview` | 不重渲染，复用已有 `preview.png` 重算对比/重写笔记 | 改了指标/README 模板后秒级刷新 |
| `--no-notes` | 不写/不覆盖 README | 调试用 |
| `--models-root` / `--out-root` / `--manifest` | 覆盖默认目录 | 一般不用 |

### 3.6 运行/截图单个场景（人工检查）

```powershell
# 打开窗口实时看（默认 800x650 窗口，等比缩放居中）
.\build-win\samples\player\maque_player.exe scenes\khronos\DamagedHelmet\DamagedHelmet.scn

# headless 截图（一条命令一张图；--shot 后退出；默认第 1 帧）
$env:MAQUE_PRESENT='off'; $env:MAQUE_WINDOW='0x0'
.\build-win\samples\player\maque_player.exe --shot out.png --shot-frame 1 scenes\khronos\DamagedHelmet\DamagedHelmet.scn

# 打印 3D 提取诊断（mesh 加载结果/材质/贴图句柄）
$env:MAQUE_RENDER3D_INFO='1'
.\build-win\samples\player\maque_player.exe --shot out.png scenes\khronos\DamagedHelmet\DamagedHelmet.scn
```

---

## 4. 流水线详解（脚本各自做了什么）

### 4.1 总流程

```
tools/migrate_gltf_models.py
  ├─ 枚举 .deps/.../Models/*             （149 个目录；默认排除 ToyCar → 处理 148 个）
  ├─ 选主变体：glTF/<Model>.gltf → glTF/ 下第一个 .gltf → glTF-Binary/*.glb
  ├─ 静态统计 glTF JSON（不读 buffer）：节点/网格/primitive/顶点/三角形/材质/纹理/图像/
  │   动画/骨骼/morph/相机/扩展/图像编码/alpha 分布/texCoord 集合/COLOR_0/非三角/缺 NORMAL
  ├─ known_gaps() → 差距清单（§5 的机器可读版本）
  ├─ blocked 判定：extensionsRequired 含 KHR_draco_mesh_compression → 记 blocked（不导入）
  ├─ tools/gltf_import.py --scene --compact  → <out>/assets/*.mesh.json + textures/* + <Model>.scn
  ├─ maque_player --shot preview.png（1280×720，第 1 帧；MAQUE_PRESENT=off MAQUE_WINDOW=0x0）
  │    └─ 完整 stdout/stderr 存 build-win/audit/migrate-logs/<Model>.render.log
  ├─ 自动异常检测（写入 issues）：cams=0 / meshes=0 / mesh=-1 / "[wgpu] error" / 无 PNG / rc≠0
  ├─ PIL：preview.png → preview.jpg（缩略 800×450，quality 82）
  ├─ compare_images(preview, screenshot/*) → compare.json（PIL/numpy；无则跳过）
  │    └─ 前景掩码上的亮度/RGB/直方图/覆盖率/包围盒 + 构图告警（§7.2）
  ├─ write_readme → <Model>/README.md（保留 <!-- 手动笔记开始 --> 之后内容）
  └─ 全部跑完 → write_index（scenes/khronos/README.md）+ manifest.json
```

驱动脚本的常用运行模式（详见 §3.3/§3.5）：`--only <模型>`（单模型/子串）、`--force`（重新导入并重写场景/派生资产，
保留 README）、`--reuse-preview`（不重渲染、只重算对比/重写笔记，秒级刷新）、`--skip-existing`（跳过已有 README 的模型）、
`--exclude`（默认排除手调 `ToyCar`）、`--no-render`/`--no-notes`（调试用）。

### 4.2 选主变体（为什么是 `glTF/`）

Khronos 每个模型可能有 `glTF/`、`glTF-Binary/`、`glTF-Draco/`、`glTF-KTX2/`、`glTF-Quantized/`、
`glTF-WebP/`、`glTF-Meshopt/`、`glTF-JPG-PNG/` 等多个变体。迁移**只取 `glTF/`**：

- `glTF/` 是**未压缩、外部 PNG/JPEG** 的规范主变体，导入器支持面最大（PNG/JPEG 直接搬运）；
- 压缩/扩展变体需要 Draco/meshopt/KTX2/WebP 解码器，引擎尚无（§5）→ 取那些变体只会 blocked；
- 变体选择规则：`glTF/<Model>.gltf` 精确同名优先，否则 `glTF/` 目录排序第一个 `.gltf`；
  没有 `glTF/` 时回退 `glTF-Binary/` 的第一个 `.glb`（GLB 含内嵌 buffer，importer 支持）。

### 4.3 导入器（`tools/gltf_import.py`）行为要点

| 环节 | 行为 |
|---|---|
| 输入 | `.gltf`（JSON + 外部/内嵌 base64 buffer）或 `.glb`（二进制 chunk）；**仅 glTF 2.0** |
| 顶点布局 | **两套**：layout0 48 B（旧：`pos3(12)+normal3(12)+tangent4(16)+uv2(8)`）；layout1 56 B（`pos3+normal3+color4+uv0_2+uv1_2`，`COLOR_0`/`TEXCOORD_1` 任一存在时自动启用）。`TANGENT` 不读（shader 用屏幕空间切线基） |
| 图元 | **只导三角形**（`mode=4`/缺省）；线/点（mode≠4）跳过并打 warning |
| 索引 | u8/u16/u32、`byteStride`、非索引 primitive 均支持；缺 `POSITION` 的跳过 |
| 缺 NORMAL | 按**真实 indices** 累积面法线后归一化（不传 indices 是旧 bug，已修） |
| 闭合朝内网格 | **整体朝内的闭合实体**（顶点 ≥64、无向边 2-共享比例 ≥98%、顶点法线相对包围盒中心平均 cos < -0.75）
→ 导入期**翻正**（法线取反 + 每个三角形绕序反转）并打 stderr；开放网格/房间内壁/天空穹顶不动。
`DirectionalLight` 的三个测试球即此类资产（源 glTF 法线/绕序一致朝内）：不修则单面剔除后只剩内表面 →
平行光只在轮廓掠射点亮、阴影贴图把远面判成被遮挡 → 整球发黑 |
| UV | `TEXCOORD_0/1` 都导入（缺 uv1 复制 uv0）；材质 `uv1_mask` 记录逐贴图用哪套（bit0..7）；`TEXCOORD_2+` 不导入——主变体唯一使用者 MosquitoInAmber 的 `eclats` primitive 带 TEXCOORD_2，但其材质无任何贴图（全 5 个 textureInfo 均 texCoord=0），已核验**无视觉影响** |
| 贴图 | PNG/JPEG **按字节原样搬运**（不解码）；WebP 用 PIL 转 PNG；KTX2/BasisU 跳过并 warning；命名 `image.name` 或 `image<idx>`（同名去重）；**glTF sampler 非默认时路径追加 `#wrap=clamp,mirror&filter=nearest`**（runtime 解析成 per-texture sampler） |
| 材质 | `pbrMetallicRoughness`（baseColor/metallic/roughness）+ emissive + `alphaMode/alphaCutoff` + `doubleSided→side`（true=2 双面/false=0 单面剔除） |
| KHR factor | unlit→`shading_model=2`(Basic)；clearcoat/sheen/iridescence/specular/ior/anisotropy → `shading_model=1`(Physical) + 对应因子；emissive_strength→`emissive_intensity`；normalTexture.scale→`normal_scale(+_y)`；occlusionTexture.strength→`occlusion_strength` |
| KHR 贴图 | 基础 5 槽 + `clearcoatNormalTexture` + **7 个扩展贴图槽**（clearcoatTexture / clearcoatRoughnessTexture / sheenColorTexture / sheenRoughnessTexture / iridescenceTexture / iridescenceThicknessTexture / anisotropyTexture，ABI `textures3[7]` + schema 路径字段）+ **KHR_materials_specular 2 槽**（specularTexture.A=specularFactor、specularColorTexture.RGB=specularColorFactor，ABI `textures4[2]`；仅作用电介质 F0，factor 可 >1） |
| KHR_texture_transform | `scale/offset/rotation` → `uv_*` + `uv_rotation`（基础贴图组）与 `normal_uv_*` + `normal_uv_rotation`（法线组）；shader 按 glTF 的 T·R·S 顺序作用（rotation 角由 gfx 打包装进 Draw 的 specular.w/sheen_color.w）；逐贴图独立 transform 仍部分近似（MR/AO/emissive 共用基础组） |
| 场景 | `--scene`：每个 `(node, primitive)` 一个**平级实体**，世界变换**烘焙**进 `local` TRS；**有 `skins`/`animations`/morph targets 的模型例外（第 29 轮）**：输出 glTF 节点树 1:1 的层级 `children`（节点=本地 TRS、mesh primitive=identity 子实体、蒙皮 mesh 实体置场景根 + `skin_path`），有非 pointer 片段时追加 `anim_player`（`scn.AnimationPlayer.clips`）；`EXT_mesh_gpu_instancing` 在导入期展开为多实体（实例 TRS = node 世界 × 实例 TRS）；自动相机（包围盒 8 角点视锥拟合，§6.2；`--use-gltf-camera` 时改用 glTF perspective 相机）+ 一盏太阳（**模型自带 `KHR_lights_punctual` 时改为导入 1~8 盏 Light、不再加太阳**）；`KHR_node_visibility.visible=false` 的节点及子树跳过；背景 `clear_color=(0.13,0.13,0.14)`；无地面、不写 `env_*` |
| 动画/蒙皮/morph（第 29 轮，plans/22） | `skins[].joints` + `inverseBindMatrices` → `assets/skin/<mesh>.<node>.skel.json`（共享 mesh 的 84 skin 用节点名后缀防覆盖）；`JOINTS_0/WEIGHTS_0` → `.mesh.json` 的 `joints/weights`（布局 2/3 = 48/56B + 24B，weights 归一化）；`animations[].channels` → `assets/anim/<clip>.clip.json`（轨道按 node 名+path 排序；pointer 通道跳过并汇总，纯 pointer 片段不写文件；CUBICSPLINE 取 value 按 LINEAR 近似）；`primitive.targets` → `.mesh.json` 的 `morph{default_weights,targets}`（targets>8 截断；TANGENT 增量忽略） |
| 确定性 | mesh JSON 排序键 + 固定缩进；**`--compact` 额外做 6 位小数舍入 + 紧凑分隔符**；同一输入两次导入逐字节一致 |

输出 `<out>/assets/*.mesh.json` 的字段：`name/vertex_stride=48/vertex_layout=0/vertices/indices/bounds_min/bounds_max/materials`；
`materials` 数组是**冗余副本**——运行时只读 `.scn` 节点上的 `asset.PbrMaterial` 组件（**以 scn 为准**）。

> ⚠️ **`.scn` 的灯光组件用重复 `"type"` 键**：第一个字符串 = 组件类型（`"render.Light"`），第二个数值 =
> `LightType`（0/1/2）。普通 `json.load`+`json.dump` 往返会丢第一个键 → 灯光组件被静默丢弃、渲染退化成
> 运行时默认主光（DirectionalLight 曾因此变成灰白球）。migrate 的手动覆盖（`apply_camera_override` /
> `apply_scene_env` / `apply_light_override` / `apply_hdri_env`）已改为**保留重复键**的读写
> （`json.loads(..., object_pairs_hook=_JsonPairs)` + 自定义 dump），并在写回后校验 `"render.Light"`
> 数量、变少则回滚该文件 + 警告。其它改 `.scn` 的工具也必须遵守这条约定。

### 4.4 渲染与 preview

- headless：`MAQUE_PRESENT=off` + `MAQUE_WINDOW=0x0` + `--shot --shot-frame 1`，场景尺寸 1280×720；
- `preview.png`（1280×720，入库排除）→ PIL `thumbnail(800,450)` → `preview.jpg`（入库）；
- 每个模型的完整渲染输出存 `build-win/audit/migrate-logs/<Model>.render.log`（构建产物，不入库；
  `--reuse-preview` 时不重渲染、不重写该日志）；
- **自动异常检测**：渲染后扫描日志，发现下列任一情况会写入该模型的 `issues`（并出现在 README「踩过的坑」）：
  - 无 `3d scene cams=` 行或 `cams=0` → 场景相机无效；
  - `meshes=0` → 没有网格被提取（资产/实体问题，通常是 `mesh=-1` 或实体为空）；
  - 出现 `mesh=-1` → 网格加载失败（路径/JSON/VFS 根）；
  - 出现 `[wgpu] error` → WebGPU 校验错误；
  - `preview` 前景覆盖率 < 2% → 机位过远/模型过小；构图告警（贴边/占比过小）见 §7.2。
- 同一机器/同一构建下，同场景同帧两次截图的 PNG **SHA256 相同**（实测；跨 GPU/驱动不保证）；
- 每个模型的 `import_seconds`/`render_seconds` 记录在 `manifest.json`，可用来定位"为什么这批这么慢"。

### 4.5 逐模型笔记（README.md）

固定结构（脚本生成，勿手改上方部分）：

```
# <Model> —— Khronos glTF 示例迁移
来源 / 上游概览 / 标签 / 许可（来自 metadata.json，保留出处）
模型统计（主 glTF、节点/网格/primitive、顶点/三角形、材质/纹理/图像、图像编码、动画/骨骼/morph/相机、扩展、alpha 分布）
导入产物（.mesh.json 数量与体积、贴图数）
生成 / 运行（重生成命令 + 运行/截图命令）
与官方 screenshot 对比（前景平均亮度/RGB/直方图相似度/前景占比/构图告警 + 自动初判）
差异归因（按本模型实际特征生成）
已知差距（引擎/导入器层面）
踩过的坑 / 注意（导入警告、大模型、无 NORMAL、渲染异常、参考图差异初判）
<!-- 手动笔记开始（重生成时保留以下内容） -->
（人工补充区：重生成时原样保留）
```

**手动笔记区**是鼓励人工补充的地方（例如"这个模型的贴图用了 TEXCOORD_1，袖子花纹错位"）。
`--force` 只清派生资产/场景/预览，**不清 `README.md`**，所以手动笔记在重生成时一定会保留；
仍建议随 README 一起提交（git 兜底）。

### 4.6 总索引与 manifest

`scenes/khronos/README.md`（脚本生成）包含：总览统计、模型表（标签/顶点/材质/差距数/状态/预览/笔记链接）、
blocked+失败明细、复现命令；自动批量记录之外，目录里"有 `.scn` + `README.md` 但不在 records"的模型
（如手调的 `ToyCar`）会补一行状态 `ok (hand-tuned)`。**勿手改**（每次批量重写）。

> ⚠️ 总索引"总览"里的**「失败/异常」计数**会把补行的手调模型也算进去（脚本统计口径 bug，例如显示
> "失败/异常 1"而实际是 `ToyCar`）——**状态以 `manifest.json` 的 148 条自动记录为准**。

`manifest.json`（脚本生成，`indent=1, sort_keys=True`）字段：

| 字段 | 说明 |
|---|---|
| `generated_by` / `models_root` / `count` / `total_seconds` | 生成器、源目录、记录数、总耗时 |
| `records[]` | 每个模型一条，按模型名排序 |
| `records[].status` | `ok`（导入+预览+JPG 全成功）/`imported`（导入成功但无 preview.jpg，通常缺 PIL）/`blocked`（主变体必需 Draco 等）/`failed`（导入失败）/`render_failed`（渲染失败）/`exception`（脚本异常） |
| `records[].source` | 主变体相对路径 |
| `records[].tags` / `summary` / `reference` | 来自上游 `metadata.json` / 参考图路径 |
| `records[].stats` | §4.1 的静态统计 |
| `records[].gaps[]` | `known_gaps()` 生成的差距清单（§5 的机器可读版） |
| `records[].issues[]` | 导入警告、大模型、无 NORMAL、`meshes=0`/`mesh=-1`/`[wgpu] error`、覆盖率/构图告警、参考图差异初判 |
| `records[].import` / `out_size_bytes` | 导入产物数量/体积 |
| `records[].import_seconds` / `render_seconds` | 单模型耗时 |
| `records[].render_log_tail` / `import_log_tail` | 渲染/导入日志末 6 行（完整日志在 `build-win/audit/migrate-logs/`） |
| `records[].compare` | §7.2 指标（PIL 可用时）：前景亮度/RGB/直方图/RGB 距离/覆盖率/bbox/构图告警 |
| `records[].readme` / `preview_png` / `preview_jpg` | 产物路径 |

### 4.7 确定性（可复现的两条硬约束）

1. **导入确定性**：同一输入 + 同一 flag 两次导入**逐字节一致**（排序键 + 固定缩进；`gltf_import_e2e` 有 `cmp` 断言）。
   流水线**固定 `--compact`**：`--compact` 会做 6 位小数舍入，**与默认输出数值不同**（X2），混用会让"确定性"失效。
2. **截图确定性**：同一机器/构建、同场景同帧两次 `--shot` 的 PNG 逐字节一致（本机实测 SHA256 相同）；
   跨 GPU/驱动不保证。`preview.jpg` 经 PIL 有损压缩，只作展示，不作确定性证据。

---

## 5. glTF 特性支持矩阵与差距

> **现状**口径：截至本次批量迁移（`plans/20` 审计修复后）。**本批命中**来自
> `scenes/khronos/manifest.json` 对 **148 个迁移模型主变体**（`glTF/`）的统计（精确到模型数/primitive 数）。
> 注意 `plans/20 §4.1` 还有一套**全变体**扫描数字（含 Draco/KTX2/WebP/Quantized 变体，量更大），
> 只作历史背景；迁移只取主变体，所以以本表为准。
> 逐模型的差距记录在 `scenes/khronos/<Model>/README.md` 的「已知差距」与 `manifest.json` 的 `gaps[]`。

### 5.1 几何 / 顶点

| 特性 | 现状 | 影响 | 规避/后续 |
|---|---|---|---|
| 三角形 `mode=4`、缺省索引、u8/u16/u32、`byteStride` | ✅ 支持 | — | — |
| 非三角 primitive（线/点/三角带） | ⚠️ 跳过 + warning（**22 个 primitive / 2 个模型**：MeshPrimitiveModes、PrimitiveModeNormalsTest） | 模型缺少线条/点装饰 | 后续补 topology 或明确标注 |
| 缺 `NORMAL` | ✅ 按真实 indices 生成（45 个 primitive） | — | — |
| `TANGENT` | ⚠️ 不读；shader 用屏幕空间切线基 | 极端 UV/法线贴图可能与官方略异 | 可接受（审计结论）；后续可加顶点切线 |
| `TEXCOORD_1` | ✅ 支持（布局 v1：uv1 进顶点；材质 `uv1_mask` 逐槽选择；主变体约 50 处 textureInfo 用 texCoord>0） | — | `TEXCOORD_2+` 未导入但无使用者（唯一属性不在任何贴图上，见 §5.3 UV 行） |
| `COLOR_0` 顶点色 | ✅ 支持（布局 v1：f32 RGBA，含 normalized u8/u16；`baseColor × 顶点色`，验证样本 `BoxVertexColors`） | — | — |
| `KHR_mesh_quantization` / `normalized` 顶点 | ⚠️ 无特殊处理（1 个模型） | 量化网格几何/法线可能异常 | 主 `glTF` 变体通常未量化；导入前检查 `extensionsRequired` |
| 扁平节点矩阵 / 负缩放（镜像） | ⚠️ 烘焙到 TRS：**镜像符号丢失**（分解出全正 scale） | 镜像件的面朝向/法线可能错 | 已知限制；需要时手改 `.scn` 的 `scale`（负值）+ 材质 `side` |
| 嵌套非均匀缩放（剪切） | ⚠️ TRS 无法表达（分解误差可达 0.36） | 少量模型几何轻微变形 | 已知限制；优先用源变异体/手修 |
| `sparse` accessor | ⚠️ 未特殊处理 | 少数特殊资产可能错 | 未见主变体命中；后续补 |

### 5.2 图像 / buffer

| 特性 | 现状 | 影响 | 规避/后续 |
|---|---|---|---|
| GLB 内嵌 buffer | ✅ 支持 | — | — |
| data: URI（base64）buffer/贴图 | ✅ 支持 | — | — |
| PNG 贴图 | ✅ 原样搬运（引擎 image 模块解码） | — | — |
| 贴图编码（主变体 148 模型） | PNG **447** 张、JPEG **237** 张、WebP **13** 张（KTX2 0；WebP 在导入期转 PNG）；全变体扫描另有 KTX2 81、WebP 26 |
| JPEG 贴图 | ✅ 原样搬运（审计修复 I1；主变体 237 张） | — | — |
| KTX2 / BasisU | ❌ 无解码器（主变体无命中；全变体扫描 7 个变体/81 张） | 相关贴图缺失（材质只剩因子） | 用 `glTF/` 主变体；根治需 KTX2 解码依赖 |
| WebP | ✅ **导入期用 PIL 转 PNG**（主变体 1 个模型 / 13 张：SheenWoodLeatherSofa）；PIL 不可用时跳过并告警 | 无（需 Pillow） | 源图也会被转成 PNG 入库（派生资产） |
| `KHR_draco_mesh_compression`（required） | ❌ 无解码器 → **blocked**；本批主变体**全部不 required（0 blocked）**（全变体扫描 18 个 Draco 变体） | 只有 Draco 变体时无法迁移 | 主 `glTF` 变体通常存在；没有则记 blocked |
| `EXT_meshopt_compression` | ❌ 无解压（主变体 1 个模型） | 该模型几何/贴图可能不完整 | 同上 |
| EXIF orientation | ⚠️ 不处理 | glTF 贴图一般不携带；异常时贴图转向 | 已知限制 |

### 5.3 材质（core + KHR）

| 特性 | 现状 | 影响 | 规避/后续 |
|---|---|---|---|
| `pbrMetallicRoughness` 因子 | ✅ | — | — |
| baseColor / metallicRoughness / normal / occlusion / emissive 贴图 | ✅ 5 槽（per-texture sampler，REPEAT/filter 生效） | — | — |
| glTF sampler（wrapS/wrapT/min/magFilter） | ✅ 导入为贴图路径后缀 → per-texture sampler；`TextureSettingsTest` 的 clamp/repeat/mirror 行全部通过 | — | 默认(REPEAT,线性)不写后缀，旧路径零漂移 |
| `alphaMode` OPAQUE / MASK / BLEND | ✅；MASK 走 opaque+discard；BLEND 物体级排序（远→近）；transmission/diffuse 近似把 MASK 改 BLEND 时用 `alpha_mask_in_blend` 保留 alpha 镂空 | BLEND 内部三角形不排序 | 可接受；复杂透明件参考图对比时人工判读 |
| `doubleSided` | ✅ 导入为 `side`（true=2/false=0），shader 背面翻转法线 | — | 手写 `.scn` 必须显式写 `side`（运行时只认它） |
| `KHR_materials_unlit` | ✅ → Basic（shading_model=2）（6 个模型）；**unlit 不做 ACES**（`output_direct`），颜色与 baseColor 一致 | — | 与 three.js toneMapped=true 有细微差异，按 glTF unlit 语义取色 |
| clearcoat / sheen / iridescence / specular / **ior**（**因子**） | ✅ → Physical 近似（本批：clearcoat 12、sheen 9、iridescence 10、specular 10、ior 17 个模型） | 观感与 three.js/Khronos 规范有差距（近似 BRDF） | — |
| `KHR_materials_anisotropy`（**7**） | ✅ **各向异性 GGX**（第 21 轮，Khronos 规范实现）：`at = mix(roughness², 1, strength²)`、`ab = roughness²`、`anisotropyRotation` 旋转切线方向、方向/强度贴图（RG/B）；无 TANGENT 时用 UV 导数构造切线基；环境反射用规范 bent-normal 近似 + 沿切向多采样 | 低粗糙度 + 高强度时高光被拉成弧/条，`AnisotropyStrengthTest` 的强度×粗糙度扫描正确；环境反射仍基于解析 studio（无真实 HDRI） | — |
| 上述扩展的**贴图槽** | ✅ 已支持 **12 槽**：clearcoat(R)/clearcoat roughness(G)/sheen color(RGB,sRGB)/sheen roughness(A)/iridescence(R)/iridescence thickness(G)/anisotropy(B) + specular(A=specularFactor)/specularColor(RGB,sRGB) + transmission(A) + diffuseTransmission(A) + diffuseTransmissionColor(RGB,sRGB)；缺失槽绑 white 且由 flags2 presence 位关闭（bit12..18/21..23/25..26） | 扩展贴图暂共用基础 `uv_scale/uv_offset` 与 TEXCOORD_0（specular 同） | — |
| `KHR_materials_emissive_strength` | ✅ → `emissive_intensity`（5 个模型） | — | — |
| `KHR_materials_transmission`（**33 个模型 / 124 处**） | ✅ **屏幕空间折射近似**（第 20 轮）：`alpha_mode=2` + 折射 pass——gfx 把 3D 拆成「opaque pass → 颜色 copy 成 scene color → blend pass」，shader 按**逐像素视线折射 + 体积厚度**把玻璃内出射点投影回屏幕采样背景（球/管件有真实透镜形变）；`transmissionTexture.R` 调制透射率（黑=不透，故 CompareDispersion 的 glTF logo 为白色不透明）；Fresnel 环境反射叠加；原 MASK 材质保留镂空；仅**单视图 + 离屏目标**（直呈 surface 无 CopySrc → 回退 alpha 近似） | 玻璃可透视且有折射畸变/边缘高光；不是真实光线追踪，折射只采样一次、无体积吸收/多次内反射；BLEND 物体**不再投影** | 根治需 backbuffer 多次折射 + volume 吸收/散射（P1 剩余） |
| `KHR_materials_volume`（**25**）/ `dispersion`（4）/ `volume_scatter`（1） | ✅ **厚度/吸收/散射完整接入**（第 24 轮）：`thicknessFactor` + `thicknessTexture.G` → 折射路径厚度；`attenuationColor`/`attenuationDistance` → **Beer-Lambert**（`T = exp(-(1-attenuationColor)·thickness/distance)`，逐通道，作用在折射采样与漫透射上，光学深度封顶 3.0 防极薄介质归零；旧「base_color×attenuationColor」薄壁 hack 已删）；**`KHR_materials_volume_scatter` 的 `multiscatterColorFactor`** → 各向同性 in-scatter + 体积介质漫反射色调化（ScatteringSkull 青绿半透）；`KHR_materials_dispersion` 仍是 R/G/B 三 IOR 折射偏移近似 | 有色玻璃/琥珀/牛奶玻璃有真实厚度衰减；色散可见但非光谱积分；散射非多次散射/相位函数 | 更高保真需体积跟踪（多次散射、相位函数）（§11） |
| `KHR_materials_diffuse_transmission`（**6**） | ✅ **不透明材质 + 背光透射**（第 22–23 轮）：**不再 alpha BLEND**（旧实现让陶瓷/叶片变"幽灵玻璃"）；`factor`/`colorFactor` + **diffuseTransmissionTexture(A) / diffuseTransmissionColorTexture(RGB,sRGB)** 两槽完整接入（`textures6[2]`、flags2 bit25/26、group1 binding 20/21）；shader 加「背光 wrap diffuse + 背面环境辐照」透射项（`Draw.trans`，材质 uniform 288→304B） | 陶瓷/叶片保持实心、有暖调透光；测试板的 factor/colorFactor/透射贴图/透射色贴图四行均可复现；无散射/厚度，能量拆分用可加近似（不压暗前向漫反射） | 完整版需体积散射/厚度 + 规范能量拆分 |
| `KHR_materials_variants`（**7**） | ❌ 只用默认材质 | 变体切换不生效 | 静态迁移可接受 |
| `KHR_materials_pbrSpecularGlossiness`（旧扩展，1） | ❌ | 极少数旧资产材质退化 | 优先切 MR 变体 |
| `KHR_materials_retroreflection`（1） | ❌ | 反光材料（交通标志类）缺失 | 记录 gap |
| `KHR_texture_transform`（**14 个模型**） | ✅ `scale/offset/rotation` 已支持（基础组 + 法线组）；逐贴图独立 transform 仍部分近似（MR/AO/emissive/扩展贴图共用基础组） | 平铺/偏移/旋转正确；个别模型逐槽差异仍在 | 剩余为低优先级逐槽矩阵 |
| glTF 默认材质语义 | ⚠️ importer/runtime 默认值与 glTF 规范略有差异 | 无材质 primitive 呈"灰塑料" | 已登记；不阻塞 |

### 5.4 场景 / 动画 / 灯光

| 特性 | 现状 | 影响 | 规避/后续 |
|---|---|---|---|
| 节点层级 | ✅（第 29 轮）有 `skins`/`animations`/morph targets 的 **27 个模型改用 glTF 节点树 1:1**（嵌套 `children`，节点实体=本地 TRS、mesh primitive 为 identity 子实体、蒙皮 mesh 实体置场景根）；其余 121 个模型仍平级烘焙（逐字节不变） | 动画/骨骼/morph 可正确播放；静态模型保持旧输出 | 非动画模型的层级化保留为 P3（会改 121 个模型的场景与基线） |
| `EXT_mesh_gpu_instancing`（1 个模型） | ✅ 导入期展开实例（SimpleInstancing 125 实例；上限 4096，超出 warning 截断） | 几何正确；非 GPU 实例化（每个实例一个实体/draw，125 draw 在本批可接受） | 评估：仅 1/148 模型、单 draw 成本低，暂不做 GPU 实例化（§11） |
| glTF 自带相机（`cameras`/`node.camera`，**14 个模型 / 45 个相机**） | ✅ **支持使用**（第 18 轮）：importer `--use-gltf-camera` 取节点遍历第一个 **perspective** 相机（fov/znear/zfar + 节点世界变换；orthographic 警告后回退自动机位）。migrate 对实测指标改善的 **5 个模型**（Duck / EnvironmentTest / IridescentDishWithOlives / TrafficCone / TransmissionTest）采用 glTF 相机；多相机模型（Cameras/AnimationPointerUVs/ToyCar/VirtualCity）参考图多为多视图拼接，仍自动机位；另保留 `MANUAL_CAMERA` 手动机位与 `--camera-position/target/fov/distance-scale` 覆盖 | 5 个模型构图显著改善（Duck hist 0.388→0.629、TransmissionTest 0.292→0.444；rgb 均下降）；其余因官方参考机位/多视图差异仍用自动或手动机位 | 引擎仅透视相机；orthographic 与"多相机选哪个"仍无解（§11） |
| `KHR_lights_punctual`（**12 个模型**） | ✅ 导入为 `render.Light`（directional/point/spot，≤8 盏、方向光优先；spot 内/外锥角、range 保留）；强度为**观感归一化**（directional=2.6×intensity；point/spot 按模型尺度换算，非 cd/lux 物理值）；有灯光时不再追加默认太阳 | 自带布光能还原主体明暗/彩色灯；与官方离线渲染的精确强度/软阴影仍有差异 | 手调 `.scn` 可改 Light 因子；后续接物理单位换算/光追阴影 |
| `KHR_node_visibility`（3 个模型） | ✅ 支持：`visible=false` 的节点及整棵子树不导入（网格/灯光都不生成） | 可见性测试模型不再出现叠影/扩展不支持卡 | 运行时动态切换可见性未做 |
| 动画（**26 个模型 / 38 个动画**）/ 骨骼蒙皮（7 个模型 / 90 个 skin）/ morph（4 个模型 / 6 个 primitive） | ✅ **第 29 轮已支持**（`plans/22`）：importer 输出层级场景 + 派生 `assets/skin/*.skel.json` + `assets/anim/*.clip.json`；runtime 按 `scn.AnimationPlayer.clips` 自动播放首个片段（STEP/LINEAR、quat slerp），关节调色板 = 关节世界矩阵 × inverse_bind（gfx 顶点布局 2/3 + storage palette + 阴影同绑）；morph 走 CPU 顶点更新（默认权重 + weights 动画）。`KHR_animation_pointer`（5）仍不导入 | 角色摆姿势/形变正确；预览=第 1 帧姿态（与参考图的其它动画帧姿势不同属正常） | CUBICSPLINE 按 LINEAR 近似、无状态机/混合；pointer 为 P3（§11） |
| 场景根 `env_*` / `env_texture` / `env_background` | ✅ **官方 HDRI IBL**（第 25 轮起用于迁移）：`env_texture` 写 **Radiance `.hdr`（RGBE）**、**OpenEXR scanline**（NONE/RLE/ZIP/ZIPS）、PNG/JPEG 时启用；装配期 CPU 预计算 **GGX importance-sample 粗糙度 mip 链** + **余弦卷积漫反射 irradiance** + **天空背景全屏 pass**；`env_background=false` → HDRI 只做 IBL，背景保留相机 `clear_color`。迁移按 `tools/khronos_env.json`（README 明确声明环境的 23 个模型）注入官方 HDRI：**env_intensity 自动标定**（按参考图前景亮度，1~2 轮）+ **指标回退**（hist 比 analytic 差 >0.02 则撤销，最终 17/23 保留）。HDRI 文件不入库（许可混杂 + 体积）→ `.deps/khronos-env/`，渲染时 `MAQUE_MOUNT="/khronos_env=<repo>/.deps/khronos-env"`，未挂载自动回退 analytic | 反射/环境光接近官方 HDRI studio（AnisotropyDiscTest/IridescenceLamp/CarConcept 等 hist +0.2~0.4）；cubemap（第 26 轮：`scn.Scene.env_cubemap` 6 面 → GGX cube + irradiance cube，按方向采样）与 equirect 任选；无面光软阴影 | §11：更多模型缺官方环境信息（126 个 README 未声明） |
| MSAA | ✅ **opt-in 4x**：`scn.Scene.msaa_samples=4` 或 `MAQUE_MSAA=4` → 4x 颜色/深度附件 + pass 结束 resolve；默认 0/1 = 关（旧场景逐像素不变）；`gfx caps.max_samples` 探测设备能力，创建失败自动回退 1x | 边缘锯齿明显改善（ToyCar/金属轮廓）；4x 显存/带宽约 4 倍，迁移批量预览默认仍关 | 参考图多为离线抗锯齿渲染，批量预览可后续按需开；MSAA 不解决纹理/高光走样 |
| 地面/背景板/阴影接收体 | ❌ 自动场景无 | "模型飘在空中"，接触阴影缺失 | 需要时手加地面 + `material_shadow.scn` 范式（§6.4 ⑤） |

---

## 6. 自动场景、机位、灯光与环境（含手调教程）

### 6.1 自动生成的 `.scn` 结构

```jsonc
{
  "title": "<Model>", "width": 1280, "height": 720, "views": 1,
  "entities": [
    { "name": "camera", "components": [
        { "type": "scene.Transform3D", "local": { "position": {...}, "rotation": {"x": pitch, "y": yaw, "z": 0} } },
        { "type": "render.Camera3D", "fov_y": 45.0, "near": ..., "far": ...,
          "clear_color": { "r": 0.13, "g": 0.13, "b": 0.14, "a": 1.0 } } ] },
    { "name": "sun", "components": [
        { "type": "scene.Transform3D", "local": { "rotation": {"x": lrx, "y": lry, "z": 0} } },
        { "type": "render.Light", "intensity": 2.6, "color": { "r": 1.0, "g": 0.96, "b": 0.9, "a": 1.0 } } ] },
    { "name": "<node>_<prim>", "components": [
        { "type": "scene.Transform3D", "local": { "position": ..., "rotation": ..., "scale": ... } },
        { "type": "render.MeshRenderer", "mesh_path": "assets/<...>.mesh.json" },
        { "type": "asset.PbrMaterial", ... } ] }   // 每个 (node, primitive) 一条
  ]
  // 可选手调：env_color / env_intensity / env_sky_color / env_ground_color / env_highlight / env_rotation
}
```

约定：**位置/旋转单位**——`rotation` 是弧度制 Euler（R = Rz·Ry·Rx，与 runtime `m4_from_trs` 一致）；
`render.Light` 的传播方向 = 节点 **-Z** 轴；贴图/网格路径**相对场景目录**（运行时 VFS 根 = 场景目录）。

### 6.2 自动机位（包围盒 8 角点视锥拟合）

相机朝向固定为**左上前 3/4 视角**方向 `dir = normalize(-0.485, 0.222, 0.849)`，`fov_y=45°`，1280×720（aspect=16:9）。

**距离不再用单一包围球半径**，而是求"让世界 AABB 的 8 个角点都落进视锥、且留 8% 边"的最小距离
（`gltf_import.py::_fit_camera_distance`，`margin=0.92`，最多迭代 10 次）：

```
1. 构造相机基：forward = -dir；right = normalize(cross(forward, up))；up = cross(right, forward)
2. dist 初值 = 2 * radius（radius = AABB 半对角线，下限 1e-3）
3. 迭代：
   对 AABB 8 角点投影：z = (p - eye)·forward，
       nx = |(p-eye)·right| / (z * tan(fov/2) * aspect)，
       ny = |(p-eye)·up|    / (z * tan(fov/2))
   worst = max(nx, ny, 1e-6)
   worst > margin → dist *= worst/margin（退远，重新投影）
   worst ≤ margin → dist *= max(0.9, worst/margin)（尝试靠近）；接近 1.0 时收敛退出
4. 上限保护：dist ≤ 200 * radius
```

最终：

```
eye = center + dir * dist
near = max(1e-4, dist * 0.02)
far  = max(dist * 10, (dist + radius) * 8)
```

- **演化历史**：旧公式 `eye = center + (-1.2r, +0.55r, +2.1r)` 对 1 单位立方体只退到 1.2r，
  物体**顶满画面**；中间版用"包围球半径/tan(fov/2)×1.35"，对**薄板/长条**模型又过远（包围球高估尺寸）；
  当前版直接按 8 角点投射进视锥，宽扁/长条模型也能贴边构图。
- `near/far` 随 `dist` 派生，能覆盖 1e-4 尺度（ToyCar/珠宝类）到上百单位的大场景。
- 注意：世界 AABB 是"局部 bounds × 世界矩阵的 8 角点 min/max 包络"（旋转时偏大，不是精确 OBB）；
  极端旋转/镜像/剪切模型取景可能偏大。**基准判定用自动指标 `preview_coverage` + `framing_notes`**（§7.2），
  仍然不佳的手调（§6.4）。

### 6.3 灯光与环境默认值

| 项 | 自动场景 | 引擎默认/说明 |
|---|---|---|
| 方向光 | `intensity=2.6`，暖白 `(1.0,0.96,0.9)`，传播方向 `(0.35,-0.85,-0.4)`（左上前） | 灯光方向约定见 `plans/20 §2.3`（`_light_rotation` ↔ runtime ↔ shader 一致） |
| 环境 | **不写 `env_*`** → 用场景默认 | `env_color=(0.27,0.28,0.31)`、`env_intensity=1.6`、`env_sky/ground=0`（0=由 env_color 派生）、`env_highlight=1.0`、`env_rotation=0` |
| 背景 | `clear_color=(0.13,0.13,0.14,1)`（深灰） | 与官方参考（多为浅灰/白棚拍）不同 → 对比时计入归因 |
| 阴影 | 方向光阴影自动生效（1024² + 3×3 PCF） | 仅对可见 mesh 生效；`MAQUE_SHADOW=0` 可对照 |
> **官方 HDRI 许可（第 25 轮）**：`tools/khronos_env.json` 引用的 11 个环境来自 Khronos
> glTF-Sample-Environments（`low_resolution_hdrs` 分支）：Wide Street / Cannon Exterior = CC0；
> Studio Neutral / Colorful Studio = CC-BY-4.0（需署名）；Footprint Court = CC-BY-NC-SA-3.0
> （非商业 + 相同方式共享）；Pisa / Dining Room = 无 license 文件（USC ICT 上游）。
> → **HDRI 一律不入库**，只放 `.deps/khronos-env/`（gitignored，下载脚本
> `maque-test-tmp/hdri_lab/fetch_hdri.ps1`）；`.scn` 里写挂载路径，未挂载自动回退 analytic。
> 复现官方对比预览：`python tools/migrate_gltf_models.py --only <Model>`（工具自动
> `MAQUE_MOUNT=/khronos_env=<repo>/.deps/khronos-env` 并做 env_intensity 标定；`--no-hdri-env` 关闭）。
> `USDShaderBallForGltf` 官方参考是 HDRI 亮背景 → `khronos_env.json` 里 `env_background: true`，
> 其余模型保持暗色背景只取 IBL 光照。

| IBL | **官方 HDRI PMREM IBL（第 25 轮）**：`.hdr`（RGBE）/EXR/PNG/JPEG/builtin studio；装配期 **GGX 粗糙度 mip 预过滤 + 漫反射 irradiance 卷积** + Karis env BRDF；`env_background=false` → 仅 IBL 不画天空。23 个 README 声明环境的模型按 `tools/khronos_env.json` 注入官方 HDRI（env_intensity 自动标定 + 指标回退，17 个保留）；其余 126 个用 analytic studio IBL（150 迁移基线默认不变） | 有 HDR 高光/环境反射；接近官方 HDRI studio；cubemap 与 equirect 均支持（第 26 轮 `env_cubemap` 6 面资产）；无面光软阴影 |

### 6.4 手调教程（preview 不理想时）

按以下顺序改 `.scn`，每次只改一类，截图对比：

**① 构图/取景**（改 `camera` 实体）——先确定"模型完整、不过小、不裁切"

| 症状 | 改法 |
|---|---|
| 模型顶满/被裁切 | 增大 `position` 的 `|x|,|y|,|z|`（沿视线方向拉远，≈×1.3），或把 `fov_y` 调大 |
| 模型太小/大量空画面 | 把 `position` 等比缩小靠近（≈×0.7） |
| 看到模型内部/背面 | 检查 `rotation` 的 pitch/yaw；也可直接抄一个相近模型的相机（`<Model>.scn` 的 camera 段） |
| 全黑或只剩背景 | 先按 §9.1 排查（`mesh=-1`/near-far/尺度），**不要**先怀疑机位 |
| 极端尺度模型（±1e4 或 1e-4） | 同时改 `near/far`（保持 `near << 模型尺度 << far`），fov 45 一般不动 |

**② 光照**（改 `sun` 或新增灯）——方向/强度/颜色

- `render.Light` 的传播方向 = 节点 **-Z**；改 `rotation`（弧度）或 `scene.Transform3D.local.rotation`。
- 多灯 ≤8 盏同时生效；**方向光优先参与阴影**，点光/聚光用 `type`（0/1/2）与 `range/inner_angle/outer_angle`。
- 参考图是软阴影/面光时，本引擎只有方向光硬阴影 → 调 `intensity`/环境项**让暗部不死黑**即可，不要追求一致。

**③ 环境/背景**（`.scn` **根级**字段，不是实体组件）——这里对观感影响最大

```jsonc
// ToyCar 手调过的"影棚"预设（可直接抄到别的模型，按参考图再微调）
"env_color":       { "r": 0.42, "g": 0.44, "b": 0.48, "a": 1.0 },
"env_intensity":   1.25,
"env_sky_color":   { "r": 0.50, "g": 0.53, "b": 0.60, "a": 1.0 },
"env_ground_color":{ "r": 0.18, "g": 0.16, "b": 0.15, "a": 1.0 },
"env_highlight":   3.2,
"env_rotation":    30.0
```

- `env_color/env_intensity` 决定整体环境亮度；`env_sky/ground` 决定半球渐变色（0 色=派生）；
  `env_highlight` 是软光箱强度（金属/车漆的镜面感来源）；`env_rotation` 单位**度**。
- 背景（`clear_color`）建议对比参考图改中性灰（如 0.5 左右），深色背景会让亮度指标整体偏低。
- 材质级还有 `asset.PbrMaterial.env_intensity`（单材质调和，ToyCar 的 Glass 用了 1.2）。

**④ 材质**（节点上的 `asset.PbrMaterial` 组件）

- 贴图缺失/贴错先看 README「已知差距」与 `manifest.json` 的 `gaps`，不要瞎调因子；
- transmission 类玻璃：`transmission_factor`（+ 可选 `transmission_texture`/`volume_thickness`/`dispersion`）
  走屏幕空间折射 pass；纯手写场景若只要"半透"观感也可继续用 `alpha_mode=2`(BLEND) + `opacity≈0.3`
  （ToyCar Glass 即此做法，非 transmission 扩展不走折射）；
- clearcoat 车漆：`shading_model=1` + `clearcoat=1` + `clearcoat_roughness≈0.05`；
- 手写材质必须显式写 `side`（true→2 / false→0），运行时剔除只认 `side`。

**⑤ 加地面/阴影接收（可选）**

自动场景没有地面，模型像"飘在空中"。需要接触阴影时，参考 `scenes/pbr/material_shadow.scn` 的
`ShadowMaterial` 范式（`shading_model=9` + `alpha_mode=2` + 平面）加到 `.scn` 里；注意这会让预览含"不存在于源资产"的内容，
**只用于视觉参考，不作为迁移正确性证据**，并在 README 手动笔记里说明。

### 6.5 机位/灯光/env 速查

| 字段 | 位置 | 类型/单位 | 备注 |
|---|---|---|---|
| `render.Camera3D.fov_y / near / far` | camera 实体的 `Camera3D` 组件 | 度 / 世界单位 | `near/far` 必须包住模型深度 |
| `render.Camera3D.clear_color` | 同上 | RGBA 0–1 | 背景色 |
| `scene.Transform3D.local.position/rotation/scale` | 任意实体 | 单位/弧度 | rotation Euler(Rz·Ry·Rx) |
| `render.Light.intensity/color/type/range` | 灯实体 | 1 / RGBA / 0,1,2 / m | >8 盏截断（按优先级） |
| `env_color/env_intensity/env_sky_color/env_ground_color/env_highlight/env_rotation/env_texture` | **场景根** | RGBA / 1 / 度 / 路径 | `env_texture` 空 = 默认 analytic IBL；写 `builtin:/textures/studio_env` 或 VFS/远程 PNG/JPEG 启用 env；`MAQUE_ENV_IBL=1` 强制内建 env |
| `asset.PbrMaterial.env_intensity/opacity/side/clearcoat/...` | 网格实体 | 见 `plans/19 §4` | `side` 必须显式写 |

### 6.6 手动机位与相机 CLI（自动机位不适配时）

自动机位是**整模型包围盒 8 角点视锥拟合**：对"官方参考图是室内/特写/特定角度"的资产并不总能对上。
迁移器提供两层手调手段：

1. **importer 相机参数**（生成 `.scn` 时直接生效）：
   ```bash
   python tools/gltf_import.py <model.gltf> --out <dir> --name <Model> --scene --compact      --camera-position "9.5,1.7,0" --camera-target "-13,2.2,0" --camera-fov 65   # 显式机位
   # 或只微调自动拟合距离：
   ... --camera-distance-scale 0.8
   ```
   `--camera-position/--camera-target` 为**世界坐标（单位与模型一致）**；两者搭配时忽略自动拟合；
   `--camera-distance-scale` 乘到自动拟合距离上（<1 靠近、>1 拉远，`near/far` 同步）。
2. **迁移器手动机位表** `tools/migrate_gltf_models.py` 的 `MANUAL_CAMERA`：按模型名覆盖
   `position/target/fov/near/far`，每次 `--force`/`--only` 后**幂等**写回 `.scn`，并在 `manifest.records[].camera_override`
   与 README 的 issues 里记录"手动机位"。当前已内置 **Sponza**（官方参考为主厅内部视角；世界尺寸约 30×12×18，
   相机放在 +X 端柱廊内沿 -X 看）。新增手调模型时把坐标加进 `MANUAL_CAMERA` 即可，别直接改 `.scn`（会被重生成覆盖）。

**坐标系注意**：很多 Khronos 模型在节点上带缩放（如 Sponza 节点缩放 ~0.02），**顶点 `.mesh.json` 的 bounds 是局部值**，
手动机位必须用**世界坐标**（可从 `.scn` 实体变换 × mesh bounds 重算，或直接拿运行日志/编辑器查看）。

> ⚠️ **不要盲目用参考图覆盖率自动缩放机位**：`--auto-framing` 实验按
> `coverage_ratio = preview/ref` 逐模型缩放距离（cov ∝ 1/d²），实测 134 个模型里 132 个出现
> "主体贴边/裁剪"（参考图常是特写/局部构图，覆盖率不可直接对齐）。默认**不开**；仅在人工确认参考图
> 构图与整模型一致时，用 `--auto-framing --dry-run` 看计划再决定。

---

## 7. 参考图对比方法（指标定义、判读与归因）

### 7.1 为什么不能做像素级 pass/fail

Khronos `Models/<Model>/screenshot/` 的官方图**大多是资产作者在离线渲染器里出的展示图**，与本引擎渲染不可逐像素比较：

| 差异源 | 说明 |
|---|---|
| 渲染器不同 | 官方常见 Blender/three.js/Marmoset 等，BRDF 实现/色调映射/曝光不同 |
| 光照/环境 | 多为 **HDRI studio**（IBL、面光、软阴影）；maque 只有方向光 + analytic studio IBL |
| 机位/背景 | 官方机位/焦距/背景与自动机位不同；部分图为**透明背景**，部分带地面/背景板 |
| 参考图格式/选择 | **large 优先**：先找 basename 含 `large` 的（`screenshot_large.*`/`screenshot-Large.*`/`large.jpg`…），再 `screenshot.*`，再 basename 含 screenshot，最后第一张图；格式 png/jpg/jpeg/webp/**gif**（gif 用 PIL 取第一帧）。**149 个模型都有 `screenshot/`，148/148 全部生成对比**（86 个用了 large 版、13 个仅有 GIF）。"统计缺失"不是"差异大"——若以后出现无 `screenshot/` 的资产，笔记应标"上游无参考图" |
| 缺失特性 | transmission/volume（真实折射）、`KHR_animation_pointer` 等（§5）；MSAA 已提供 opt-in 4x；动画/骨骼/morph 第 29 轮已支持 |
| 尺寸 | 参考图尺寸各异（150×80 到 1870×1033），本预览固定 1280×720 |

**结论**：自动指标只做**筛查/趋势**，不做 pass/fail；每个模型的验收 = **人工看 preview + 参考图**并记录差异归因。

### 7.2 自动指标定义（`compare.json`，前景掩码口径）

对比先在**前景掩码**上做（避免背景/机位差异主导统计）：

- **参考图**：有 alpha 且**全透明像素 >2%** → 用 alpha>0.5 当主体（官方透明背景图）；否则取**图像四边框像素的中值**
  当背景色，像素与背景色最大通道差 >0.06 的视为前景（`mask_kind="border-bg"`）；
- **预览图**：**永远不用 alpha**（`allow_alpha=False`）——`--shot` 的 PNG 虽有 alpha 通道，但那是 3D pass 写出的
  **材质 alpha**（OPAQUE 材质的 `baseColor.a` 也可能是 0），不是前景掩码；用 alpha 会把大片像素误判为背景。
- 前景占比 <0.5% 视为背景检测失败 → 退化为全图。
- **参考图格式/缺失**：上游参考图有 png/jpg/jpeg/webp/**gif**（gif 用 PIL 打开即第一帧，动画帧只取首帧）；
  149 个模型都有 `screenshot/`，148/148 全部生成对比（86 个用 large 版、13 个仅 GIF）。若将来出现上游
  无 `screenshot/` 的资产，其指标是**统计缺失**，不是"差异大"。

统计（亮度 Rec.709，0–1；RGB 0–255）：

| 指标 | 定义 | 判读 |
|---|---|---|
| `ref_mean_lum` / `preview_mean_lum` / `mean_lum_delta` | **前景**平均亮度及其差 | 系统偏差（光照/曝光/缺失 IBL） |
| `ref_mean_rgb` / `preview_mean_rgb` / `rgb_delta` | 前景平均 RGB 与两者平均通道距离（0–1） | 色偏/材质主色丢失 |
| `hist_similarity` | 两图前景掩码**交集**上亮度的 16 桶归一化直方图交集（0–1；交集 <64 px 时退化到整图） | >0.6 较接近、>0.4 中等、否则差异较大 |
| `ref_coverage` / `preview_coverage` / `coverage_ratio` | 前景像素占比；`coverage_ratio = preview/ref` | **构图/取景**是否合理（主体太小/太大） |
| `ref_bbox` / `preview_bbox` | 前景包围盒 `[x0,y0,x1,y1]` | 判断贴边/裁剪 |
| `framing_notes[]` | 构图告警：`preview_coverage < 5%` → "机位过远"；bbox 贴边（距边 ≤2px）→ "被裁剪/机位过近" | 直接对应 §6.4 ① 的修法 |
| `verdict` | 脚本初判（`hist_similarity` + `mean_lum_delta` + framing_notes 拼接；阈值 0.6/0.4、亮度差 0.2） | **只作筛查**，不是结论 |
| `reference_size` / `reference_has_alpha` / `preview_size` | 参考图尺寸/是否透明背景；预览固定 1280×720 | 判断可比性 |

> 早期版本还有 `edge_density`/`colorfulness` 等指标（基于整图、对背景/构图更敏感），当前流程已改为
> "前景掩码 + 覆盖率 + 构图告警"；旧 `compare.json` 若含这些字段，按旧口径理解即可（重跑会自动刷新）。

**指标 caveat（判读前必读）**：

1. **透明背景**：参考图"有 alpha 且全透明像素 >2%"才用 alpha 取主体（避免把普通 alpha 通道误当透明背景）；
   若参考图透明背景而**预览**是深灰背景（本引擎默认），前景统计仍可比（背景不计入），但
   **构图**（主体在画面中的占比/位置）不同会让 `coverage_ratio` 偏移。
2. **参考图尺寸差异大**（150×80 到 1870×1033）：直方图/覆盖率在统一缩放到 256×256 后再算，
   小图会被放大、细节不可比 —— 指标只做量级筛查。
3. **机位/背景不同**：任何构图差异都会影响 `coverage_ratio`/`hist_similarity`；先看 preview 完整性，
   再看指标，最后人工并排看。
4. 阈值是**经验值**（`migrate_gltf_models.py::compare_images`），不同批次之间只做相对比较。
5. 参考图是**动画帧**（角色行走/姿态）时，姿势差异会主导全部指标 → 只记录，不作结论。

### 7.3 判读流程（推荐）

1. **先看 preview 有没有内容**（不是全黑/全白/只有立方体）→ 否则按 §9 排查导入/加载；
2. 与官方参考图**并排**看：模型形状/主色/贴图是否存在、材质类型（金属/粗糙/透光）是否合理；
3. 读 `compare.json`/README 指标，先看**构图类**（`preview_coverage<5%`、bbox 贴边、`coverage_ratio` 偏离 1 很多），
   再看**颜色/亮度类**（`rgb_delta` 大、`mean_lum_delta` 大、`hist_similarity` 低）；
4. 对异常逐一**归因**：是缺失特性（transmission/IBL/动画…）、机位问题（手调），还是导入/渲染 bug（贴图丢/uv 错/mesh=-1）；
5. 把结论写进该模型 README 的**手动笔记区**（重生成保留）。

### 7.4 差异归因清单（按现象查原因）

| 差异现象 | 常见原因 | 处置 |
|---|---|---|
| 整体偏暗/偏亮 | 无 HDRI（analytic IBL 偏弱）、背景深色、参考图曝光不同 | 判为缺失特性；可手调 `env_*`/`clear_color` 后再比 |
| 高光/反射平淡，金属像塑料 | 无 HDRI/PMREM、默认无 MSAA；扩展贴图槽已接（clearcoat/sheen/iridescence/anisotropy） | 缺失特性，记录；HDRI opt-in 已有 |
| 玻璃发白/不透/内部不可见 | `KHR_materials_transmission` 未实现 | 缺失特性；GLB 变体无帮助；手调 alpha BLEND 近似 |
| 布料/车漆纹理"巨大/重复方向错" | `KHR_texture_transform` 逐槽 UV 不同 / UV 集缺失 | 记录；scale/offset/rotation 已支持（ToyCar 已修） |
| 某张贴图完全没出现 | KTX2/WebP 贴图被跳过、贴图槽不支持、`TEXCOORD_1` 贴错 | 看 `textures/` 是否缺图、README「已知差距」 |
| 角色姿势与参考不同 | 动画已播放（预览固定为第 1 帧 ≈0.017s 的采样姿态），参考图可能是动画的其它帧/状态；或机位不同 | 第 29 轮起动画/骨骼/morph 已导入；用 `--shot-seq --shot-frames 1,30,60` 看多帧确认在动；`MAQUE_ANIM=0` 对照 bind pose |
| 参考图是动画帧 | 上述原因（姿态时间点不同） | 记录"参考为动画帧"，用多帧截图人工判读 |
| 模型只有一部分/缺件 | 非三角 primitive 被跳过、缺 mesh/节点、导入 warning | 看 `issues`/导入日志 |
| 模型看起来像灰立方体 | `mesh=-1`（路径/JSON/VFS 根错）→ 回退内建 cube | 按 §9.2 修 |
| 构图/大小/朝向不同 | 自动机位 vs 官方机位（glTF 相机被忽略） | 手调相机（§6.4）；看 `coverage_ratio`/`framing_notes` 量化 |
| preview 主体过小（<5%） | 自动机位偏远（薄板/长条/超小模型） | 先看 `framing_notes`，再按 §6.4 ① 拉近/调 fov |
| preview 主体贴边/被裁 | 自动机位偏近（极端尺度/旋转 AABB 包络偏小） | 按 §6.4 ① 拉远或用 §6.2 公式重算 |
| 边缘锯齿明显 | 默认无 MSAA（可选 `msaa_samples=4`） | 批量预览为保持基线与性能默认关；需要时按场景开 |
| 背景/地面不同 | 官方有 backdrop/地面/接触阴影 | 缺失/未生成；可选手加（§6.4 ⑤） |
| 阴影缺失/方向不同 | 无地面、方向光固定、阴影仅可见 mesh | 缺失特性/场景差异；手加地面或调灯 |
| 日志有 `[wgpu] error` | 管线/绑定校验失败（多为资产/材质异常） | 查 `build-win/audit/migrate-logs/<Model>.render.log`；记 issue |

---

## 8. 踩坑清单（现象 → 原因 → 解决/规避）

### 8.1 环境与工具链

| # | 现象 | 原因 | 解决/规避 |
|---|---|---|---|
| P1 | shell 下载失败：`SEC_E_NO_CREDENTIALS`、`connection reset`、TLS 握手错误 | 本机 shell 走 schannel/wolfSSL，出网被阻断；**Node/Python 的 OpenSSL TLS 正常** | 用 `node .deps/download-gltf-assets.js ...` 下载 codeload zip；Python `urllib` 亦可（§3.1） |
| P2 | `.deps/gltf-assets` 解包后 149 模型 1.34 GiB、zip 1.09 GiB，磁盘紧张 | 全部变体（含 Draco/KTX2/WebP/Quantized/screenshot）都解出来了 | 只解 `Models/` 或迁移后保留 `.deps`（不入库）；至少留 ~4 GiB 磁盘 |
| P3 | 受限沙箱里 Git Bash/MSYS 启动报 `NtCreateDirectoryObject ... 0xC0000022` | 沙箱阻止 MSYS 命名对象创建；**完整权限下正常** | 需要 ctest 的 e2e（依赖 Git Bash）时用完整权限会话；纯 Python 迁移不受影响 |
| P4 | `cmake --build build-win` 卡在 `Re-checking globbed directories` / `Re-running CMake` 不动 | ninja 的 console pool 与工具会话 stdio 交互（工具层读管道导致挂起） | 手动两步：`cmake -S . -B build-win` 重新配置 + `ninja -C build-win`；或配置时加 `-DCMAKE_SUPPRESS_REGENERATION=ON` |
| P5 | 运行时报 `Unable to find entry point 'fs_shadow_alpha'`（改 shader 后） | 内置 shader 是**配置期打包**进 `build-win/generated/builtin_shaders` 的；只 ninja 不会重打包 | 改 `assets/builtin/shaders/` 后必须**重新 configure**（`cmake -S . -B build-win`）再 `ninja -C build-win` |
| P6 | Python 批量脚本长时间无输出，看起来卡死 | stdout 经管道/重定向时**块缓冲** | 一律 `python -u`；或在脚本里 `print(..., flush=True)` |
| P7 | Windows 控制台 `Get-Content` 看中文文档/README 花屏 | 控制台代码页 GBK，文件本身是 UTF-8（用 `read` 工具/Python 读是正常的） | 用 `Get-Content -Encoding UTF8`、`read` 工具或 Python 验证；**写文件统一 `encoding='utf-8'`**（importer/driver 已如此） |
| P8 | 模型目录含空格（`Box With Spaces`）导致脚本/命令失败 | ① shell 未加引号；② **glTF 内部 URI 用百分号编码**（`glTF%20Logo%20With%20Spaces.png`），按原始字符串拼路径会 `FileNotFoundError` | ① Python 用 `os.path`（driver 已如此）、shell 路径加引号；② importer 已对 `uri` 做 `urllib.parse.unquote`（本会话实测 `Box With Spaces` 修复前 failed、修复后可迁移） |
| P9 | `python3` 弹 Microsoft Store 安装提示并挂住 | `WindowsApps\python3.exe` 是 0 字节占位程序 | 用 `python`（或 `py -3`）；e2e 脚本用 `maque_python.sh` 探测 |
| P10 | PowerShell 里多行 Python heredoc 不可用 | PowerShell 无 bash heredoc | 用单行 `python -c "..."`，或 Git Bash 的 `python - <<'PY'` |

### 8.2 导入器 / 资产 / 流水线

| # | 现象 | 原因 | 解决/规避 |
|---|---|---|---|
| I1 | 某模型贴图大片缺失、只剩因子色 | 贴图是 KTX2/BasisU（无解码器）或其它未接扩展贴图 | 换 `glTF/` 主变体；看 `manifest.json.issues`/README；无法根治（§11） |
| I2 | 模型明显缺件（线/点/三角带装饰） | importer 只导三角形，非三角 primitive 跳过 | 已知限制；记录在 README |
| I3 | 手调 `.scn` 被覆盖 | `gltf_import.py --scene` **无条件写 `<out>/<name>.scn`**；`migrate --force` 会重写它 | 手调模型加入 `--exclude`；刷新素材用不带 `--scene` 的导入（§3.4）；改动及时 `git commit` |
| I4 | 想只重算指标/重写笔记，不想再渲染 148 次 | 每个模型渲染要 0.5–20s，全量重渲耗时 | `--reuse-preview`（复用 `preview.png` 只重算对比+笔记，秒级） |
| I4b | 重跑后 README 手动笔记还在吗 | `--force` 只删派生资产（assets/textures/scn/preview/compare），**不动 README.md** | 手动笔记安全；仍建议提交（git 兜底） |
| I5 | 同一 glTF 两次导入数值不同 | 混用了 `--compact` 与默认输出（6 位小数舍入差异） | 流水线固定 `--compact`（§4.7） |
| I6 | 大模型导入/加载慢、`assets/*.mesh.json` 几十 MB | 文本 JSON 体量大（compact ≈53 B/顶点，默认 indent=2 ≈119 B/顶点） | 固定 `--compact`；单模型超 50 万顶点会在 `issues` 标记；长期需二进制网格格式 |
| I7 | 参考图对比没生成（无 `compare.json`） | 缺 PIL/numpy，或模型无 `screenshot/` | `pip install pillow numpy`；无参考图的模型正常跳过 |
| I8 | 模型状态 `imported` 而非 `ok` | `preview.jpg` 生成失败（典型是缺 PIL） | 装 PIL 后重跑 `--only <Model>` |
| I9 | 中断后重跑，manifest 记录变少/不全 | 全量重跑会重建 records；`--skip-existing` 只写本次处理子集 | 中断恢复：全量 `--force` 重跑，或按 `--only a,b` 分批（会合并旧 records） |
| I10 | 归一化/量化网格几何异常 | `KHR_mesh_quantization`/`normalized` 未特殊处理 | 优先非量化变体；记 gap |
| I11 | 镜像件（负缩放）面朝向/法线不对 | 世界矩阵分解为 TRS 时**镜像符号丢失** | 已知限制；手改 `.scn` scale（负值）与材质 `side` |
| I12 | 中文/emoji 在生成文件里乱码 | 写入时未指定 UTF-8 | 脚本已统一 `encoding='utf-8', newline='\n'`；不要用系统默认编码写文件 |
| I13 | 某个模型 `failed`：`FileNotFoundError: ...glTF%20Logo%20With%20Spaces.png` | glTF 的 `image.uri`/`buffer.uri` 是 **URL 百分号编码**；importer 旧版直接当文件名用 | 已修：import 时对 uri 做 `urllib.parse.unquote`；重跑该模型即可 |
| I14 | 参考图是 `.gif`/动画截图，导致"无对比/指标缺失" | 旧版 `find_reference_image()` 只认 png/jpg/webp → 13 个只有 `.gif` 的模型没有对比 | 已修：参考图选择支持 `.gif`（PIL 打开即第一帧）；另有 4 个模型 GIF 只作补充图、优先匹配 `screenshot.*` 主图；**148/148 均有对比**。若将来遇到上游无 `screenshot/` 的模型，笔记标注"上游无参考图" |

### 8.3 渲染 / 截图 / 诊断

| # | 现象 | 原因 | 解决/规避 |
|---|---|---|---|
| R1 | preview 全黑/只有背景色 | `meshes=0`（没导入/实体 0）、`mesh=-1`（加载失败回退 cube 但相机没拍到）、相机 near/far 裁掉、模型尺度 1e-4/1e4 | 按 §9.1 逐项排查 |
| R2 | preview 里出现一个灰立方体 | `mesh_path` 加载失败 → 回退内建 cube；或场景没写 `mesh_path` | `MAQUE_RENDER3D_INFO=1` 看 `mesh=-1`；修路径/JSON |
| R3 | 模型超大顶满/被裁 | 自动机位对极端尺度/长条模型不适配 | 手调相机（§6.4） |
| R4 | 截图报错/退出码非 0 | 场景加载失败（JSON 错）、`--shot` 路径不可写、GPU 初始化失败 | 看 stderr 首行；用绝对路径；确认 `MAQUE_PRESENT=off` |
| R5 | 渲染很慢/批量卡住 | 大模型（数十万~百万顶点）加载/渲染慢；单模型 render 超时 900s、import 超时 1200s | 看 `manifest.render_seconds/import_seconds`；大模型可先 `--no-render` |
| R6 | 两次截图不一致 | 跨 GPU/驱动/构建，或场景含动画/随机 | 同机同构建确定性成立（§4.7）；迁移只渲第 1 帧 |
| R7 | 阴影缺失/像没开 | 自动场景无地面、阴影只对可见 mesh 生效；或 `MAQUE_SHADOW=0` | 手加地面（§6.4 ⑤）或接受缺失 |
| R8 | `MAQUE_RENDER3D_INFO` 没输出 | 一般运行时不打印，需要显式环境变量 | 截图/运行命令前 `$env:MAQUE_RENDER3D_INFO='1'` |

### 8.4 盘点/记录/对比

| # | 现象 | 原因 | 解决/规避 |
|---|---|---|---|
| C1 | 自动对比结论"差异较大"但模型看起来正常 | 官方图离线渲染/透明背景/不同机位/缺 IBL | 按 §7.1/§7.4 归因，不以自动 verdict 判失败 |
| C2 | 参考图为透明背景 | 透明像素在比较空间变黑 | 记录 `reference_has_alpha=true`；人工判读 |
| C3 | 每个模型 README 手动笔记被清空 | 删除/改动了 `<!-- 手动笔记开始（重生成时保留以下内容） -->` 标记行之外的结构 | 只改标记行**之后**的内容；标记行本身不要动 |
| C4 | 总索引手改后消失 | `scenes/khronos/README.md` 与 `manifest.json` 每次批量重写 | 想补充信息写进 `docs/` 或模型 README 手动笔记区 |
| C5 | 想统计某类模型/差距 | 手工翻 README | 用 §10.5 的一行命令读 `manifest.json` |
| C6 | `manifest.total_seconds` 只有十几秒 / `render_seconds` 全是 0 / 没有 `import_seconds` | 批量后又跑过 `--reuse-preview`（复用预览刷新对比/笔记），它重写了 manifest | 真实批量耗时看批量日志（`migrate-batch2.log` 末尾 `migrate: ... (472.3s)`）；要保留计时就别在批量后直接 reuse |
| C7 | 总索引写"失败/异常 1"，但 `manifest` 里 148 个记录全是 ok | 索引把"手调模型补行"（`ok (hand-tuned)`，如 ToyCar）计入了 failed 统计（脚本的统计口径小 bug） | 以 `manifest.records` 的状态为准；索引里 149 行 = 148 自动 + 1 手调 |
| C8 | 把"**无参考图**"的模型当成"迁移差异大"去归因 | 当前 149 个模型都有 `screenshot/`、148/148 有对比；未来若上游出现无参考图模型，其指标是**统计缺失**（不是渲染差异） | 先看该模型 `compare`/README 是否有参考图；无参考图的只记录"无参考图"，**不要与"有参考但差异大"混为一谈** |
| C9 | 对比结果"覆盖率/构图"明显不对（如 OPAQUE 模型被判"主体贴边"） | 曾把**预览 PNG 的 alpha** 当前景掩码——它是 3D pass 写出的材质 alpha（OPAQUE 材质的 `baseColor.a` 也可能 0），不是前景 | 已修：预览一律用边框背景色估计掩码（`allow_alpha=False`）；参考图才有条件用 alpha（全透明像素 >2%）。见 §7.2 |

### 8.5 迁移过程中发现并修复的工具/导入器缺陷（经验沉淀）

迁移本身是"用真实资产压测工具链"的过程。以下是本会话中被 149 个模型**实际暴露**、随后修复的缺陷
（记录在此，避免以后重犯；修复都在 `tools/`，属迁移工具链，不在引擎运行时）：

| 缺陷 | 暴露方式 | 修复 |
|---|---|---|
| JPEG 贴图被 importer 整张跳过 | 大量模型贴图全丢（DamagedHelmet 等）；引擎 image 模块其实支持 JPEG | `image_encoding()` 放行 jpeg（审计 I1 修复） |
| `image.uri` 百分号编码未解码 | `Box With Spaces` 导入 `FileNotFoundError: glTF%20Logo...png`（BATCH1 唯一 failed） | `urllib.parse.unquote(uri)`（buffer 与 image 都处理） |
| 自动机位对 1 单位立方体顶满画面 | 初版 `-1.2r/+0.55r/+2.1r` 构图普遍过近 | 先改"包围球/tan(fov/2)×1.35"；再改**包围盒 8 角点视锥拟合**（§6.2，兼顾薄板/长条） |
| 整图对比指标鉴别力不足 | BATCH1 中 128/137 个模型都被判"差异较大"（背景/构图主导），`hist_similarity` 中位数仅 0.11 | 改为**前景掩码**口径 + 覆盖率 + 构图告警（§7.2） |
| 用**预览 PNG 的 alpha** 当"前景掩码" → 覆盖率/构图误判 | `CompareAlphaCoverage` 等 OPAQUE 材质 `baseColor.a=0` 的模型被误报"主体贴边/裁剪" | 预览掩码改为 `allow_alpha=False`（只按边框背景估计）；参考图仍用 alpha（全透明像素 >2% 时） |
| `--force` 曾 rmtree 整个模型目录 | 手调 scn 和 README 手动笔记有丢失风险 | 只删派生资产（assets/textures/scn/preview/compare），**保留 README** |
| 渲染异常只能靠人翻日志 | `meshes=0`/`mesh=-1`/wgpu 错误容易被漏看 | 渲染日志逐模型落盘 + 自动检测写入 `issues`（§4.4） |
| 重算对比/笔记要重渲染全部模型 | 改指标后刷新成本高 | 新增 `--reuse-preview`（复用 preview.png，秒级刷新） |
| **欧拉分解万向锁**：`_decompose` 在 `|ry|≈90°` 时用噪声比值 atan2，TRS 往返误差 1.7 → 整车翻倒/裁剪 | QA+视觉复核发现 `CesiumMilkTruck` 车身朝向错、底部裁剪（`Yup2Zup` 根节点 β=±90°） | 万向锁时固定 `rz=0`、用 `r01/r11` 求 `rx`；并按 det<0 取负 `sx` 保住镜像（`NegativeScaleTest` 修复） |
| **贴图输出重名串图**：`image.name` 相同/缺失时互相覆盖 | `PlaysetLightTest` 12 图只剩 4 名（2/3 材质错）、`TextureTransformMultiTest`、`TransmissionRoughnessTest` | `name_registry` 去重（同名追加 `_<src>`）；并去掉 `image.name` 自带扩展名避免 `x.jpg.jpg` |
| **EXT_texture_webp / KHR_texture_basisu 的 source 在 texture.extensions 里** | `SheenWoodLeatherSofa` 全白（tex.get("source") 为 None） | 从 `EXT_texture_webp`/`KHR_texture_basisu`/`EXT_texture_avif` 取 source；WebP 用 PIL 转 PNG |
| **未登记扩展 gap**：`KHR_node_visibility`/`EXT_mesh_gpu_instancing`/`KHR_materials_retroreflection`/`EXT_lights_image_based`/`KHR_animation_pointer` | QA 指出模型"已知差距"清单缺项 | `known_gaps()` 补齐并进各模型 README |
| **COLOR_0 / TEXCOORD_1 未导入**（顶点布局只有 48B） | 顶点着色模型变纯材质色；第二套 UV 贴图错位（报告 P1） | 新增**顶点布局 v1（56B）**：color4+uv1；gfx 按 layout 懒建管线；材质 `uv1_mask` 逐槽选 UV |
| **glTF sampler wrap/filter 未导入**（运行时硬编码 REPEAT） | `TextureSettingsTest` clamp/mirror 行失败（报告 P1） | 贴图路径 `#wrap=...&filter=...` 后缀 + runtime per-texture sampler 解析 |
| **unlit 材质被 ACES 色调映射** | 参考图 unlit 色块橙→偏黄/蓝→偏青（报告 P1） | `fs_basic` 改 `output_direct`（unlit 直接输出 baseColor） |
| **transmission 全不透明** | 32 个模型玻璃/透明件与参考差异大（报告 P1） | importer 近似 alpha BLEND + 阴影 pass 跳过 BLEND caster（并新增 `vs_shadow1` 支持布局 v1 的 alpha mask 投影） |
| **自动机位对室内/特写资产不可用** | Sponza 官方参考为主厅内部，自动包围盒拟合只能看到建筑外壳（报告视觉复核"部分可接受"） | `MANUAL_CAMERA` 表 + 世界尺度手动机位（Sponza 室内沿 -X 看）；importer 增加相机 CLI |
| **盲目按参考图覆盖率自动取景会裁切** | 尝试 `--auto-framing`（按 coverage_ratio 缩放距离）→ 134 模型中 132 个出现"主体贴边/裁剪"，hist 有升有降 | **不默认启用**；保留为实验工具（`--auto-framing --dry-run`），迁移报告明确"覆盖率差异属参考图为特写/构图不同，非渲染缺陷" |
| **KHR 扩展材质贴图槽全部丢弃** | clearcoat/sheen/iridescence/anisotropy 贴图缺失，观感只剩因子（报告"部分可接受"主因之一） | ABI 尾部追加 `textures3[7]`（material 268B）+ schema 7 路径字段 + gfx group1 binding 10..16（15 sampled tex + shadow depth = 16 上限）+ shader `surface_ext_params`（R/G/A 通道按 glTF 语义）；缺失槽绑 white 且 flags2 presence 位关闭 |
| **`KHR_lights_punctual` 被忽略** | 12 个模型自带灯不还原（DirectionalLight/LightsPunctualLamp 等） | importer 把灯节点转成 `render.Light`（≤8、方向光优先、spot 锥角/range、强度观感归一化、无灯才加太阳） |
| **`KHR_node_visibility` 未实现** | 可见性测试模型隐藏节点仍渲染/叠影 | importer 跳过 `visible=false` 节点及整棵子树（网格+灯光） |
| **e2e grep 中文日志在 Windows 失败** | `gltf_import_e2e` 新加的灯光告警断言 `grep "KHR_lights_punctual 共 9 盏 > 8"` 失败：Python 重定向 stderr 用 locale(GBK) 编码，脚本里的 UTF-8 中文 pattern 匹配不上 | e2e 脚本前部 `export PYTHONIOENCODING=utf-8`；告警断言改 ASCII 正则 `grep -Eq "KHR_lights_punctual.*> 8"`；CI/Windows 上任何"中文日志 grep"都要注意编码 |
| **`KHR_texture_transform.rotation` 忽略** | 14 个模型的旋转 UV 错（面料/车漆纹理方向） | importer 导出 `uv_rotation`/`normal_uv_rotation`（弧度）；shader 按 T·R·S 旋转 UV（旋转角复用 Draw 的 specular.w/sheen_color.w，避免扩 uniform） |
| **`EXT_mesh_gpu_instancing` 不展开** | SimpleInstancing 只显示 1 份实例 | importer 在场景生成期按实例 TRS 展开为多实体（上限 4096） |
| **volume/diffuse_transmission 无观感** | 有色玻璃/乳浊材质与参考差异大 | volume：`base_color ×= attenuationColor`；diffuse transmission：alpha BLEND 近似（阈值/常量见 importer 注释） |
| **transmission 近似把 MASK 改成 BLEND 后丢了 alpha 镂空** | `StainedGlassLamp` 彩绘玻璃/`GlassBrokenWindow` 裂纹/`TransmissionTest` Mask 行：整片玻璃变均匀半透明，镂空图案消失 | importer 记录原 alphaMode，改 BLEND 时置 `alpha_mask_in_blend`；runtime 映射 flags2 bit19；shader 在混合管线里按**未乘 opacity** 的 alpha 做 cutoff discard（20 个材质/5 个模型重导） |
| **玻璃近似太"幽灵"（无边缘反光/单层无厚度）** | CompareTransmission/GlassVaseFlowers/ABeautifulGame 等玻璃中心过透、边缘与背景融合 | 材质新增 `transmission_factor`；shader 按 Fresnel 提高掠射角 alpha；gfx 对 transmission 近似材质画两遍（cull front 背面 → cull back 正面）；37 个 transmission 相关模型重导重渲 |
| **envmap/IBL 完全没有** | 金属/车漆/玻璃反射平淡（报告最大观感差距）；`texture_desc.kind=1/2` 直接 -3，`sky_mode` 2/3 预留，shader 只有 analytic env | 实现 **equirect envmap IBL（opt-in）**：runtime 内建 `builtin:/textures/studio_env`（512×256 sRGB + mip 链，程序化 studio 软光箱）+ 场景 `env_texture`（PNG/JPEG，VFS/远程）；gfx group0 新增 env 纹理/采样器绑定（请求适配器更高 sampled texture 上限）；shader 镜面反射按方向采样、roughness→mip（漫反射保留 analytic）；ToyCar 样板与 `scenes/pbr/env_ibl.scn` 已启用；`MAQUE_ENV_IBL=1` 强制内建 env。默认关闭的原因：部分测试图（如纯色纹理/图表）参考图是平光渲染，env 反射会显著改变直方图指标；对金属/玻璃等真实资产观感更好 |
| **envmap 只有 LDR 近似、没有 HDR/天空背景** | PNG env 软光箱被截断、背景仍是 clear_color，金属高光不够亮 | ①Radiance `.hdr`（RGBE）解析 → **RGBA16F** 纹理（gfx create_texture2 支持 pixel_format=2/8B 每像素）+ 线性 mip（PMREM 近似，经度 wrap）；②`scenes/pbr/env/studio.hdr`（128×64 HDR，`tools/make_env_hdr.py` 可复算）+ `env_ibl.scn` 引用；③**天空背景 pass**：camera uniform 增加 `inv_vp`（896B），全屏三角形按射线采样 env（独立 sky pipeline，不写深度）；ToyCar/示例场景启用；EXR 暂不支持（会打印并回退 analytic） |
| **env IBL 只有 box-filter mip + analytic 漫反射** | 中高 roughness 反射能量分布不对（box 平均不是 GGX 波瓣）；漫反射完全不来自 env → 哑光物体在 HDRI 下仍是 studio 常数色 | runtime 装配期 CPU 实现真 PMREM：specular = **GGX importance sampling**（Hammersley，V=N=R，NdotL 加权归一，32→128 样本/texel）生成粗糙度 mip 链；diffuse = **余弦加权半球卷积**（64×32，256 样本）生成 irradiance；两者 RGBA16F。gfx `environment.env_irradiance_texture` + group0 binding5；shader `env_irradiance` 有 env 时查表、无 env 时保持 analytic（148 基线不变）。e2e ⑩ 用 `env_pmrem.scn` 断言天空/镜面锐利度/哑光 env 差异 |
| **`specularColorFactor` 被 clamp 成 1、specular 贴图丢弃** | `CompareSpecular` 用 10× specularColorFactor + specularColorTexture 做"有无 specular"对照；旧实现 clamp 后两球几乎一样，自动指标反而更高（把缺陷当基准） | ①schema/ABI 新增 `specular_texture/specular_color_texture` + `textures4[2]`，importer 映射两槽；②shader 仅调制电介质 F0（金属 F0=baseColor 不受影响），specularColorFactor **不设上限**（只 clamp ≥0）；③每 stage sampled texture 增至 20，依赖适配器上限（桌面 GPU 通常 ≥128；gfx 创建 device 时向适配器请求上限） |
| **MSAA 帧崩溃（pipeline/pass 样本数不匹配）** | 4x 附件建好后，材质/天空管线仍是 `multisample.count=1` → `wgpuCommandEncoderFinish` 验证失败、进程 panic 无截图 | 管线缓存加 `[msaa4]` 维（`p3dMatPipeline[2][...]`、`skyPipeline[2]`），`rp.multisample.count=4`；`WGPURenderPassColorAttachment.resolveTarget` 指向真实目标、深度附件也换成 4x；`caps.max_samples` 先探测 4x 纹理是否可建，失败回退 1x（不静默）。e2e ⑫ 用 1x/4x 像素差断言 |
| **EXR 解码出天文数字/负值** | 直接 `zlib.uncompress` 后按"行内通道连续"读 float，得到 `-1e17` 量级垃圾；偶发个别像素对但整体错位 | ①OpenEXR ZIP/RLE 在压缩前做了 **predictor（首字节原样，其后 `b[i]-b[i-1]+128`）+ 字节交错**，解压后必须逆变换；②scanline 内像素布局是「本行 B 整行 → G 整行 → R 整行」，通道偏移要乘 `width`，不是逐像素交错；③用 `tools/make_env_hdr.py` 纯 Python 写 FLOAT/ZIP + HALF/ZIPS fixture，e2e ⑬ 以 `.hdr` 渲染为基准断言逐像素一致（max_diff=0） |

---

## 9. 排查手册（症状 → 检查项 → 修复）

### 9.1 preview 全黑 / 为空 / 不像模型

逐项检查（每条都给出确认命令或日志特征）：

1. **资产是否导入**：`scenes/khronos/<Model>/assets/*.mesh.json` 是否存在、是否非 0 字节；
   `textures/` 是否为空（全空常见于 KTX2 模型；WebP 现已在导入期转 PNG）。
2. **场景实体数**：运行日志应打印
   `runtime: 3d scene cams=1 meshes=N lights=1 cube_mesh=0`——`meshes` 是 MeshRenderer 组件数，
   **N=0 = 场景里没有网格实体**（导入失败/`.scn` 被覆盖成空/只有相机）。
3. **网格是否加载成功**：`MAQUE_RENDER3D_INFO=1` 时逐节点打印
   `runtime: mesh renderer node=<id> path='assets/xxx.mesh.json' → mesh=<句柄>`；**`mesh=-1` = 加载失败**。
   失败明细（stderr）：`runtime: mesh not found '<path>'`（路径错/文件缺）或 `runtime: mesh json 解析失败 '<path>'`（JSON 坏）。
   注意 **VFS 根 = 场景目录**：`mesh_path` 必须是相对场景目录的路径（`assets/x.mesh.json`），不能写 `scenes/khronos/...`。
4. **相机 near/far 是否裁掉模型**：`near` 太大（模型比 near 近）或 `far` 太小 → 全黑/部分缺失；
   对照 `preview` 与自动公式（§6.2）重算，或先临时 `near=0.001, far=10000` 验证。
5. **模型尺度**：1e-4 尺度（ToyCar 类）或 1e4 尺度会让默认 near/far/fov 取景失败；
   按 §6.4 ① 调相机，必要时按包围盒重算 `dist/near/far`。
6. **材质全黑**：检查 `asset.PbrMaterial.base_color` 是否 (0,0,0)、贴图是否 sRGB/线性槽错放
   （albedo/emissive 走 sRGB，MR/AO/normal 走线性——运行时已按槽选择，手写别乱指）、
   法线贴图是否被误给成立方图。
7. **背景正常但模型在画面外**：相机 rotation 朝向错了（Euler 顺序/符号）；直接抄相近模型 camera。

### 9.2 贴图/材质异常

| 症状 | 检查 | 修复 |
|---|---|---|
| 贴图缺失（README 有 gap） | `textures/` 是否有该图；`issues` 是否提"跳过不支持的贴图"；源变体是否 KTX2（WebP 已支持转码，需 Pillow） | 换 `glTF/` 主变体；无法根治的记 gap |
| 贴图存在但没生效 | `.scn` 材质字段名/路径拼写；路径必须相对场景目录；文件是否真 PNG/JPEG | 修 scn 字段/路径 |
| 颜色偏暗/偏亮 | 手写贴图槽是否把 MR/AO/normal 放到 sRGB 槽；`env_intensity`/曝光 | 按运行时约定放槽 |
| 面料/车漆纹理巨大 | `uv_scale/uv_offset/uv_rotation` 字段缺失/为默认 | 检查 `.scn` 材质三字段；rotation 走 `uv_rotation` |
| 双面材质背面变黑 | 手写 scn 没写 `side=2`，或 `doubleSided` 语义未落到 `side` | 显式 `"side": 2` |
| 法线细节"反了" | 审计已修切线基；若手写材质把 normal 槽指到错误贴图 | 检查 `normal_texture` 指向 |

### 9.3 批量/工具异常

| 症状 | 检查 | 修复 |
|---|---|---|
| `migrate: ... failed` 某些模型 | `manifest.json` 对应 `records[].issues/import_log_tail/render_log_tail`；完整日志 `build-win/audit/migrate-logs/<Model>.render.log` | 修路径/换变体/记 blocked |
| 某模型 preview 可疑（全黑/近纯色/主体极小/贴边） | 该模型 `manifest.records[].issues` 是否有 `meshes=0`/`mesh=-1`/`[wgpu] error`/覆盖率告警；对照 `compare.json` 的 `preview_coverage`/`framing_notes` | 按 §9.1/§9.2 修；机位问题手调（§6.4） |
| 单个模型 importer rc≠0 | 末尾日志（`import_log_tail`）；常见：非三角、无 scenes、坏 buffer、资源缺失 | 该模型记 blocked/failed，不中断整批（driver 已保证） |
| 批量中途崩溃 | 重跑；先看最后处理到的模型目录 | 全量 `--force` 重跑（幂等） |
| 索引/统计没更新 | 批量是否跑完（最后一行 `migrate: ...`） | 跑完才写 `README.md`/`manifest.json` |
| 改了对比指标想刷新全部笔记 | 需要重算 compare/README | 快速路径：`python -u tools/migrate_gltf_models.py --reuse-preview`（不重渲染） |
| `cmake --build` 卡住 | 见 §8.1 P4 | 手动 configure + ninja |
| 运行时报 shader 入口找不到 | 改过 shader 未重新 configure | §8.1 P5 |

### 9.4 对比/复核异常（参考图与指标）

| 症状 | 检查 | 修复 |
|---|---|---|
| `compare.json` 缺失 / README 写"未生成对比数据" | 该模型是否**没有精确参考图**（上游无 `screenshot/` 或参考图与主变体不对应） | 这是**统计缺失**，不是迁移失败：笔记标注"上游无参考图"；有 gif 参考图时重跑 `--reuse-preview`（PIL 取第一帧） |
| 参考图是动画 GIF | 只取第一帧，姿势可能与主变体静态姿势不同 | 记录"参考为动画帧"；不作姿势/差异结论 |
| 覆盖率/构图指标明显异常（如 OPAQUE 模型"贴边"） | 是否为旧版指标（用预览 alpha 当掩码） | 用当前工具重跑 `--reuse-preview` 刷新（预览 `allow_alpha=False`，§7.2） |
| `hist_similarity` 极低但模型看起来正常 | 参考图小/背景纯色/透明背景/机位不同（如 Box/Triangle 类简单模型） | 结合 `preview_coverage`/`rgb_delta` 与人工并排看，不要单看一个指标 |
| `verdict` 说"差异较大"但人工看正常 | 缺失特性（transmission/IBL/动画）+ 机位/背景差异 | 按 §7.4 归因，写进该模型 README 手动笔记区；不是迁移缺陷 |
| 有人把你手调的 `.scn` 覆盖了 | 是否跑过 `--force`/批量（手调模型没在 `--exclude` 里） | 从 git 恢复 `.scn`/备份；把该模型加入 `--exclude` |

---

## 10. 迁移结果统计（读 `manifest.json` 汇总）

> 本节数据来源：`scenes/khronos/manifest.json`（`tools/migrate_gltf_models.py` 生成）+ 批量日志
> `build-win/audit/migrate-batch2.log`（构建产物，不入库）。
> **本节是快照**：重跑批量（或 `--reuse-preview` 刷新对比/笔记）后数字会变，按 §10.5 的命令复算。
> §10.1 是"资产面静态统计"（与批次无关，来自 `plans/20 §4.1` + 本机实测），§10.2–10.4 是本批（BATCH2）结果。

### 10.1 资产面静态统计（主变体口径 + 全变体历史对照）

| 项 | 数值 |
|---|---|
| 模型目录数 | **149**（每个都有 `screenshot/` 参考图；其中 10 个只有 `.gif`） |
| 迁移处理的模型 | **148**（149 − 手调 `ToyCar`） |
| `glTF` 主变体文件 | 336 个（其中 1 个模型只有 GLB 变体） |
| 主变体资产体积 | **597.0 MB**（`Models/*/glTF`，1019 个文件） |
| 参考图体积 | 126.0 MB（`Models/*/screenshot`） |
| 解包后 `Models/` | 1,433,732,917 B（约 1.34 GiB，全部变体） |
| 148 个主变体的合计规模 | 节点 13,253、网格 11,900、primitive 12,164、顶点 **6,635,971**、三角形 **7,302,253**、材质 11,812、纹理 10,681、图像 697 |
| 贴图编码（主变体） | PNG **447**、JPEG **237**、WebP **13**（KTX2 0） |
| 贴图编码（全变体扫描） | PNG ~499、JPEG **482**、KTX2 81、WebP 26 |
| 扩展使用（主变体/模型数） | transmission 32、volume 25、diffuse_transmission 6、ior 17、texture_transform 14、clearcoat 12、lights_punctual 12、iridescence 10、specular 10、sheen 9、anisotropy 7、variants 7、unlit 6、emissive_strength 5、animation_pointer 5、dispersion 4、node_visibility 3、meshopt 1、quantization 1、webp 1、pbrSpecularGlossiness 1、retroreflection 1、mesh_gpu_instancing 1 |
| 几何/动画（主变体） | 动画 26 个模型（38 个动画）、skin 7 个模型（90 个 skin）、morph 4 个模型（6 个 primitive）、相机 13 个模型（38 个）、`COLOR_0` 44 个 primitive、`TEXCOORD_1` 141 个 primitive、`textureInfo texCoord>0` 49 处、非三角 22 个 primitive、缺 NORMAL 45 个 primitive |
| 扩展使用（全变体扫描，历史） | transmission 77、volume 55、texture_transform 39、ior 33、clearcoat 31、lights_punctual 24、iridescence 23、variants 22、specular 20、sheen 19、Draco 18、anisotropy 15、emissive_strength 15、unlit 12、quantization 10、animation_pointer 10、basisu 7、webp 5 |
| 几何/动画（全变体扫描，历史） | 动画 69、skin 24、morph 15、`COLOR_0` 78 primitive、`TEXCOORD_1` 691 属性引用（209 textureInfo）、非三角 28、缺 NORMAL 68 |
| alpha 材质（主变体） | OPAQUE **11,750**、MASK **47**、BLEND **15** |
| alpha 材质（全变体扫描，历史） | OPAQUE 23597、MASK 92、BLEND 30 |

### 10.2 本批迁移结果（`manifest.json` 汇总）

**数据快照**：2026-10-26 第 10 轮（texture_transform.rotation + EXT_mesh_gpu_instancing + diffuse/volume 近似后整批重新导入+渲染，见 `build-win/audit/batch6-round3.log`，整批 **544.3s ≈ 9.1 min**）。
**第 29 轮（动画/蒙皮/morph）**只对 27 个相关模型做了定向 `--only ... --force` 重跑（74.1s；其余 121 个模型的
`.scn`/preview/compare 未变），所以本表统计仍是第 27 轮快照；动画模型的最新指标看 `manifest.json` 的
`records[].compare` 与 `records[].animation`。
`scenes/khronos/manifest.json` 的 `total_seconds` 只是最后一次写 manifest 的耗时（不含全部渲染）——**真实批量耗时看批量日志**。

| 项 | 值 |
|---|---|
| 处理模型 | **148**（149 − 手调 `ToyCar`） |
| 成功 `ok` | **148（100%）**；`blocked` **0**；`failed` **0** |
| 整批耗时 | **544.3s**（单模型含导入+渲染+对比+笔记；平均 3.7s） |
| 最慢模型（墙钟） | `NodePerformanceTest` 74.3s、`ABeautifulGame` 31.4s、`MetalRoughSpheresNoTextures` 22.1s、`IridescenceMetallicSpheres` 18.9s、`Sponza` 15.2s、`CarConcept` 8.0s |
| 派生资产（本地、不入库） | `out_size_bytes` 合计 **974.1 MB**（第 9 轮重新导入；含 WebP 转码/去重贴图、布局 v1 顶点与扩展贴图）；文件系统实测约 1.0 GB |
| 入库部分 | **18.4 MB**（`.scn` 15.5 + `preview.jpg` 2.3 + `README.md` 0.5 + `compare.json` 0.1）+ `manifest.json`/总索引 0.4 MB |
| 产物数量 | `preview.jpg` **149** 张（含 ToyCar）、`compare.json` **148** 个、`README.md` 149 份 |
| 参考图口径 | 149 个模型目录都有 `screenshot/`；**148/148 全部生成对比**；其中 **86 个模型使用 large 版参考图**、**13 个模型的参考图是 `.gif`**（PIL 取第一帧）。判读原则：对照参考图前先确认它对应主变体；若是动画帧/过程图，只作参考、不作姿势结论（以该模型 README 为准） |
| 渲染异常自动检测 | **0**（无 `cams=0` / `meshes=0` / `mesh=-1` / `[wgpu] error`） |
| 构图告警 | 3 个：`AntiqueCamera`、`StainedGlassLamp`（细长/异形导致覆盖率偏低，属指标误报）、`Sponza`（手动机位为内部视角，前景占比自然偏低/部分贴边；人工确认构图与官方参考一致）。`CesiumMilkTruck` 翻倒/裁剪已由万向锁修复解决 |
| 自动对比初判（148 个） | 38 × "主体亮度/色调较接近"、43 × "中等差异"、67 × "差异较大"（"差异较大"主要来自缺失特性/机位/背景，需人工归因，**不是失败**） |
| 对比指标中位数 | `hist_similarity` 0.436（min 0.004 / max 0.981）、`mean_lum_delta` +0.078、`rgb_delta` 0.119、`preview_coverage` 0.166（min 0.043 / max 0.701）、`coverage_ratio` 0.289 |

> 迭代记录：**BATCH1** 147 ok / 1 failed（`Box With Spaces`：`image.uri` 百分号编码未解码）→ **BATCH2** 148 ok / 0 failed
> → 第 6 轮补 `.gif` 参考图/前景掩码口径 → **第 7 轮** 由 QA/视觉复核驱动修复并重导 13 个模型：
> 欧拉万向锁（`CesiumMilkTruck` 翻倒）、WebP 转码（`SheenWoodLeatherSofa` 全白）、贴图重名串图
> （`PlaysetLightTest`/`TextureTransformMultiTest`/`TransmissionRoughnessTest`）、负缩放镜像（`NegativeScaleTest`）、
> `EXT_texture_webp` 的 source 位置、贴图双扩展名等；并补登记 5 类未实现扩展。
> **第 8 轮** 按迁移报告修复引擎/导入器特性：顶点布局 v1（`COLOR_0`+`TEXCOORD_1`）、glTF sampler
> wrap/filter、unlit 去 ACES、transmission 近似 alpha BLEND（+BLEND 不投影、`vs_shadow1` 支持布局 v1 镂空投影）、
> Sponza 手动机位；整批重导+重渲 148 模型。数据用最终第 8 轮。
> **第 9 轮** 继续补 P1：KHR 扩展材质贴图槽（clearcoat/roughness/sheen/sheen roughness/iridescence/
> iridescence thickness/anisotropy 共 7 槽，ABI `textures3[7]` + shader 通道语义）、`KHR_lights_punctual`
> 导入为 Light（≤8 盏、强度观感归一化）、`KHR_node_visibility` 隐藏节点跳过；14 个灯光/可见性场景与
> 15 个扩展贴图模型预览更新。数据用最终第 9 轮。
> **第 10 轮（本快照）** 补最后一批可定位导入缺陷：`KHR_texture_transform.rotation`（基础组/法线组，
> 按 T·R·S 旋转 UV）、`EXT_mesh_gpu_instancing`（导入期展开实例，SimpleInstancing 125 实体）、
> `KHR_materials_diffuse_transmission`（alpha BLEND 近似）、`KHR_materials_volume`（薄壁 attenuationColor
> 色调近似）；21 个模型预览更新。数据用最终第 10 轮。
> **第 11 轮（渲染观感）** ①transmission 近似增强：Fresnel 视角 alpha + 双面玻璃两遍绘制（37 个模型）；
> ②envmap/IBL：equirect 环境贴图（PNG/JPEG、`builtin:/textures/studio_env`、`MAQUE_ENV_IBL=1`）、
> gfx group0 env 绑定、shader roughness→mip 镜面 IBL（漫反射保留 analytic）、`scenes/pbr/env_ibl.scn` 示例、
> ToyCar 样板启用 env。**默认关**（避免纯色/图表测试资产被反射改变直方图），需要时按场景/环境变量开启。
> 数据（148 模型基线）与第 10 轮一致；ToyCar 因 env 预览更新。
> **第 12 轮（HDR envmap + 天空）** Radiance `.hdr`（RGBE）→ RGBA16F 纹理 + 线性 mip 预过滤；天空背景全屏 pass
> （camera.inv_vp + sky pipeline）；`scenes/pbr/env/studio.hdr`（HDR 软光箱）与 `env_ibl.scn` 示例；ToyCar 样板
> env+天空预览更新。默认仍关（148 模型基线不变）。
> **第 13 轮（真 PMREM）** runtime 装配期 CPU 预计算：**GGX importance-sample 粗糙度 mip 预过滤**
>（128×64 `.hdr`/512×256 builtin → 8/10 级 RGBA16F，经度 wrap）+ **余弦卷积漫反射 irradiance**（64×32 RGBA16F）；
> gfx group0 新增 irradiance 纹理绑定（binding5）/`environment.env_irradiance_texture`；shader `env_irradiance`
> 改查 irradiance（无 env 时仍走 analytic，148 基线零回归）；新增 `scenes/pbr/env_pmrem.scn` 与 e2e ⑩
>（天空/镜面锐利度/漫反射 env 三项断言）。EXR 已支持（第 17 轮），cubemap kind=1 已支持（第 26 轮，见 §5.4）。
> **第 14 轮（specular 贴图）** `KHR_materials_specular` 两槽：specularTexture.A=specularFactor（线性）、
> specularColorTexture.RGB=specularColorFactor（sRGB）→ schema `specular_texture/specular_color_texture` +
> ABI `textures4[2]` + flags2 bit21/22 + group1 binding17/18；shader 仅调制**电介质 F0**（金属不受影响），
> specularColorFactor 允许 >1（CompareSpecular 用 10× 做有无 specular 对照）。3 个用到贴图的模型
>（CompareSpecular / SpecularTest / AnimationPointerUVs）重导重渲；`gltf_import_e2e` 夹具扩 2 槽断言。
> **第 15 轮（MSAA 4x）** `frame3d.msaa_samples` + `scn.Scene.msaa_samples`（0/1=关、4=4x，`MAQUE_MSAA` 覆盖）；
> gfx 新增 4x 颜色/深度附件 + `resolveTarget`，材质/天空管线按 sampleCount 分两份缓存（否则
> pipeline/pass 样本数不匹配会验证失败），`caps.max_samples` 探测设备能力并自动回退；默认关 → 148 基线
> 零回归，e2e ⑫ 断言 1x/4x 仅边缘像素变化（1095 px、max 290）且覆盖不变。
> **第 16 轮（TEXCOORD_2+ / 逐槽 UV / GPU 实例化评估）** 三项均为 P3，实测后决定不做：
> ①TEXCOORD_2 唯一使用者 MosquitoInAmber 的该属性未被任何材质贴图引用（无视觉影响）；
> ②同 UV 集但逐贴图 transform 不同的材质实测 10 模型（最多 AnimationPointerUVs 69 处，且是 pointer 动画），
> 需 9 槽×3 float 的 uniform 扩展 → 成本大于收益；③GPU 实例化仅 SimpleInstancing 125 实例，
> CPU 展开 draw 开销可忽略。结论与证据写入 §5.4/§11。
> **第 17 轮（EXR envmap）** runtime 新增 OpenEXR scanline 解码：NONE/RLE/ZIP/ZIPS，HALF/FLOAT/UINT 通道，
> R/G/B 或 Y（灰度）；解压后必须做 OpenEXR 的 **predictor + 字节交错逆变换**（否则得到量级离谱的浮点垃圾），
> 扫描线内通道按**整行**排列。接入 `env_texture` 的 PMREM/天空路径；`tools/make_env_hdr.py` 新增纯 Python EXR
> 写入（FLOAT/ZIP + HALF/ZIPS），fixture 与 `.hdr` 解码值逐位一致；`env_pmrem_exr*.scn` + e2e ⑬ 断言
> 与 `.hdr` 渲染 **max_diff=0**。cubemap kind=1 已支持（第 26 轮）；IBL 光照标定仍剩。
> **第 18 轮（glTF 相机）** importer `--use-gltf-camera`：节点遍历第一个 perspective 相机（yfov/znear/zfar
> + 节点世界 TRS；orthographic 警告回退自动机位）；migrate 新增 `GLTF_CAMERA_MODELS`，对实测指标改善的 5 个模型
>（Duck / EnvironmentTest / IridescentDishWithOlives / TrafficCone / TransmissionTest）启用并重导重渲：
> hist 0.388→0.629、0.280→0.421、0.363→0.456、0.332→0.451、0.292→0.444，rgb_delta 均下降；
> 变差/持平/多相机模型保持自动或手动机位（决策与数据见 §5.4/§11）。
> **第 19 轮（渲染回归测试）** 新增 `tests/pbr_regression_e2e.sh`（ctest `pbr_regression_e2e`）：
> ①148 条 manifest + 149 条 comparison 完整性，关键模型 hist 下限（ToyCar/Duck/CompareSpecular/
> IridescentDishWithOlives）；②Normal oracle（前向法线编码 188/188/255）+ 镜像 scale.x=-1 front-face
> 可见性；③重渲染 ToyCar/Duck/CompareSpecular 与已提交 `preview.jpg` 比较（mean<6、max<140；无 PIL 跳过）。
> 覆盖原 findings §11 的 S1/负行列式/对比基线三条；AO/wrap/ACES/mip/双面法线仍由 material_*_e2e 相对断言覆盖。
> **第 20 轮（transmission P1：屏幕空间折射 + transmissionTexture + dispersion）** gfx 3D 拆分
> 「opaque pass（clear）→ copy 颜色成 scene color → blend pass（load）」两级 pass（仅单视图 + 离屏目标；
> 直呈 surface 无 CopySrc → 回退旧 alpha 近似）；camera bind group 新增 binding6（scene color），
> 材质 group1 新增 binding19（transmissionTexture，`flags2` bit23 presence）。shader 在 fs_standard/
> fs_physical：`refract()` 逐像素视线折射（IOR + `volume_thickness`）把出射点投影回屏幕采样背景，
> R/G/B 用不同 IOR ⇒ `KHR_materials_dispersion` 色散；`transmissionTexture.R` 调制透射率（CompareDispersion
> 的白色 glTF logo）；Fresnel 环境反射叠加；体积厚度 → `Draw.clearcoat.z`、色散 → `Draw.alpha.w`（不扩
> 288B uniform）。importer/schema/ABI 新增 `transmission_texture`/`volume_thickness`/`dispersion` +
> `textures5[1]`（struct 296B）。CompareDispersion 手动机位按官方参考图反解（平视长焦 fov 4.24°）；
> 32 个 transmission 场景重导重渲：CompareDispersion 对比官方 hist 0.44→0.57、rgb_delta 0.064→0.018、
> 覆盖率比 0.235→0.964；非 transmission 场景逐像素不变（ToyCar/Duck/CompareSpecular diff=0）；
> 全套 ctest 91 项通过（含 khronos_gallery/pbr_regression/material_*）。
> **第 21 轮（KHR_materials_anisotropy 各向异性 GGX）** 按 Khronos 规范实现替代旧的"只压缩粗糙度"近似：
> 10_utils 新增 `distribution_ggx_aniso` / `visibility_ggx_aniso`（`at = mix(roughness²,1,strength²)`、
> `ab = roughness²`，Burley 各向异性 GGX）；`Surface` 增加 `aniso`/`aniso_dir`（strength × 贴图 B；
> 方向默认切线 +X，经 `anisotropyRotation` 旋转，贴图 RG 给每像素方向）。无 TANGENT 属性时用
> `cotangent_frame` 从 UV 导数构造切线基；环境反射按规范 bent-normal 近似 + 沿切向多采样
> （低粗糙度 + 高强度 → 高光拉成弧/条）。灯光侧 `light_standard` 直接镜面项换各向异性 BRDF；
> 删掉 fs_physical 的"粗糙度压缩"旧 hack。AnisotropyStrengthTest 增加**手动机位**（正面平视 7×7
> 球阵，fov 30°）+ **手调环境**（migrate 新增 `MANUAL_ENV`：暗色影棚 + 0.25 软光箱，默认 analytic
> studio 会把测试球打成亮灰）；7 个 anisotropy 模型重渲：该模型对比官方 hist 0.26→0.58、
> mean_lum delta 0.476→0.175、rgb 0.479→0.178。其余场景（非 anisotropy 材质）逐像素不变。
> **第 22 轮（KHR_materials_diffuse_transmission：不透明 + 背光透射）** 修「陶瓷/叶片被 alpha BLEND
> 近似成透明幽灵」：importer 不再改 `alphaMode/opacity`，改为导入 `diffuse_transmission_factor` /
> `diffuse_transmission_color`（对齐 glTF 的 factor/colorFactor）；runtime 读入并置 flags2 bit24；
> gfx 在材质 uniform 尾部追加 `Draw.trans`（288→304B，`du[72..75]`）传 factor/color。shader 在
> `light_standard` 加「背光 wrap-diffuse + 背面环境辐照」两项透射（乘 factor/colorFactor），材质保持
> **不透明**（陶瓷有实心观感与暖调透光）。6 个模型重导重渲：DiffuseTransmissionTeacup 由幽灵玻璃变为
> 实心瓷器（hist 0.42→0.63、亮度差 -0.127→+0.164），DiffuseTransmissionTest 测试板 factor/colorFactor
> 两行的单调变化恢复（hist 0.82）；透射贴图（条纹/logo 行）与散射/厚度仍未做。
> **第 23 轮（KHR_materials_diffuse_transmission 完整接入）** 补上两张贴图槽：
> `diffuseTransmissionTexture.A`（透射率乘子，线性）与 `diffuseTransmissionColorTexture.RGB`（sRGB 透射色）
> → schema `diffuse_transmission_texture` / `diffuse_transmission_color_texture`、ABI `textures6[2]`
> （struct 312→320B）、flags2 bit25/26、group1 binding 20/21（材质 bind group 20→22 项）；
> runtime 分别用 `load_texture_mips` / `load_texture_srgb` 加载；shader 在 surface_common 里
> `factor *= tex.A`、`color *= colorTex.rgb`。6 个模型重导重渲：DiffuseTransmissionTest 的
> `diffuseTransmissionTexture`（条纹）与 `diffuseTransmissionColorTexture`（glTF logo）两行与官方一致
> （hist 0.79）；Teacup 仍为实心瓷器（hist 0.65）。`gltf_import_e2e` 夹具扩两槽断言
> （`tests/data/gltf/diffuse_volume.gltf` 加 1×1 PNG 透射率/色贴图）。
> **第 24 轮（KHR_materials_volume 厚度/吸收/散射）** 按规范补齐体积模型：importer 导入
> `attenuation_color`/`attenuation_distance`/`volume_thickness_texture`（thicknessTexture.G）与
> `KHR_materials_volume_scatter.multiscatterColorFactor`（`multiscatter_color`），删除旧的
> 「base_color × attenuationColor」薄壁色调 hack；ABI 追加 4 字段 + `textures7[1]`（struct 320→352B），
> 材质 uniform 304→336B（`Draw.vol`/`Draw.scatter`），group1 新增 binding 22；shader 加
> `volume_transmittance()`（Beer-Lambert）作用在折射采样与漫透射上，体积散射以各向同性 in-scatter
> + 漫反射色调化近似（ScatteringSkull hist 0.18→0.58）。25 个 volume 模型重导重渲：玻璃/琥珀/牛奶玻璃
> 出现真实厚度衰减（GlassHurricaneCandleHolder 0.56→0.75、AttenuationTest 0.70→0.81、CompareDispersion
> 0.44→0.57 等）。
> **第 25 轮（官方 HDRI 环境接入）** 迁移工具新增 `tools/khronos_env.json`（README 明确声明环境的 23 个模型
> → 官方 Khronos glTF-Sample-Environments HDRI）+ `--no-hdri-env` 开关；引擎 `scn.Scene` 新增
> `env_background`（HDRI 只做 IBL、不画天空），渲染时用 `MAQUE_MOUNT="/khronos_env=<repo>/.deps/khronos-env"`
> 挂载（HDRI 不入库：许可混杂 + 16MB）。每个模型自动 **env_intensity 标定**（按参考图前景亮度 1–2 轮）
> 并做**指标回退**（hist 比 analytic 差 >0.02 撤销）。最终 17/23 保留 HDRI：AnisotropyDiscTest
> 0.255→0.648、IridescenceLamp 0.320→0.642、CarConcept 0.310→0.567、TransmissionRoughnessTest
> 0.408→0.657、CarbonFibre 0.418→0.566、ClearCoatCarPaint 0.723→0.823、DragonAttenuation 0.204→0.286
> 等；6 个（Teacup/DiffuseTransmissionTest/GlassBrokenWindow/PotOfCoals/Sunglasses/ClearcoatWicker）
> 回退 analytic。
> **第 26 轮（cubemap 环境贴图）** 完整支持 6 面立方体环境：
> `scn.Scene.env_cubemap` 写基名 → 运行时按 `<基名>_px/_nx/_py/_ny/_pz/_nz.(hdr|exr|png|jpg)` 加载（面序
> +X/-X/+Y/-Y/+Z/-Z，u 向右 v 向下；与 `tools/equirect_to_cube.py` 逐式一致）；装配期 CPU 生成
> **GGX 预过滤 cube mip 链** + **余弦卷积 irradiance cube（16×16×6）**；`create_texture2(kind=1)`
> 建 6 层纹理 + `Cube` view（ABI `mq_renderer_environment` +`env_cubemap/env_cubemap_irradiance`，
> struct 92→100B，camera uniform +`env4` 896→912B）；shader group0 新增 binding7/8
> （`texture_cube`），`env_sample/env_irradiance/天空 pass` 按方向采样（缺失绑 1×1×6 dummy、
> `env4.x=0` 走 equirect/analytic）。样例：`scenes/pbr/env_cubemap.scn`（studio 环境 6 面，
> 由 `tools/equirect_to_cube.py` 从 `scenes/pbr/env/studio.hdr` 生成、入库）；e2e `env_cubemap_e2e`：
> 日志 `env CUBEMAP … mips (GGX) + irradiance`、同环境 equirect vs cube 渲染 hist 0.97/mean 1.26、
> 确定性逐字节一致、缺失 6 面时 warn + 回退 analytic。
> **第 27 轮（DirectionalLight 效果修复）** 官方 `screenshot.png`（IKEA 测试查看器）是浅灰背景 + 纯方向光
> （无 IBL）的黄色球阵。原预览灰白：两个叠加原因 ——
> ① **源资产三个测试球整体朝内**（NORMAL 与绕序一致朝内）：单面材质剔掉外表面后只剩内表面，法线朝背 →
> 平行光只在轮廓掠射点亮，阴影贴图又把远面判成被近面遮挡 → 几乎全黑；
> ② 手调覆盖用 Python `json` 往返 `.scn` 时**丢了灯光组件的重复 `"type"` 键**（`"render.Light"` 字符串 +
> 数值 LightType 双键约定）→ 灯光组件被静默丢弃，退化成运行时默认主光。
> 修复：importer 新增「闭合且法线/绕序一致朝内的实体 → 导入期翻正（法线取反 + 绕序反转）」保守判定
> （顶点≥64、边 2-共享≥98%、平均 cos<-0.75；开放网格/房间内壁/天空穹顶不动；DirectionalLight 三球 avg cos
> 由 -1.0 变 +1.0）；migrate 场景编辑改为**保留重复键**的 JSON 读写 + 写回后灯光数量校验/回滚；
> DirectionalLight 加 `glTF 自带相机` + `MANUAL_CAMERA` 浅灰背景（0.88）+ `MANUAL_ENV` 关 analytic IBL +
> 新增 `MANUAL_LIGHT`（光强按 glTF 原值 1.0，README 能量测试语义）；DiffuseTransmissionTest 因同样
> round-trip 丢灯的旧问题一并重导修复。对比指标 hist 0.11→0.50、前景 RGB 距离 0.424→0.061；
> `gltf_import_e2e` 增夹具 `inside_out.gltf`（闭合朝内球壳 → 必翻正）/`open_plane.gltf`（开放网格 → 不得误翻）。
> **第 28 轮（USDShaderBallForGltf 效果 + KHR_materials_volume 厚度单位）** 官方 `screenshot_large.jpg` 是
> three.js 编辑器特写（pathtraced | rasterized 两联图、平背景、球占 ≈90% 画面高）；原预览是**近黑玻璃球**
> （hist 0.42）。根因：`KHR_materials_volume` 的 `thicknessFactor/thicknessTexture` 给的是 **mesh 空间**厚度
> （规范：随节点变换缩放），而 `attenuationDistance` 是**世界空间** —— 旧实现把 mesh 空间厚度直接当世界空间
> 路径长用，USD 球节点 `scale=0.01` × `thicknessFactor=8.9` → 光学厚度被放大 100 倍（clamp 后 T≈e⁻³）→ 发黑；
> 折射偏移同样被放大 100 倍。修复：runtime 提取时 `volume_thickness *= cbrt(|sx·sy·sz|)`（非均匀缩放取几何均值
> 近似路径长）。连带改善（同因）：AttenuationTest 0.695→0.789、DragonAttenuation 0.144→0.395、
> DragonDispersion 0.496→0.581；MosquitoInAmber 因厚度正常化后需要亮棚环境（官方参考是亮背景棚拍）
> → `MANUAL_ENV` 暖白 0.75 / intensity 2.5 + `MANUAL_CAMERA` 浅灰背景，hist 0.137→**0.792**（lum 0.415→0.568，
> 参考 0.573）。USD 球另加 `MANUAL_CAMERA`（按官方构图拟合：方位 40°/仰角 8°/距离 0.135/fov 40°、浅灰背景）+
> `MANUAL_ENV env_background=false`（Studio Neutral HDRI 只做 IBL），hist 0.42→**0.71**。
> **注意**：该模型官方参考是两联图，整图指标只作趋势参考（README 已注明）；`MANUAL_ENV` 新增
> `env_background` 覆盖（HDRI 注入会尊重它，migrate 三处 `apply_hdri_env` 调用都传覆盖值）。
> 新增回归 `ctest -R volume_scale_e2e`：`scenes/pbr/volume_scale.scn` 两个同材质 cube（scale 1.0 vs 0.1，
> 世界厚度差 10×）→ 前景亮度差必须 >0.10；已验证关掉缩放修复时该测试**失败**（鉴别力自检）。
> **第 29 轮（DamagedHelmet 观感）** 官方 `screenshot.png` 只有 130×126、透明底（旧 SampleViewer 截图）：
> 自动机位太远且俯视（前景覆盖率 0.11 vs 0.61），且 analytic 环境强度 1.6 让大面积金属发白。
> 处理：`MANUAL_CAMERA` 按官方构图拟合（世界包围盒中心 (-0.0025,0,-0.1872)/半径 1.0 → 方位 45°、仰角 8°、
> 距离系数 0.95、fov 42°、浅灰底 0.92）；`MANUAL_ENV` 把 analytic 亮度 1.6→**0.8**（金属高光不过曝）。
> 指标 hist 0.579→**0.817**、前景 RGB 距离 0.114→**0.052**；`MANUAL_REFERENCE` 注明官方图是 130×126 小图、
> 整图指标只作趋势参考。细扫脚本按「整图 hist + 前景亮度」选了 az45/el8/×0.95（比 az60/el12 再 +0.01），
> 说明自动机位拟合流程可复用：包围盒 → 距离系数/方位/仰角网格扫描 → 官方图整图指标排序 → 手调表落盘。
> **第 29 轮（glTF 动画 / 蒙皮 / morph，plans/22 S1+S2+S3）** importer 读 `skins`/`animations`/`morph targets`：
> 27 个模型（26 动画 ∪ 7 骨骼 ∪ 4 morph）改用**层级场景**（glTF 节点树 1:1；蒙皮 mesh 按规范忽略节点变换、
> 置场景根 + `skin_path`），派生 `assets/skin/*.skel.json` 与 `assets/anim/*.clip.json`；
> 顶点新增布局 2/3（48/56B + joints u16x4 + weights f32x4，stride 72/80）；gfx group1 binding23 = 骨骼调色板
> readonly-storage（`mq_renderer_skin` + `draw3d.skin`/`frame3d.skins`，阴影 pass 同绑），shader
> `vs_skin/vs1_skin/vs_shadow_skin/vs_shadow1_skin`；runtime `Entity::world_m` + `flatten_world3d`（派生姿态，
> 不写回 Transform3D）、`anim_step` STEP/LINEAR + quat slerp、`palette = world(joint) × inverse_bind`、
> morph CPU 顶点重算 + `update_mesh`；场景 `scn.AnimationPlayer.clips` 自动播放首个片段。
> 验证：`ctest -R 'gltf_(skin|anim|morph)_e2e'` 全绿（bind pose palette≈I：`MAQUE_SKIN=0` 与默认逐字节一致；
> 帧 1/20/40 不同；同帧两次一致），`render3d_scene_e2e|pbr_regression_e2e|material_*_e2e|gltf_import_e2e|determinism`
> 无回归；27 模型定向 `--only ... --force` 重导重渲（74.1s），`manifest.animation` 记录 clips/skins/morph。
> 已知边界：`KHR_animation_pointer` 仍跳过；CUBICSPLINE 按 LINEAR 近似；无动画状态机/混合；预览=第 1 帧姿态。
> 回归夹具：`tests/data/gltf/{skinned,animated,morph}.gltf`。

### 10.3 差距分布（`gaps[]` 的模型计数 Top）

> 同一模型可命中多条；这是"README 已知差距"的模型数（不是 primitive 数）。

| 差距 | 模型数 | 说明 |
|---|---|---|
| `KHR_materials_transmission` 屏幕空间折射近似 | **32** | 玻璃可透视且有折射畸变/厚度；无真实体积吸收/多次折射（P1 剩余） |
| 动画已导入（关键帧采样，非 gap） | **26** | 第 29 轮：`plans/22` S2；预览=第 1 帧姿态、CUBICSPLINE 近似、无状态机（信息性） |
| `KHR_materials_volume` / dispersion / volume_scatter | **25** | 厚度/吸收（Beer-Lambert）/thicknessTexture 已接入；散射为非多次散射近似；色散非光谱积分（P1 剩余） |
| `alphaMode=MASK/BLEND 已支持（物体级排序）` | 16 | 信息性 gap（不是阻塞） |
| `KHR_texture_transform` 已支持 scale/offset/rotation | **14** | 信息性 gap：逐贴图独立 transform 仍部分近似 |
| 未使用 glTF 自带相机（自动机位） | **9** | 构图差异的主因之一；5 个模型已采用 glTF 相机（第 18 轮），Sponza 手动机位 |
| `KHR_lights_punctual` 已导入为 Light | **12** | 信息性 gap：自带灯已生效，强度为观感归一化 |
| `KHR_materials_clearcoat` 贴图已导入（BRDF 近似） | **12** | 清漆贴图已接（P3 仅剩精确 BRDF） |
| `KHR_materials_iridescence` 近似 | **10** | 虹彩/厚度贴图已接；干涉 BRDF 近似 |
| `KHR_materials_specular` 贴图已接（f90 未实现） | **10** | factor/color + specularTexture/specularColorTexture 已映射；仅作用电介质 F0 |
| `KHR_materials_sheen` 近似 | **9** | sheen 贴图已接；BRDF 近似 |
| `KHR_materials_anisotropy` 各向异性 GGX（环境为近似） | **7** | 强度/旋转/方向贴图已接；环境反射为解析 studio + bent-normal 近似 |
| 骨骼蒙皮已应用（palette = world × inverse_bind） | **7** | 第 29 轮 S1；法线用调色板 3×3 线性部分近似（信息性） |
| `KHR_materials_variants` 只用默认 | **7** | 变体切换不生效 |
| `KHR_materials_diffuse_transmission` 不透明 + 背光透射 | **6** | 实心材质 + 暖调透光；factor/color/透射率贴图/透射色贴图均已接入；无散射/厚度 |
| `KHR_animation_pointer` 动画未导入 | **5** | 材质/贴图变换/可见性指针通道（节点 TRS/骨骼/morph 已支持；P3） |
| morph targets 已应用（CPU 顶点变形） | **4** | 第 29 轮 S3：默认权重 + weights 动画；TANGENT 增量忽略（信息性） |
| `KHR_node_visibility` 已支持 | **3** | 信息性 gap：隐藏节点已跳过 |
| 非三角 primitive 被跳过 | **2** | 线/点装饰缺失 |
| `EXT_mesh_gpu_instancing` 已导入展开 / quantization / meshopt / WebP | 各 1 | 个别模型（见 §5；instancing 为 CPU 展开，非 GPU 实例化） |

> **blocked 为 0** 的原因：所有 149 个模型的 `glTF/` 主变体都**没有** `extensionsRequired` 含 Draco/KTX2/WebP；
> 压缩变体只存在于 `glTF-Draco/` 等目录（迁移不取）。

### 10.4 大量缺失特性的模型（后续排期样本池）

| 特性 | 模型数 | 代表模型（全量列表见 `manifest.json` 的 `gaps[]`） |
|---|---|---|
| transmission / volume | 32 / 25 | `ABeautifulGame`、`ChronographWatch`、`FlightHelmet`、`GlassBrokenWindow`、`GlassHurricaneCandleHolder`、`GlassVaseFlowers`、`MosquitoInAmber`、`StainedGlassLamp`、`SunglassesKhronos`、`TransmissionTest`、`IridescenceLamp`、`DragonAttenuation` |
| 扩展材质贴图槽（clearcoat/sheen/iridescence/specular/anisotropy） | 约 37 个模型含相关 gap | `ClearCoatCarPaint`、`ClearCoatTest`、`ClearcoatWicker`、`SheenChair`、`SheenCloth`、`GlamVelvetSofa`、`IridescenceAbalone`、`IridescenceDielectricSpheres`、`IridescenceMetallicSpheres`、`AnisotropyDiscTest`、`AnisotropyStrengthTest`、`CarbonFibre`、`SpecularTest` |
| 动画 / 骨骼 / morph（✅ 第 29 轮已支持） | 26 / 7 / 4 | `BrainStem`、`CesiumMan`、`CesiumMilkTruck`、`Fox`、`RecursiveSkeletons`、`RiggedFigure`、`RiggedSimple`、`SimpleSkin`、`AnimatedMorphCube`、`MorphStressTest`、`AnimationPointerUVs`（后者仍为 pointer-only 静态） |
| `TEXCOORD_1+`（多 UV） | 8 | `CarConcept`、`ChairDamaskPurplegold`、`ChronographWatch`、`MorphStressTest`、`MultiUVTest`、`SheenChair`、`SheenWoodLeatherSofa`、`TextureTransformMultiTest` |
| `COLOR_0` 顶点色 | 8 | `BoxVertexColors`、`CompareBaseColor`、`IridescentDishWithOlives`、`MeshoptCubeTest`、`PrimitiveModeNormalsTest`、`RecursiveSkeletons`、`SheenWoodLeatherSofa`、`VertexColorTest` |
| 参考图需特殊处理 | 13 个 `.gif`（PIL 取第一帧）+ **12 个无精确参考图**（笔记标注"上游无参考图/统计缺失"，与"有参考但差异大"区分） | 见 §10.2「参考图口径」与 §8.2 I14 |
| 大模型/慢模型 | 8 | `NodePerformanceTest`（731,850 顶点，95s）、`ABeautifulGame`（408,691 / 32.5s）、`MetalRoughSpheresNoTextures`（528,291 / 23.9s）、`Sponza`、`IridescenceMetallicSpheres`、`IridescenceDielectricSpheres`、`MetalRoughSpheres`、`CarConcept` |
| 特殊扩展（单点） | 各 1 | `SheenWoodLeatherSofa`（WebP 贴图）、`MeshoptCubeTest`（meshopt）、`PrimitiveModeNormalsTest`（量化 + 非三角）、`MeshPrimitiveModes`（非三角）、`MaterialsVariantsShoe`（variants） |

### 10.5 如何读取/复算（命令可复制）

```powershell
# 1) 总量/状态分布（PowerShell）
$m = Get-Content scenes/khronos/manifest.json -Raw -Encoding UTF8 | ConvertFrom-Json
"records = $($m.records.Count)  total_seconds = $($m.total_seconds)"
$m.records | Group-Object status | Sort-Object Count -Descending | Format-Table Count,Name -AutoSize

# 2) blocked / 失败明细
$m.records | Where-Object { $_.status -notin @('ok','imported') } |
  Select-Object model,status,@{n='issues';e={$_.issues -join '；'}} | Format-Table -AutoSize

# 3) 差距分布 Top（Python 一行；注意 PowerShell 里用内层单引号）
python -u -c "import json,collections; m=json.load(open('scenes/khronos/manifest.json',encoding='utf-8')); r=m['records']; print('models',len(r)); print(collections.Counter(x['status'] for x in r)); print('gaps top', collections.Counter(g for x in r for g in x.get('gaps',[])).most_common(20))"

# 4) 派生资产总体积（本地；不入库）/ 最慢模型
python -u -c "import json; m=json.load(open('scenes/khronos/manifest.json',encoding='utf-8')); r=m['records']; print('derived MB', round(sum(x.get('out_size_bytes',0) for x in r)/1048576,1)); print('slowest import', sorted(r,key=lambda x:-x.get('import_seconds',0))[:5]); print('slowest render', sorted(r,key=lambda x:-x.get('render_seconds',0))[:5])"

# 5) 渲染/构图异常筛查（meshes=0 / mesh=-1 / wgpu / 覆盖率过低 / 贴边）
python -u -c "import json; r=json.load(open('scenes/khronos/manifest.json',encoding='utf-8'))['records']; bad=[(x['model'],i) for x in r for i in x.get('issues',[]) if any(k in i for k in ('meshes=0','mesh=-1','wgpu','覆盖率','构图告警'))]; print(len(bad)); [print(b) for b in bad]"

# 6) 前景覆盖率最低的 15 个（自动机位可疑）
python -u -c "import json; r=json.load(open('scenes/khronos/manifest.json',encoding='utf-8'))['records']; c=[(x['model'],(x.get('compare') or {}).get('preview_coverage')) for x in r if (x.get('compare') or {}).get('preview_coverage') is not None]; [print(m,round(cov,4)) for m,cov in sorted(c,key=lambda t:t[1])[:15]]"

# 7) 某模型细节
$m.records | Where-Object model -eq 'DamagedHelmet' | ConvertTo-Json -Depth 6

# 8) 参考图/对比覆盖（区分 gif 参考图 / 无参考图）
python -u -c "import json; r=json.load(open('scenes/khronos/manifest.json',encoding='utf-8'))['records']; c=[x for x in r if x.get('compare')]; print('compare',len(c),'/',len(r)); print('gif refs',sum(1 for x in c if str(x.get('reference','')).endswith('.gif'))); print('no reference',[x['model'] for x in r if not x.get('reference')])"
```

# 8) 批量真实耗时（见下方 Git Bash heredoc 里的批量日志解析；`migrate-batch2.log` 是 UTF-16）

Git Bash 等价（可用 heredoc）：

```bash
python -u - <<'PY'
import json, collections, statistics, re
m = json.load(open('scenes/khronos/manifest.json', encoding='utf-8'))
r = m['records']
print('models', len(r), 'total_seconds(manifest, 可能被 --reuse-preview 覆盖)', m.get('total_seconds'))
print(collections.Counter(x['status'] for x in r))
print('gaps top', collections.Counter(g for x in r for g in x.get('gaps', [])).most_common(20))

# 批量真实耗时：解析 migrate-batch2.log（Windows PowerShell Tee-Object 写出 UTF-16）
rows = []
for line in open('build-win/audit/migrate-batch2.log', encoding='utf-16', errors='replace'):
    if not line.startswith('['):
        continue
    tok = line.split()
    if 'ok' not in tok and 'failed' not in tok:
        continue
    i = tok.index('ok') if 'ok' in tok else tok.index('failed')
    rows.append((' '.join(tok[1:i]), float(tok[i + 1].rstrip('s'))))
t = [x[1] for x in rows]
print('batch models', len(rows), 'sum', round(sum(t), 1), 'median', round(statistics.median(t), 2))
print('slowest', sorted(rows, key=lambda x: -x[1])[:8])
PY
```

---

### 10.6 逐模型对比报告（`reports/khronos-comparison/`）

迁移完成后生成**可浏览的逐模型对比报告**（149 个模型 = 148 自动 + 手工 ToyCar）：

| 文件 | 说明 |
|---|---|
| `index.html` | 左右对比卡片：官方参考图（**large 优先**；GIF 取首帧）vs maque `preview.jpg`，附指标（直方图相似度/亮度差/RGB 距离/前景占比）、自动初判、已知差距、人工视觉复核（34 个模型）、到模型笔记/场景的链接；支持搜索、按初判筛选、按相似度/差距排序 |
| `ref/<Model>.jpg` | 官方参考图下采样（≤900px、JPEG q82；透明背景合成中性灰，便于左右并排）——报告自包含，不依赖 `.deps` |
| `overview.jpg` | 代表模型总览拼图（差异两端 + 手工样板 + 特性代表） |
| `comparison.csv` / `comparison.json` | 全量指标（可用 Excel/脚本二次分析） |
| `README.md` | 报告摘要 + 最需关注的 15 个模型 + 最接近的 10 个模型 |

生成命令（仓库根目录）：

```bash
python tools/make_migration_report.py                 # 默认 ≤900px / q82
python tools/make_migration_report.py --max-dim 1200 --quality 85
```

> 报告中的参考图选择与 `manifest.json.compare` 一致（`find_reference_image()` **large 优先**）。
> 注意报告仍**不是像素级 pass/fail**（见 §7.1）：自动指标只用于筛查，结论以人工复核/README 归因为准。

## 11. 后续工作（按收益/成本排序）

| 优先级 | 事项 | 影响面（本批样本） | 落点/备注 |
|---|---|---|---|
| P2 | **IBL 光照标定**（cubemap kind=1 已完成，第 26 轮） | 全部 | 现有：`.hdr`（RGBE→RGBA16F）、**EXR（NONE/RLE/ZIP/ZIPS、half/float）**、PNG/JPEG env、内建 studio_env、**GGX importance-sample 粗糙度 mip 预过滤**、**漫反射 irradiance 卷积**、天空背景、**cubemap（第 26 轮：`scn.Scene.env_cubemap` 6 面 `<基名>_px.._nz`(.hdr/.exr/.png/.jpg) → CPU GGX cube PMREM + irradiance cube + `texture_cube` 方向采样（IBL/天空），`kind=1` cube 纹理/view，样例 `scenes/pbr/env_cubemap.scn`，e2e `env_cubemap_e2e`）**；剩余：默认开启前的光照/曝光标定 |
| P3 | ✅ **specularTexture/specularColorTexture**（第 14 轮已完成） | 3 个模型实际用到贴图（CompareSpecular / SpecularTest / AnimationPointerUVs） | 已加 `textures4[2]` + flags2 presence bit21/22 + group1 binding17/18；现每 stage 20 张 sampled texture（15+2 材质 + shadow/env/irradiance），依赖适配器上限（桌面 GPU 通常 ≥128） |
| P3 | ✅ **MSAA 4x（opt-in）**（第 15 轮已完成） | 全部 3D 场景（默认关；按场景/环境变量开） | `scn.Scene.msaa_samples` / `MAQUE_MSAA`；4x 颜色+深度附件、pass resolve、管线按样本数分缓存；`caps.max_samples=4` 探测，失败自动回退；e2e ⑫ |
| P1 | **transmission / volume / dispersion / diffuse_transmission 完整实现** | transmission 33、volume 25、diffuse_transmission 6、dispersion 4 | **第 20/22–24 轮已完成**：屏幕空间折射 + scene color copy + 逐像素折射（厚度）+ transmissionTexture + dispersion RGB 偏移（第 20 轮）；diffuse transmission 不透明 + 背光透射 + 两贴图槽（第 22–23 轮）；volume 厚度/Beer-Lambert 吸收/thicknessTexture/volume_scatter in-scatter（第 24 轮）。**剩余**：光线追踪级多次折射/散射、相位函数、直呈（surface）路径的折射 |
| P2 | ✅ **动画 / 骨骼 / morph**（第 29 轮已完成） | 动画 26 个模型、skin 7、morph 4 | `plans/22-gltf-animation-implementation.md`：S1 骨架蒙皮（布局 2/3 + storage palette + 阴影变体）、S2 关键帧采样（`scn.AnimationPlayer.clips`，STEP/LINEAR + quat slerp）、S3 CPU morph；测试 `gltf_{skin,anim,morph}_e2e`。**剩余**：`KHR_animation_pointer`（P3）、CUBICSPLINE 精确切线、状态机/混合 |
| P2 | **Draco / KTX2 / meshopt 解码** | 主变体：meshopt 1（KTX2/Draco 0；全变体扫描 Draco 18、basisu 7、webp 5、meshopt 2） | 评估结论：主变体不阻塞、不做解码器（`plans/21` §B）；当前用 `glTF/` 主变体规避 |
| P2 | **glTF 自带相机（部分完成）** | 相机 14 个模型 / 45 个 | ✅ importer `--use-gltf-camera` + migrate 按模型采用（5 个指标改善模型，第 18 轮）；剩余：orthographic 相机（引擎仅透视）、多相机/多视图参考的选择策略 |
| P3 | **`TEXCOORD_2+` / 逐贴图独立 UV transform**（已评估，暂不做） | TEXCOORD_2 仅 1 个 primitive（且无贴图引用）；逐槽 transform 冲突实测 10 模型 | 评估：①TEXCOORD_2 零视觉影响（MosquitoInAmber 该属性未被材质引用），不做；②同 UV 集但不同 transform 的材质共 10 模型（AnimationPointerUVs 69 处最多且属 pointer 动画；其余 CarConcept/ToyCar/Sheen* 等 1–5 处）→ 需 9 槽×3 float 的材质 uniform 扩展，收益小于 ABI 成本，保留近似并记录 |
| P3 | **GPU 实例化**（已评估，暂不做） | 大实例数资产：主变体仅 SimpleInstancing（125 实例；其余模型 ≤10 实例） | 评估：当前 CPU 展开 125 draw 在此批量渲染中开销可忽略（单模型 <1 ms 量级），而实例化需 `frame3d.instances` + `drawIndexedInstanced` + 每实例矩阵 buffer，ABI/管线成本 > 收益；保留 P3 |
| P2 | **ACES 曲线是否切 three.js RRT/ODT** | 全部（与参考图的系统性色调偏差） | 会整体改变观感基线，需用户拍板（`plans/20 §6`） |
| P3 | **--scene 保护**（输出 `.auto.scn` 或要求 `--force`） | 手调模型 | 流程安全；当前靠 `--exclude` + README 警告 |
| P3 | **二进制网格格式 / 更大的 `--compact` 压缩** | 大模型（百万顶点级） | 降低文本 JSON 体积与解析成本 |
| P3 | **per-draw uniform arena**（去掉每帧重建 UB/bind group） | 149 模型批量渲染性能 | `plans/20 §6` 待办 |
| P3 | ✅ **参考图对比回归测试**（第 19 轮已完成） | 全部 | `tests/pbr_regression_e2e.sh`：manifest/compare 指标下限 + Normal/镜像 oracle + 3 模型 preview.jpg 渲染基线（mean<6）；原 findings §11 的 S1/负行列式/对比基线已覆盖，AO/wrap/ACES/mip/双面法线仍由 material_* 相对断言覆盖（精确 oracle 可后续补） |
| P3 | **迁移回归脚本** | 全部 | 新增模型后重跑 `migrate --only <Model>` + `ctest -R 'gltf_import_e2e|render3d_scene_e2e|pbr_regression_e2e|material_.*_e2e'` |
| P3 | **地面/阴影预设** | 参考图有棚拍背景的模型 | 可选 `--with-ground`，或手加 `material_shadow.scn` 范式 |

---

## 附录 A · 文件与命令速查

| 用途 | 位置 |
|---|---|
| 批量迁移驱动 | `tools/migrate_gltf_models.py` |
| 离线导入器 | `tools/gltf_import.py` |
| 资产下载脚本 | `.deps/download-gltf-assets.js`（`.deps` 不入库，内容见 §3.1） |
| 源资产 | `.deps/gltf-assets/glTF-Sample-Assets-main/Models/<Model>/` |
| 参考图 | `.deps/.../Models/<Model>/screenshot/screenshot*.png|jpg` |
| 上游元数据 | `.deps/.../Models/<Model>/metadata.json`（标签/许可/summary） |
| 迁移产物 | `scenes/khronos/<Model>/` |
| 总索引 | `scenes/khronos/README.md`（脚本生成） |
| 机器清单 | `scenes/khronos/manifest.json`（脚本生成） |
| 审计证据（构建产物，不入库） | `build-win/audit/{shader,renderer,asset,verify}/findings.md` |
| 设计/审计计划 | `plans/18-pbr-3d.md`、`plans/19-threejs-materials.md`、`plans/20-pbr-audit.md` |
| 手工调校样板 | `scenes/khronos/ToyCar/`、`scenes/pbr/*.scn` |

```powershell
# 全流程（重复执行安全；手调模型先排除）
python -u tools/migrate_gltf_models.py --force
# 单模型
python -u tools/migrate_gltf_models.py --only <Model> --force
# 单场景人工看/截图
.\build-win\samples\player\maque_player.exe scenes\khronos\<Model>\<Model>.scn
$env:MAQUE_PRESENT='off'; $env:MAQUE_WINDOW='0x0'
.\build-win\samples\player\maque_player.exe --shot out.png scenes\khronos\<Model>\<Model>.scn
```

## 附录 B · 迁移状态机（`manifest.records[].status`）

| status | 含义 | 处理 |
|---|---|---|
| `ok` | 导入 + 渲染 + `preview.jpg` + 对比（如有参考图）全成功 | 正常交付 |
| `imported` | 导入成功但没生成 `preview.jpg`（通常缺 PIL） | 装 PIL 后 `--only` 重跑 |
| `blocked` | 主变体必需 Draco（或找不到主变体） | 换变体/记录原因；不重试 |
| `failed` | importer 失败（rc≠0） | 看 `import_log_tail`；修资产/工具后重跑 |
| `render_failed` | 渲染失败/没出 PNG | 看 `render_log_tail`；修场景/相机/GPU |
| `exception` | 驱动脚本异常（traceback） | 看 `traceback`；修工具后重跑 |
| `ok (hand-tuned)` | 总索引对"有 scn+README 但不在 records"的手调模型补的行 | 手工维护，不参与自动批量 |
