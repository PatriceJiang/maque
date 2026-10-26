# AGENTS.md — 面向 AI 协作者

maque 是一个以 **"引擎即浏览器、游戏即资源"** 为核心理念的轻量游戏引擎（C++23）。
本文件给自动化协作者提供**构建/测试命令、约定与架构落点**。人读概览见 `README.md`。

## 构建与测试（务必用这些命令）

```bash
# 首次：codegen 依赖（node + yaml）
cd modules/core/data && npm install && cd -

# 构建（Ninja）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# 全部测试（CTest）
ctest --test-dir build --output-on-failure

# 只跑 codegen 校验（schema 目录/import/尺寸/引用）
cd modules/core/data && node parser.js --check

# 运行示例
./build/samples/player/maque_player scenes/ball.scn
```

- `-DMAQUE_BUILD_TESTS=ON|OFF` 控制测试（默认 ON）。

### Windows（clang-cl + Ninja；完整支持）

```powershell
# ① 一次性：取第三方依赖（SDL2 / zlib-ng / zstd / freetype / libcurl / libwebsockets）
#    下载源码 → 用同一 clang-cl 构建 → 装到 .deps/dist → 生成 .deps/deps.cmake + 自检
powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\fetch-deps.ps1

# ② 配置 + 构建 + 测试（一条命令；默认 Debug + ASan）
tools\win\build.cmd                 # 配置 + 构建
tools\win\build.cmd -RunTests       # 再跑 ctest
tools\win\build.cmd -Sanitize "" -BuildType Release -RunTests   # 不带 ASan 的 Release

# 或手工（等价）：
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Debug `
      -DCMAKE_C_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" `
      -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" -DMAQUE_SANITIZE=address
cmake --build build-win
ctest --test-dir build-win --output-on-failure
```

- **切勿加载 `vcvars64.bat`**：它会把 PATH 指向 VS 自带的 clang-cl 19.x，并让 CMake 的
  `Detecting CXX compiler ABI info` **永久挂起**（已实测）。CMake 会自行定位 MSVC/Windows SDK。
- **wgpu-native**：Windows 用官方 `wgpu-windows-x86_64-msvc-release.zip` 里的
  `wgpu_native.dll` + `wgpu_native.lib`（放到 `third_party/wgpu-native/lib/`，二者不入库）。
  仓库现有 `webgpu.h` 与 v29.0.1.1 发行包逐字节等价（仅行尾不同），故无需替换头文件。
- **e2e 用 Git Bash**：CMake 自动探测并把绝对路径传给 `add_test`。Windows 自带的
  `C:\WINDOWS\system32\bash.exe` 是 **WSL 启动器**，语义不一致，会被显式排除
  （`-DMAQUE_BASH=<bash.exe>` 可覆盖）。
- **UDS（`uds:`）在 Windows 明确不支持**（控制端点与 inspector 客户端都只支持 `tcp:端口`），
  调用方会得到清晰报错而非静默降级。
- `libwebsockets` 的 Windows 构建 **TLS 关闭 → 仅 `ws://`**（`wss://` 不可用）。
- 已知平台差异：模块卸载在 Windows 上**默认不调用 `FreeLibrary`**（见
  `modules/core/module/src/host.cpp` 顶部注释：wgpu 类库的 DLL 退出路径会踩已卸载内存）；
  `MAQUE_MODULE_FORCE_UNLOAD=1` 可强制打开。
- **单测临时目录**：用 `tests/maque_test_fs.h` 的 `maque_test_setup_dir()`，**不要**直接
  `fs::temp_directory_path()` + 抛异常的 `fs::create_directories`。后者在 TEMP 不可写时抛
  未捕获的 `filesystem_error` → 进程 `__fastfail`（退出码 `0xC0000409`）、**零输出**，
  完全看不出原因（实测三连崩）。helper 会探测可写性并回退到 CWD 下的 `maque-test-tmp/`，
  也可用 `MAQUE_TEST_TMPDIR` 指定。
- **e2e 需要 Python 3 解析 JSON**：脚本顶部 `. "$(dirname "$0")/maque_python.sh"` 取 `$PY`，
  **不要**直接写 `python3` —— Windows 的 `WindowsApps\python3.exe` 是 Microsoft Store
  占位程序（0 字节），裸调用会弹安装提示并挂住。helper 用 `"$cand" -c 'pass'` 探测，
  自动退回 `python` / `py -3`。

### ASan / UBSan（可选，跨平台）

```bash
# Linux（GCC/Clang）
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMAQUE_SANITIZE=address,undefined
# 跑测试：wgpu/SDL/Mesa 退出期全局单例会被 LeakSanitizer 误报；stdbuf 的 LD_PRELOAD
# 又会让 libasan 不在首位（ASan 直接 abort）——这两个开关是跑全绿的必要条件。
ASAN_OPTIONS=detect_leaks=0:verify_asan_link_order=0 ctest --test-dir build-asan --output-on-failure
# Windows（clang-cl；仅 address）
tools\win\build.cmd -Sanitize address -RunTests
```

- `-DMAQUE_SANITIZE=` 空 = 关闭。Windows 上会自动把 MSVC 运行时从 `/MDd` 改成 `/MD`
  （ASan 不支持调试版 CRT），并把 clang 的动态 ASan 运行时显式接进链接行、
  复制到各可执行文件/插件旁（否则启动即 `0xC0000135` / 链接报 `__asan_*` 未定义）。
- **Tracy 兼容**：`third_party/tracy` 的客户端配置必须带 `TRACY_NO_CRASH_HANDLER`
  （Tracy 自带崩溃处理器与 ASan 冲突；见 docs/29 §8）。
- **鉴别力自检**（别只看"能构建"）：故意越界应被报出 `heap-buffer-overflow`：
  ```bash
  printf 'int main(){int*p=new int[4];p[4]=1;return p[4];}\n' > t.cpp
  clang-cl /std:c++latest -fsanitize=address t.cpp && ./t.exe    # Windows
  clang++ -fsanitize=address t.cpp -o t && ./t                    # Linux
  ```

