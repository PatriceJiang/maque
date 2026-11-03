# 012 · Windows 检出让 e2e 门假失败 / 假跳过（autocrlf、窗口模式、反向探针）

- **类型**：测试基础设施 + 正确性（一处解码健壮性）
- **状态**：fixed
- **影响面**：所有 `core.autocrlf=true` 的 Windows 工作区、以及"有显示器"的机器；
  6 个 e2e 门（`env_cubemap_e2e`、`gltf_import_e2e`、`render3d_scene_e2e`、
  `render3d_present_e2e`、`imgui_engine_options_e2e` ⑤b）+ 9 个被静默跳过的用例
  （`spine_*` ×5、`flappy_smoke`、`tank_smoke` 等）
- **发现版本**：`95de6de` 之后跑全量 `ctest --test-dir build-win -j 4`；
  每一项都用 10/30 的旧构建 A/B 过（PPM 逐字节相同）→ 与当轮改动无关，属长期潜伏

## 症状

同一份提交，Linux/无显示环境全绿，Windows 有显示器的机器上：

| 门 | 症状 |
|----|------|
| `env_cubemap_e2e` | `env_texture '…/studio_py.hdr' HDR(RGBE) 解析失败` → 回退 analytic → `FAIL cube 未走 CUBEMAP 装配` |
| `render3d_scene_e2e` ① / `gltf_import_e2e` ③ | `FAIL 已入库资产与 importer 输出不一致（导入资产漂移）`，但 `git diff` 干净 |
| `render3d_scene_e2e` ① | 离屏占比断言 `nonbg=0.694`（应 <0.6），同一场景无窗口时是 0.187 |
| `imgui_engine_options_e2e` ⑤b | `vram no3d=2608`，断言写死 `== 1584` |
| `render3d_present_e2e` ① | `AssertionError: ('content starts black', (0,0,0), 60)` |
| `spine_*` / `flappy_smoke` / `tank_smoke` | 明明有 GPU（其它 GPU 用例都在跑），却报 `SKIP 渲染插件不可用（无 wgpu/GPU）` |

## 最小复现

```powershell
# 1) .hdr 被换行转换改写（blob 是 LF，工作区是 CRLF）
git cat-file -s HEAD:scenes/pbr/env/studio_cube/studio_py.hdr   # 65585
(Get-Item scenes/pbr/env/studio_cube/studio_py.hdr).Length      # 65589
# 2) 生成物同理：importer 输出 5571 字节，已入库资产 5985 字节
python tools/gltf_import.py tests/data/gltf/cube.gltf --out build/imp
cmp build/imp/cube.mesh.json scenes/pbr/assets/cube.mesh.json
# 3) 窗口模式漂移
ctest --test-dir build-win -R '^render3d_scene_e2e$' --output-on-failure   # 有显示器时
# 4) 反向探针
./build-win/samples/player/maque_player.exe samples/spine/spine.scn 2>&1 | Select-String renderer
```

## 根因

1. **`core.autocrlf=true` 的检出按"前 8000 字节有没有 NUL"猜文本**。
   `studio_cube` 的 `_py`（天空）/`_ny`（地面）是平滑渐变，整段头部区一个 NUL 都没有 →
   被判成文本，头部 4 个 `\n` 变成 `\r\n`。RGBE 解析器用"空行结束头部"定位数据起点，
   行尾多个 `\r` 就永远等不到空行 → 拿扫描线数据去 `sscanf` 分辨率行 → 整张 env 静默回退。
   同一机制让 `*.mesh.json` 生成物多出 `\r`，于是"importer 输出 vs 已入库资产"的逐字节 `cmp`
   永远不等 —— 报的是"资产漂移"，实际是检出改写。
   `git status` 查不出来（它按转换后的内容比较），只有 `git cat-file -s` 对字节数才看得见。
2. **像素门没钉死输出配置**：`render3d_scene_e2e` 除 ② 外全是离屏断言，却从不设 `MAQUE_WINDOW` →
   有显示器时默认建 800×650 真窗口，抓帧走另一条回读路径，覆盖率从 0.187 跳到 0.694。
   Linux CI 无显示 → 整脚本 skip，所以这条分支从来没被验证过。