- **窗口（宿主启动期覆盖）**：**默认窗口 800x650**（世界/渲染目标仍是场景尺寸，
  窗口内**等比缩放 + 居中留边**显示 —— 场景比窗口大就缩小、小就放大，都不裁剪；
  输入坐标自动映射回世界坐标）。`MAQUE_WINDOW=WxH` 覆盖尺寸，**`0x0` = 不建窗口**（CI/无显示环境）；
  ⚠️ 渲染倍率 `MAQUE_SS` 未设时只在**确有窗口**时跟随窗口像素（离屏/headless 恒为 1.0 =
  设计分辨率），否则截图尺寸与文本折行行数会随窗口大小漂移。`MAQUE_TITLE=<标题>`；
  `MAQUE_VIEWS=n`；`MAQUE_PRESENT=off|software|accelerated`（默认 Windows accelerated=Direct3D /
  其它平台 software；两者都走统一的 SDL_Renderer 呈现 → 不建 GL/EGL 上下文，不与渲染插件的
  wgpu(EGL) 争用）；`MAQUE_CAPTURE=<前缀>` 从 **renderer 后台缓冲**回读前 3 帧为 PPM
  （= present 前用户即将看到的整帧，含 ImGui 面板）；`MAQUE_KEEP_OPEN=1` 忽略脚本 `quit()`。
  `MAQUE_IMGUI_POINTER="frame:x:y:type[;...]"`（type 0=move 1=down 2=up）把合成指针事件压进
  **本帧设备事件队列**，与真实鼠标同一条路径 → CI 验证 ImGui 面板拖动（`imgui_present_e2e`）。
  示例：`MAQUE_WINDOW=800x600 MAQUE_TITLE=MaqueEngine ./build/samples/player/maque_player scenes/ball.scn`
- **3D/PBR 与直接呈现（plans/18-pbr-3d.md）**：`MAQUE_PRESENT=wgpu` → 渲染插件建 **wgpu swapchain 直呈**
  （不建 SDL_Renderer、不走 CPU readback；ImGui 面板走 CPU 像素→2D 网格链叠加）；**默认仍是 SDL 路径**
  （老 2D 行为不变）。`MAQUE_CAPTURE_WGPU=<前缀>` + `MAQUE_CAPTURE_FRAMES=N` 从**交换链纹理**回读前 N 帧 PPM
  （要求 surface 支持 CopySrc）→ CI 直呈像素断言（`ctest -R render3d_present_e2e`）；`MAQUE_RENDER3D_INFO=1`
  打印 renderer caps；`--shot/--shot-seq` 在直呈模式下仍走离屏回读。3D pass/PBR 尚未实现（M2–M4）。
- **3D 组件（plans/18 M2）**：**已声明接口直接可用**——节点挂 `scene.Transform3D` + `render.Camera3D` /
  `render.MeshRenderer` / `render.Light` 即渲染 3D（`.scn` 的 `components[].type` 写全名；实体按 id 排序保证确定性）。
  M2 光照 = 方向光 Lambert + 环境色（`assets/builtin/shaders/unlit3d/`，`MAQUE_SHADER_UNLIT3D=OFF` 可剔除）；
  **所有网格共用内建 cube**（M2 尚无 mesh 资产管线，M4 接 glTF）；PBR/贴图/阴影在 M3–M4。
  样例 `scenes/pbr/cube.scn`；测试 `ctest -R 'render3d_scene_e2e|render3d_present_e2e'`
  （离屏结构断言：非背景占比/中心可见/顶部为空/明暗面颜色多样性 + 直呈交换链抓帧）。
  **M3 PBR**：材质写在节点上——`asset.PbrMaterial` 组件（`base_color`=math.Color(rgba)、`metallic`、
  `roughness`、`emissive`、`normal_scale`、`occlusion_strength`、`alpha_mode`、5 个贴图路径槽（M4 接））；
  shader `assets/builtin/shaders/pbr/`（GGX + ACES + gamma，环境项暂用常量环境色）。
  样例 `scenes/pbr/materials.scn`（光滑 vs 粗糙金属，e2e 断言粗糙度能量分布差异）。
  **M4a 贴图**：`base_color_texture` 写 `builtin:/textures/checker` 或 VFS 图片路径 → 运行时
  `create_texture` 句柄绑到 draw bind group（无贴图缺省 1×1 白，不改变基色）；uv 走 48B 顶点布局 offset40。
  **M4b 离线导入**：`python tools/gltf_import.py <model.gltf|model.glb> --out <dir> [--name x]`
  → `<name>.mesh.json`（48B 顶点布局 + indices + bounds + `asset.PbrMaterial` 材质数组 + 贴图引用）
  与 `textures/*.png`；输出排序键、固定缩进（同一输入逐字节确定），测试 `ctest -R gltf_import_e2e`。
  运行时加载 `.mesh.json` 与阴影/MR·normal 贴图槽在 M4b 后续。
  **M4c 运行时消费**：`render.MeshRenderer.mesh_path`（如 `"assets/cube.mesh.json"`，
  **相对场景目录**——运行时 VFS 根 = 场景目录）→ `load_mesh()` 解析并 `create_mesh`
  （按路径缓存，失败缓存 -1）；为空回退内建 cube。样例 `scenes/pbr/imported.scn`（含地面）
  与导入资产 `scenes/pbr/assets/cube.mesh.json`；e2e ④ 校验「importer 输出 = 已入库资产」逐字节一致（漂移）
  + 断言网格加载成功（`mesh=-1` 视为失败）。
  **M4d 贴图槽**：`base_color_texture` / `metallic_roughness_texture`（G=roughness、B=metallic）/
  `occlusion_texture`（R）/ `normal_texture`（切线空间，shader 用屏幕空间导数构造切线基，无需 TANGENT 属性）四条路径 → 运行时加载为纹理句柄；材质未给的槽默认 **-1** → 渲染器绑 1×1 白纹理
  （参数乘 1 不变）。法线贴图（需切线空间）与阴影仍待做。
- **输入（真实设备事件）**：`MAQUE_TRACE_INPUT=1` 打印 SDL 事件与平台队列；`MAQUE_HITS=1` 打印命中测试；
  `MAQUE_SIM_CLICK="frame:x:y[;...]"` 把指针事件压入 **SDL 事件队列**（与真实鼠标同一条翻译路径：
  SDL → 平台 → 输入模块 → 命中测试 → 脚本回调），用于 CI 验证输入通路。
  手动/脚本点击真实窗口：`python3 tools/xclick.py <窗口类名|标题> <窗口内 x> <窗口内 y>`
  （⚠️ 本机 Wayland 会话下 **XTest 注入无效**：指针不动，故 CI 用 `MAQUE_SIM_CLICK`）。
  演示时窗口易被其它窗口遮挡 → `MAQUE_ALWAYS_ON_TOP=1`、`MAQUE_WINDOW_POS=x,y`。
- **输入模拟器（一等机制，docs/19）**：`modules/input/simulator`（接口 DSL 声明 → `abi/sim.h`）。
  场景文件 `.sim.json`（帧索引事件：pointer_move/down/up、click、wheel、key_down/up、text；
  `frame` 绝对或 `delay` 相对；`space` window/world；`loop` 循环），样例见
  `tests/data/sim/ball_clicks.sim.json`。驱动方式：启动期 `MAQUE_SIM=<file>`、简写
  `MAQUE_SIM_CLICK="frame:x:y[;...]"`、控制面 `Input.sim_load/sim_status/sim_events/sim_stop`。
  语义：`mq_sim_due` 取"到期或已过期"事件（按块步进不漏）；模拟事件与设备事件**同一条路径**
  并被录制覆盖 → 录制/回放等价；引擎内部一律世界坐标（设备事件在平台边界转换）。
- **输入帧缓冲必须预分配**：`mq_input_frame.pointers/pointer_cap`（step_frame 内静态缓冲）——
  容量为 0 时平台会静默丢弃**全部**设备事件（曾导致"真实鼠标点击永远到不了脚本"）。
- **VFS / 远程加载（docs/04）**：VFS 支持 `http(s)://` **目录挂载**（libcurl；逐段 percent-encode、
  404→`MQ_VFS_ERR_NOTFOUND`、同 URL 会话内只抓一次）。运行 `./build/samples/player/maque_player https://host/game.scn`
  即"远程游戏即资源"（场景/脚本/资源全走 VFS）；`MAQUE_MOUNT="/cdn=https://cdn.example.com/assets/,/x=../shared"`
  额外挂载（后挂载优先 → 可遮蔽远程目录，是"远程游戏 + 本地覆盖"的开发路径）；远程场景无文件监视热重载。
  **协议实现按目录分**：`modules/io/vfs/src/{file,mem,zip,url,builtin}/backend.cpp`（核心 `vfs.cpp`
  只做实例状态 + 最长前缀匹配 + 分发；共享工具 `util.cpp`，内部接口 `vfs_internal.h`）。
  测试：`ctest -R 'vfs|remote_e2e'`（本地 127.0.0.1 HTTP 服务器，不依赖外网）。
- **键值存储 / 存档（docs/25、storage 模块）**：`storage.set/get/exists/remove/keys/info`（脚本，JS/Lua 同一套绑定）
  —— 模式由 spec 决定：`file://<目录>`（默认 **`<场景目录>/.maque`**，场景在 CWD 时为 `<CWD>/.maque`）、
  `mem://`（不落盘）、`http(s)://`（REST：`GET/PUT/DELETE <base>/<uid>/<session>/<key>` + `GET <base>/<uid>/<session>?keys=1`）、
  `ws(s)://`（JSON 文本帧，协议名 `maque.storage.v1`）。命名空间 `MAQUE_UID`（默认 `default`）/
  `MAQUE_SESSION`（默认持久档 `main`；给运行期 id 即临时档）；`MAQUE_STORAGE` 覆盖模式。
  控制面：`Storage.info/list/get`（只读）· `Storage.put/remove`（变更类）。样例 `scenes/storage/`（JS 与 Lua 同逻辑）；
  测试：`ctest -R storage`（四模式单测，http/ws 用测试内建服务端）。
- **只有一个宿主：`samples/player`**。`samples/**` 里的游戏（flappy/tank/spine/scenes/**）**都没有自己的
  `main.cpp`**，一律用 `maque_player <场景>` 打开 —— 之前每个 sample 各带一份 main、彼此只差默认场景名，
  属于纯复制（已删除）。需要"默认场景/默认参数"就在脚本或 CI 命令里写，不要新增宿主。
- **场景 → 图片（截图/预览）**：`./build/samples/player/maque_player --shot <out.png|.ppm> [--shot-frame N] <scene.scn>`
  （`MAQUE_SHOT` / `MAQUE_SHOT_FRAME` 同义；默认第 1 帧；**截图后即退出**，一条命令得一张图）。
  **批量截图**（动画/物理"随时间变化"，只拍首帧看不出对错）：
  - `--shot-seq <tmpl> --shot-frames 1,30,60`：tmpl 里 `{}` → 帧号（无 `{}` 则插到扩展名前）；
    拍完最大帧即退出。等价环境变量 `MAQUE_SHOT_SEQ` / `MAQUE_SHOT_FRAMES`。
  - `--shot-every 20 --shot-count 4`：每 20 帧一张、共 4 张后退出（`MAQUE_SHOT_EVERY`/`MAQUE_SHOT_COUNT`）。
  - `--frames N`：最多跑 N 帧（`MAQUE_MAX_FRAMES`）。**到上限属于正常结束（退出码 0）**，
    不是失败 —— 之前返回 1，在脚本里看起来像出错。
  PNG 由 image 模块写出（zlib 可用则真压缩，否则 stored deflate）；尺寸 = 渲染目标（设计分辨率 × 视图 × `MAQUE_SS`）。
  测试：`ctest -R shot_e2e`（PNG 有效性/确定性/PPM/错误路径/**批量截图**/`--frames` 退出码）。VSCode 预览插件：`editors/vscode/`（`install.sh` 装到本地扩展目录；
  文档在该目录 README）。绑定面一致性：`ctest -R bindings_e2e`（schema → JS/Lua 探针，逐条检查可调用性）。