3. **断言钉了绝对字节数**：⑤b 假设"不开面板时除内建 cube 没有别的资源"（1584B），
   而场景后来给 Sprite 用了 `builtin:/textures/checker`（16×16 RGBA8 = 1024B）→ 恒差 1024；
   字体图集大小还随机器/DPI 漂（实测同场景 base 在 0.5 MB 与 16 MB 之间）。
4. **探针 grep 的词组只存在于失败行**：成功日志是
   `runtime: renderer '<插件>' backend='<适配器>'`，失败才是 `cannot load renderer plugin '<插件>'`。
   用 `grep "renderer plugin"` 判"有没有 GPU"正好反向 → 有 GPU 的机器一律跳过（9 个用例长期没跑）。
5. **`render3d_present_e2e` 的场景与断言不匹配**：断言按"16:9 场景 ⊂ 4:3 窗口 → bar=60"写，
   CMake 却传 `scenes/ball.scn`（320×240 且 `views:2` → 世界 640×240，真实留边 120）。
6. **`pbr_regression_e2e` ③ 依赖不入库的派生资产**：`scenes/khronos/<模型>/assets/` 由
   `tools/migrate_gltf_models.py` 从 LFS 原始 `.glb` 生成。缺了它 `mesh not found` → 画面里没有模型，
   与参考图的差异是"Duck 不见了"（p50 差 1、4.19% 像素差 >60），不是渲染回归。

## 修复

- `.gitattributes`：二进制资产（`.hdr/.exr/.ktx2/.dds/图片/字体/音频/.bin/.zip`）标 `-text binary`；
  按字节比对的生成物（`*.mesh.json/*.skel.json/*.clip.json`）标 `text eol=lf`；
  被改写过的文件用 blob 字节直接写回（`git checkout --` 因 stat 缓存不生效）。
- 解析器健壮性：`parse_radiance_hdr` 逐行剥掉尾部 `\r`（第三方 `.hdr` 也常是 CRLF 头部）。
  数据区一旦被插字节无从恢复 —— 所以 `.gitattributes` 才是根治。
- `render3d_scene_e2e`：顶部 `export MAQUE_WINDOW=0x0` 钉死离屏；② 自己给 `MAQUE_WINDOW=640x360`
  （直呈必须有真 surface）。
- `imgui_engine_options_e2e` ⑤b：改成断言**关系** —— 无选项基线 == `3d=0` == `2d=0`
  （图层开关只隐藏提交、不释放资产）、cube 1584B 仍在、开 F2 后严格变大（字体图集计入）。
- 探针：7 处 `grep "renderer plugin"` → `grep "runtime: renderer '"`（成功行）。
- `render3d_present_e2e`：场景改 `scenes/pbr/cube.scn`，与它自己的信箱断言/注释一致。
- `pbr_regression_e2e` ③：逐模型检查派生资产是否存在，缺则打印跳过原因；一个都没有就整节跳过
  （① ② 仍有效），不再假报渲染回归。

## 回归与证据

- 全量：`ctest --test-dir build-win --timeout 600 -j 4` → 修复前 118/120（2 failed、9 skipped），
  上述门全绿；`spine_*`/`flappy_smoke`/`tank_smoke` 从"假跳过"变成真跑。
- 判别力自检：把 `.hdr` 换回 CRLF 版本、或把 ⑤b 的 `no3d == plain` 改成 `!=`，对应门立刻红。
- 排查手法（写进 AGENTS Windows 一节）：怀疑换行改写时比 `git cat-file -s HEAD:<path>` 与
  工作区字节数，不要信 `git status`。

## 相关

- [004](004-render-target-attachment-churn.md)（同尺寸/附件缓存，Windows 上也表现为"只有本机复现"）
- [005](005-windows-scene-watch-path.md)（同为"路径/行尾分隔符"类平台差异，仍 open）
- docs/31 §8 踩坑表、docs/33 §11（逐目标 MSAA）、AGENTS「Windows」一节