- **内建 shader（assets/builtin/shaders/）**：**一个 shader 一个目录**（目录名 = shader 名，
  目录内 `*.wgsl` 按文件名排序拼接），源码是唯一真相；配置期由 `cmake/shader_pack.cmake` +
  `tools/shader_pack.js` 打包成构建目录里的**压缩源码**（zstd -19；生成物不入库，改资产自动重打包）。
  运行时由 libzstd 解压并缓存（`MAQUE_BUILTIN_SHADER_COMPRESS=OFF` → 直嵌原文）。
  **可按 shader 剔除**（默认全部）：`cmake -S . -B build -DMAQUE_SHADER_TEXTURED=OFF`
  → 不生成/不链接/不进二进制（gfx 取不到时打印原因并跳过该批次）；手动重打包 `--target maque_shaders`。
  测试：`ctest -R builtin_shaders`（注册表 = 开关 + 解压文本与磁盘资产逐字节一致）。
- **墙钟限速（游戏速度 = 真实时间）**：主循环是固定步长（`dt = 1/60`），
  **真窗口**下按墙钟推进游戏时间 —— 机器比实时快就 `sleep` 压到 1x，
  比实时慢（渲染重，实测每帧 ~29 ms）就一轮补几步（上限 8 步，超出丢弃 → 变慢而不卡死）。
  以下情形**不限速**（保持"尽快出结果"，测试/CI 依赖它）：无窗口（`MAQUE_HEADLESS=1` 等）、
  `--shot`/`MAQUE_SHOT`、录制/回放。显式覆盖 `MAQUE_PACE=0|off` / `1|on`；
  诊断 `MAQUE_TRACE_PACE=1` 打印"墙钟 / 仿真 / 速率 / 睡觉 / 重锚定次数"。
  （历史坑：完全不限速时动画跟着机器速度跑，实测窗口下 **2.7 倍速**；
  只 sleep 不补步时，渲染慢的机器上又变成 **0.58 倍速**。）
- **两类图元：textured（实例化矩形/圆）与 mesh（任意三角网格）**：
  - `textured`：一个实例 = 一个矩形/圆，逐实例一次变换 —— 精灵/字形/UI 用。
  - `mesh`：**逐顶点** pos/uv/color + 索引缓冲 —— 骨骼动画（Spine）那种"一帧一变形的网格"用。
    顶点缓冲 stride 32B（pos.xy / uv.xy / col.rgba，见 `assets/builtin/shaders/mesh/shader.wgsl` 顶部），
    提交走 `mq_renderer_v1::render_mesh_views(self, tw, th, views, nviews, items, count, meshes, mesh_count, bg, out, out_size)`。
  - 两者**共用同一份相机 uniform 布局**（48B：size/view/cam/zoom/rot/pan/eye），且顶点都已是
    "渲染目标像素空间、y 向下"→ 变换逐项一致（**不要再翻 y**）。
  - 剔除开关：`-DMAQUE_SHADER_MESH=OFF` 时不生成/不链接，gfx 取不到时打印原因并跳过网格图元。
  - **成帧性能（勿回退）**：gfx 每视图只开**一个 render pass**，图元/网格命令在同一 pass 内切
    pipeline/bind group/scissor 完成（bind group 按 solid/textured/mesh 状态缓存）。
    ⚠️ 曾按"每条命令一个 pass"实现 → `all.scn` 331 条命令 = 331 次 BeginRenderPass，
    render 从 8.4ms 涨到 12.5ms；改成单 pass 后 Debug+ASan ~8.4ms / Release ~5.1ms。
  - **帧预算诊断**：`MAQUE_TRACE_STATS=1` 每 30 帧打印 `logic/render/present/draws/tris/target`。
    主循环每个模拟步都渲染+present 一次 → **屏幕 FPS ≈ 1000 / (logic+render+present) ms**；
    单步 >16.7ms 就上不了 60fps（Debug+ASan 构建比 Release 慢 ~2 倍，属预期）。
- **性能分析 Tracy（docs/29）**：默认**零开销**（客户端手动启动，普通运行不监听端口/不占线程）；
  游戏里 **F3** 启动客户端并拉起 profiler GUI，或 `MAQUE_TRACY=1` 启动即开（等外部连）。
  GUI/CLI 二进制不入库：Windows 跑 `tools/tracy/get-profiler.ps1` 下载官方包；
  Linux/macOS 跑 `tools/tracy/build-profiler.sh`（F3 的 GUI）与 `build-capture.sh`（CI 抓 trace），
  都装到 `.deps/tracy/`
  （`MAQUE_TRACY_PROFILER=<exe>` 可覆盖路径；`MAQUE_TRACY_PORT` 改端口，默认 8086）。
  埋点**只用** `modules/core/tracy` 的 `MQ_TRACY_*` 宏（模块颜色/命名/开关一处管理，
  `#include "core/tracy/mq_tracy.h"`）；只加在**外层入口**，不要进热内循环；新增模块色改
  `mq_tracy.h` 的 `Module`/`color()`。`-DMAQUE_TRACY=OFF` 时全部宏 no-op。
  ⚠️ Tracy 客户端配置必须含 **`TRACY_NO_CRASH_HANDLER`**（与 ASan 冲突：不关时 Debug+ASan 下
  `tracy-capture` 只能收到 3 帧/0 zone，关掉才正常）。
  无 GUI 抓 trace：`MAQUE_TRACY=1 MAQUE_WINDOW=0x0 ./maque_player <scene> &` +
  `.deps/tracy/tracy-capture.exe -o out.tracy -a 127.0.0.1 -p 8086 -s 2`；测试 `ctest -R tracy`。
- **节点属性对渲染生效（含层级累积）**：`node_set_angle`（度）/`node_set_scale`/`node_set_opacity`
  影响**自身与子树** —— 提取时沿父链累积（角度相加、缩放相乘、透明度相乘并夹 [0,1]），
  `node_set_active(id,0)` 隐藏整棵子树。所以"炮塔挂在车体下、各自转向"这类层级结构直接可用：
  子节点的角度是**相对父节点**的局部角。
  （历史坑：这些字段曾被提取整个忽略 → 四个 API 静默无效；渲染项 ABI 本来就有
  `rotation`/`a`/`half_w`/`half_h`，只是没填。）
- **脚本键码是"引擎规范键空间"**：`MQ_KEY_*`（空格=2、回车=3、Tab=4、退格=5、左右=6/7、上下=8/9）
  + **大写 ASCII**（'w' → 87）。真实设备事件在平台边界经 `key_from_sdl` 归一化，
  `.sim.json` 注入经 `mq_sim_normalize_key` 归一化 —— **两条路径同一套码**，脚本只需认一套。
  写 `.sim.json` 时用 SDL 风格码（空格 32、'w' 119、方向键 0x40000052 等）也可以，会被自动转换。
  （历史坑：两条路径语义不同 → 回放/热更/CI 与真机不一致；按 raw SDL 码判断按键的脚本永不触发。）
- **控制端点 / Inspector（plans/17、docs/09/11）**：`MAQUE_INSPECT=7777`（或 `tcp:7777` / `uds:/tmp/maque.sock`）
  在游戏内开一个**单线程、非阻塞、帧边界轮询**的 JSON-RPC 端点（与 stdin 控制面同一 dispatch，无线程 → 不改确定性）；
  默认**只读**（D2）：变更类方法（`control_list` 里 `control_mutating` 列出的）返回 `-32001` 并计入 `Control.info.rejected`；
  加 `MAQUE_INSPECT_RW=1` 才允许编辑。inspector 工具（独立进程，ImGui + SDL2 软件渲染）：
  `./build/samples/inspector/maque_inspector --connect 7777`（`--headless <方法…>` 用于 CI，无需显示器）。
  面板：场景树 / 属性（反射驱动，`Scene.get(id,"")` dump 组件全字段）/ 控制台（手写任意方法，与 MCP 同源）/
  **字体图集**（`Font.info`/`Font.glyphs`/`Font.atlas`：看动态图集的实时状态、缩放查看、悬浮显示码点与图集矩形）/
  **预览**（`Capture.screen`：分块传 PPM，可调刷新间隔与缩略图尺寸）/ 模式。
  界面字号：`--font-size`（默认 42px）、`--ui-scale`（连样式一起缩放）、`--tab <面板名>` 直接打开某面板；
  **键盘切页**：`1`~`5` 直选、`Ctrl+Tab` 循环（鼠标不便时用；状态栏有提示）；
  窗口默认**置顶**（`--no-on-top` / `MAQUE_INSPECTOR_ON_TOP=0` 关闭）与 `--pos x,y` 定位 ——
  否则容易被游戏窗口盖住（点击落到游戏上，表现为"tab 点不动"）。配对着用时游戏侧**不要**加 `MAQUE_ALWAYS_ON_TOP`。
  环境变量：`MAQUE_INSPECTOR_FONT_SIZE` / `MAQUE_INSPECTOR_UI_SCALE` / `MAQUE_INSPECTOR_ON_TOP` / `MAQUE_INSPECTOR_POS`；
  诊断/自测注入（本机 XTest 无法移动指针，故用这两条；两者都走真实事件路径）：
  `MAQUE_INSPECTOR_FAKE_KEY=3`（按键切页）、`MAQUE_INSPECTOR_FAKE_CLICK=x,y`（真实挪指针 + 三段式点击）、
  `MAQUE_INSPECTOR_FAKE_CLICK=sweep`（沿 tab 栏扫描点击，验证所有标签页可点）；
  配合 `MAQUE_INSPECTOR_TRACE=1` 打印鼠标 pos/down/clicked/悬停控件/悬停 tab/当前页。
  **点击被"抢回"的坑**：给标签页长期传 `ImGuiTabItemFlags_SetSelected` 会每帧强制选中，
  鼠标点选会被立刻覆盖；请求必须"满足后即清空"（请求页 / 当前页分离）。
  **二进制传输**：控制面是行分隔 JSON → 图集/画面走 **base64 分块**（`offset`/`max`，单块 ≤48KB，客户端在途上限 4 且偏移递增）；
  自建纹理必须 `ImGui::RegisterUserTexture()`（1.92+ 的外部纹理 API），否则后端不创建 SDL_Texture（白图）。
- **引擎侧 ImGui 调试面板（docs/28）**：`modules/ui/imgui` + `modules/runtime`。**默认不显示**（`MAQUE_IMGUI=1`
  或游戏内 **F2** 打开）；打开后 stats 面板（FPS/logic/render/draws/tris/frame/target）+ 74 个 `imgui_*` 脚本函数。**帧时序**：脚本阶段只
  **录制** ImGui draw list → 引擎渲染到 CPU 像素 → `mq_platform_draw_rect` 游戏帧进 `SDL_Renderer` 后台缓冲 →
  `mq_imgui_backend_render` 官方后端单独一趟 pass 叠画 → `mq_platform_present_ui` **只 present 一次**。
  **窗口呈现唯一通道 = SDL_Renderer，禁止 `SDL_GetWindowSurface`**（SDL2 不允许与 renderer 混用；
  混用会导致"imgui 一开、游戏画面不上屏"——已踩过）。面板拖动/缩放交给官方后端，**不要**再加
  `NoMove|NoResize`。无窗口/`MAQUE_PRESENT=off` 时回退自研网格链（面板进 `--shot` 像素缓冲）。
  **UI 度量固定 = 屏幕分辨率派生**（`imgui_resolve_metrics`：`ui_scale = 屏幕逻辑高/1080`，
  正文 15、标题 = 正文×4/3，面板 = 正文×16×正文×10；`MAQUE_IMGUI_SCALE` / `MAQUE_IMGUI_FONT_SIZE`
  = 正文字号 覆盖）：**窗口大小/场景分辨率/视图数不参与**，别再用 `win_w/TW` 当字体倍率
  （会把面板大小绑到窗口 → "标题太大/有的窗口又偏小"）。官方后端显示空间 = 绘制区**设备像素**，
  鼠标逻辑坐标按 content_scale 换算。
  测试：`ctest -R 'imgui|imgui_present_e2e'`（后者断言游戏上屏 + 拖动后位置变化 +
  换窗口后 UI 度量逐字一致 + software 回退）。
- **音频（docs/30）**：`modules/world/audio` **分层**——`maque_audio_core`（Mixer/解码，无 SDL）
  + `maque_audio`（C ABI 门面 + **可替换设备后端**）。只有 `src/backend_sdl.cpp` / `backend_null.cpp`
  碰设备；换 miniaudio/SoLoud 只需实现 `AudioDevice` 接口 + CMake 加一个源文件，核心/解码/ABI/runtime 不动。
  **默认 null 后端**（逻辑与脚本可用、CI 静音）；`MAQUE_AUDIO=sdl|auto|1` 开设备
  （CI 可 `SDL_AUDIODRIVER=dummy`）。脚本：`audio_play(path, volume, loop, bus)` /
  `audio_stop/pause/set_volume/set_pitch/state/playhead`、`audio_set_listener`、`audio_master_volume`、
  `audio_bus_volume`；`playhead` 按**模拟 dt** 推进（不进 checksum，docs/30 §5）。
  **组件**：`AudioSource`（`autoplay`、每帧 volume/pitch/空间化同步、`state/playhead/voice` transient 回写）与
  `AudioListener`（世界位置 + 节点 angle；`master_volume` 变化才下发）；字段用 **string 路径**（`clip/bus/curve`，
  不是 `asset_ref`，见 docs/27 §6）。`MQ_ASSET_KIND_AUDIO`（`.wav/.ogg/.mp3`）已加；
  **解码**：WAV/OGG（vendored `third_party/stb_vorbis`）/MP3（`third_party/dr_mp3`），解码器表在
  `modules/world/audio/src/decode_wav.cpp`。
  **离线抓音**：`MAQUE_AUDIO_CAPTURE=<wav>` → 强制 null 后端，每模拟步 `mq_audio_mix` 后写 PCM16 WAV
  （确定性；e2e 断言 RMS 非静音）。
  **控制面**：`Audio.info/voices`（只读）、`Audio.play/stop/set_master/set_bus`（变更类；voice 用字符串句柄）。
  测试：`ctest -R 'audio'`（单测 + `audio_e2e`：null/OGG/组件/抓音 + SDL dummy + `audio_control_e2e`）。
- WebGPU 需 `third_party/wgpu-native/lib/libwgpu_native.so`（二进制**不入库**）。
- 系统依赖：SDL2 / libcurl / libwebsockets（`libsdl2-dev libcurl4-openssl-dev libwebsockets-dev`）。
- 已 vendored：`third_party/quickjs-ng`（JS 后端）、`third_party/lua`（Lua 5.4 后端）、`third_party/wuffs`、
  `third_party/imgui`（master；**inspector 与引擎侧 `modules/ui/imgui` 共用**，见 docs/28）。

## 硬约定（改代码前先读）

1. **语言**：C++23（`CXX_STANDARD 23`）；纯 C 模块用 C11。
2. **输出**：一律 `std::print`（`#include <print>`），**不要 `std::printf`**；stderr 用 `std::print(stderr, ...)`；`FILE*` 场景用 `std::print(f, ...)`。
3. **跨模块接口**：只暴露 **C ABI**（`extern "C"` + 版本化 vtable，见 `docs/02`）。
   禁跨边界异常/RTTI/STL；**分配者释放**；引用用**句柄**（`{index,generation}`）而非裸指针。
4. **无全局可变状态**（多实例，不同实例各持一份）。
5. **确定性**：模拟单线程、固定步长、引擎 RNG、帧内不发异步、**脚本 cycle GC 仅帧边界**（`docs/10/14`）。
6. **不做无关注释**；不改与本任务无关的文件。
7. **构建产物不入库，但生成物入库**：
   - 不入库：`build/`、`third_party/wgpu-native/lib/*`。
   - **入库**：`modules/core/data/generated/`（生成的头/绑定/描述符/JSON/**API.md**/**TYPES.md**）
     —— 生成的公开接口是交付物，改 `schema/` 后必须重新生成并提交（`ctest -R codegen_drift` 会抓漂移）。

## 架构核心

- **声明 + 生成（单一真相 = `modules/core/data/schema/` 目录）**——**引擎的一切公开接口只声明一次**
  （`docs/21` §1；手写公开接口 = 漂移之源）：
  | DSL 段 | 声明内容 | 生成产物 | 编译目标 |
  |--------|----------|----------|----------|
  | `class_list` | 数据类型（struct/enum） | `maque_types.h` + `maque_desc.*`（含 `init_defaults` 默认构造器）+ `maque_schema.json` | `maque::schema` |
  | `api_list` | 引擎函数接口 | `maque_api.h`（声明）+ `maque_bindings.cpp`（**JS 绑定**） | `maque::api` / `maque::genbind` |
  | `interface` | 插件接口 | `maque_ifaces.h`（C vtable + 名称/版本宏） | 各模块 |
  | `control_list` | 控制面方法（AI 面，docs/11 §3） | `maque_control.h`（处理器声明）+ `maque_control_gen.cpp`（注册表 + 类型化 MCP `inputSchema`） | `maque::gencontrol` |
  | `callbacks` | 脚本事件回调（docs/14/19） | `maque_events.h` + `maque_events_gen.cpp`（回调枚举 + `on:` 输入映射 + 精确派发） | `maque::genevents` |
  | `module_abi` | 模块 C/C++ ABI（跨模块公开面，**20 个模块全覆盖**） | `abi/<module>.h`（纯 C 头 + `MQ_<MOD>_ABI_VERSION/IFACE_NAME` 宏；`language: cxx` + `namespace:` 时生成 C++ 命名空间声明） | 各模块（原手写头退化为 `#include` shim） |
  - 生成物在 `modules/core/data/generated/`（**源码树、随仓库提交**；构建配置期自动生成）；
    **改接口一律改 `schema/`（一个文件一个 namespace，目录镜像 `modules/`），不要改生成物或 shim 头**。
    人读文档：`generated/API.md`、`generated/TYPES.md`；schema 目录/import 规范：`modules/core/data/SCHEMA.md`。
  - 加一个 schema 文件 = 在 `schema/<区>/<模块>.yaml` 新建（目录层级镜像 `modules/`）+ 在入口
    `schema/schema.yaml` 登记；有依赖就写 `- imports: [./x.yaml]`（结构引用必须落在 import 闭包内，
    `ctest -R schema_imports` / `node parser.js --check` 会校验）。
  - **MANUAL**：`api_list` 项可带 `manual: "<header>"` → 生成器只 `#include` 该头（实现内联于头，不重复声明）。
  - 加控制面方法 = 在 `control_list` 加一条 + 实现 `mq_control_<Domain>_<method>`（签名见生成头）。
  - 加模块 C ABI 函数 = 在 `module_abi.fns` 加一条；结构体/枚举/宏同理（`structs`/`enums`/`macros`）。
  - ABI 由静态断言守卫（如 `modules/world/physics/tests/physics_test.cpp` 的尺寸/偏移断言），漂移即编译失败。
  - `module_abi` 支持 `opaque/typedefs/consts/macros(值/`value_cxx`/`raw`)/code/structs/enums/fns`；
    迁移工具：`tools/hdr2abi.py`（头 → DSL）、`tools/migrate_abi.py`（转换 + 合并进 `schema/`）。
    **改接口只改 `schema/`，不要改 shim 头或生成物**。
- **反射中枢**：`mq::reflect`（id/名/路径查询、标量读写、visitor）；持久化 `maque::persist` 反射驱动。
- **运行时（浏览器）**：`modules/runtime` 的 `mq_run_scene(path)`：加载 `.scn` → 反序列化 → 加载 `.js` → 固定步长循环 → WebGPU 渲染 → 退出。
  宿主 `samples/player` 仅一行；**游戏逻辑全在资源**（`scenes/*.scn`、`scenes/*.js`）。
- **渲染**：`maque::gfx`（wgpu-native）。注意：该构建的 `wgpuInstanceWaitAny` **未实现**，异步统一用 `wgpuInstanceProcessEvents` 轮询。
- **场景模型（单一模型，ADR-0003 2026-10 修订）**：**节点树（children + 反射组件）是唯一模型**；
  runtime 的实体表只是"`id → 节点` 的扁平索引 + 每帧 `Scene::flatten_world()` 展平出的世界坐标缓存"
  （`Entity::world_x/world_y` 是派生数据，**不是位置真相**——位置在 Transform 组件里，改 `world_*` 会被下一帧覆盖）。
  渲染/物理/UI 按**组件角色**从展平视图提取（只读）；**不做 ECS/archetype 存储**（高基数用专用数组，如粒子）。
  脚本两面同一模型：`node_*` 是树语义规范面，`entity_*` 是**扁平别名**（按 id 直接寻址）。
  控制面 `Scene.expand_prefabs` = 展开 PrefabInstance（旧名 `Scene.bake` 为兼容别名，已弃用）。

## 脚本可见 API（docs/18）

`api_list` 中的 `host` 命名空间即脚本全局函数（`global: true` → 无前缀）。本轮按参考实现的惯例补齐（对照见 docs/18）：

- **Node**：`node_set_parent/parent/child_count/child_at/find_child/find_path/set_name`、
  `node_set_position/position_x/position_y`、`node_set_scale/scale`、`node_set_angle/angle`、
  `node_set_opacity/opacity`、`node_set_z_index/z_index`、`node_width/height/set_content_size/set_anchor`、
  `node_set_active/active`、`node_destroy`（帧末生效）、`node_valid`
- **Component**：`node_has_component/add_component/remove_component/component_count/component_at/child_with_component`、
  `node_get_number/set_number/get_bool/set_bool/get_string/set_string`（组件字段名可用 `Type.field` 或短名）
- **事件**：`node_on(id, event, handler)` / `node_off` / `node_emit` / `node_emit_number`；emit 沿父链冒泡，
  处理器是脚本里的全局函数名（引擎按名字回调）
- **UI 值**：`ui_set_value/ui_get_value`（ProgressBar/Slider）、`ui_set_checked/ui_get_checked`（CheckBox）
- **Director**：`director_pause/resume/paused/frame/load_scene`
- **动作/定时**：`tween_start(id, "scn.Sprite.radius", to, duration, easing)`、`tween_stop/count/progress`、
  `schedule_once/schedule/unschedule/schedule_count`

**多脚本 / 跨语言（事件总线，docs/14 §多 VM）**：`scn.Scene.scripts: ["game.js", "ai.lua"]` →
每个脚本一个 VM，由**事件总线**统一投递：**顺序 = 声明顺序**、**只投给订阅者**、**载荷只有标量/字符串**、
**错误隔离**（`mq_script_bus_error_count` 增量判定 → 写崩溃上下文）、GC 逐 VM 在帧边界。
事件按**处理器名**路由（`node_on(id,"evt","on_evt")` + `node_emit_number(id,"evt",v)`）→ 语言无关，
JS 注册的处理器可由 Lua 触发，反之亦然。宿主/工具用控制面 **`Scene.emit`**（变更类方法，只读会话拒绝）。
样例：`scenes/multilang/`（JS 玩法 + Lua AI）；测试 `ctest -R multilang`。
**热重载（按文件、原子，docs/14 §热重载）**：`MAQUE_WATCH=1` 监视场景 + **全部脚本**；
只重建**改动的那一个** VM（其它脚本状态保留）、**槽位不变**（投递顺序不变）、
新 VM eval **成功才换入**（失败保留旧 VM → 热更出错不致命），换入后只对新 VM 派发一次 `on_start`
（重新注册事件处理器）。测试 `ctest -R hot_reload`。
⚠️ 长跑进程的日志在重定向到文件时是**全缓冲**：测试里要 `stdbuf -oL`，否则会误判"脚本没生效"。

**脚本后端（`.js` / `.lua`）**：语言按脚本扩展名选择（`"script": "game.lua"` → Lua 后端，
docs/14）。两后端**同一套 API 面与回调**（DSL 生成绑定；`math.clamp` 等在 Lua 侧合并进 `math` 标准库）。
Lua 为沙箱运行（无 `io/os/package/debug/require/collectgarbage`），`math.random` 由引擎 RNG 接管，
GC 仅在帧边界推进（`mq_script_frame_boundary`）。加后端见 docs/14「脚本后端」一节。

**文本**：`ui_set_text/ui_get_text/ui_text_width/ui_text_height/ui_text_lines`（对 `scn.Label` /
`ui.TextLabel` / `render.RichText` 通用，按 role 定位）。排版字段：`font_size` / `align`（或 `align_h`）/
`wrap`（或 `wrap_mode`）/ `max_width` / `line_spacing`；实测结果写回 transient 字段
（`text_w`/`text_h`/`line_count` 或 `text_size`/`content_size`）。
富文本标记：`<color=#RRGGBB>`、`<size=NN>`、`<b>`、`<br/>`；`parse_tags=false` 时按纯文本渲染。
**字体解析顺序**：`MAQUE_FONT=<ttf>` → **`scenes/fonts/` 下首个 `.ttf`（仓库自带，含中文）** →
常见系统字体 → 内置 5x7 位图字体（`tools/font_gen.py` 生成）。组件级覆盖：`scn.Label` /
`ui.TextLabel` / `render.RichText` 的 `font` 字段（相对工作目录或场景目录解析）。
**字形图集按需增长**（任意 Unicode/CJK）：ASCII 预热 95 字形，其余码点排版时惰性栅格化并打包；
`mq_font_atlas_generation` 变化 → 宿主刷新纹理（**旧纹理延迟到下一帧释放**，避免帧内悬空）；
排版按 **UTF-8 码点**（非字节），CJK 逐字可断行；文本图元**两阶段**提交：先全部排版（预热图集），
再按**最终图集尺寸**算 UV（否则先排的标签会用过期 UV 采样 → 错位/重叠）。

**文本 pixel-perfect（FreeType 集成）**：排版在**设备像素**空间进行 ——
`mq_text_style.device_scale`（= 窗口/目标缩放）决定实际栅格化字号（`round(font_size × scale)`，整数 ppem →
笔画落在像素网格），`pixel_snap=1` 把笔位置与字形位图左上角吸附到整数像素；
`mq_text_glyph` 输出位图矩形（`x0/y0` + `w/h`，已按 FreeType `bitmap_left/bitmap_top` 定位）。
渲染侧固定用 **矩形图元 + `filter=nearest` + `snap=1`**（方形会拉伸字形，线性会糊）。
硬证据：`ctest -R text_pixel` —— 渲染结果与字体模块位图**逐像素一致**（渗出 0、max_diff 0），
且内置"线性 + 半像素偏移"对照组证明测试有鉴别力。

**画布 / 分辨率策略（`ui.Canvas`）**：UI 按**设计分辨率**排版，运行时适配任意窗口与显示器。
`fit_mode`（None/FitWidth/FitHeight/ShowAll/NoBorder/Exact）+ `align` + `min/max_scale` + 安全区；
解析结果写回 transient（`scale/scale_x/scale_y/offset_x/offset_y/viewport_*/window_*/device_scale`）。
呈现、输入映射、文字栅格化**共用同一份映射**（错一处就错位）；策略数学在 `modules/ui/canvas`（纯函数 + 单测）。
脚本：`screen_w/h()`=设计分辨率，`window_w/h()`=设备像素，`canvas_scale()/canvas_offset_x/y()/canvas_device_scale()`。
测试/CI：`MAQUE_DPI_SCALE=2` 模拟 HiDPI，`MAQUE_SAFE_AREA="l,t,r,b"` 模拟刘海；`ctest -R 'canvas'` 覆盖。

**坐标约定**：节点位置、**输入事件、脚本可见坐标统一为世界坐标（y 向上）**；
设备事件在平台边界做一次"窗口 → 世界"转换（等比缩放 + 居中留边 + y 翻转），注入/模拟事件按世界坐标给出。
渲染时再翻到屏幕坐标（`y_screen = H - y_world`）。UI 控件（`ui.Control`）以**中心**为锚点。
文本排版坐标 y 向下，屏幕映射为 `y_screen = H - (node_y + label_y) + glyph_y`。

## 常见改动怎么做

- **加一个数据类型/组件**：编辑 `modules/core/data/schema/<模块>.yaml` 的 `class_list` → 重构建（自动重生成）。
- **加一个引擎接口**：在 `api_list` 加 `fn/args/returns`（标量/string）；实现放 `modules/api/src/api.cpp`；
  要内联实现则用 `manual: "<header>"` 并把头放到 `modules/api/include/`。
- **加一个模块**：`modules/<area>/<name>/`，含 `include/`、`src/`、`tests/`、`CMakeLists.txt`；
  在根 `CMakeLists.txt` `add_subdirectory`；导出 `maque::<name>` 静态库 + `add_test`。
- **脚本里调用引擎**：用生成绑定（如 `math.clamp(...)`）；宿主 `log/cfg/draw_sprite/entity_*` 由 runtime 注册。

## 测试与代码风格

- 每个模块自带**独立单测**（`tests/`），用简易 `CHECK` 宏，经 `add_test` 挂到 CTest。
- 格式/静态检查：`.clang-format`、`.clang-tidy`（导出 `compile_commands.json`）。
- 经典用法：先加/改接口与测试，再实现。

## 文档地图

- 设计文档：`docs/`（根文档 `docs/00-设计哲学与约束.md`；索引 `docs/README.md`）。
- 接口绑定机制：`docs/21-接口声明与绑定生成.md`；**API 参考（自动生成）**：`docs/api/README.md` → `modules/core/data/generated/API.md`。
- 运行时/资源模型：`docs/20-运行时与资源模型.md`。
- 里程碑/路线图：`docs/milestones/00-路线图.md`。
- 各模块开发计划：`plans/`（`plans/README.md`）。
