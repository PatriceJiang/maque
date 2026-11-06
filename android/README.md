# Maque Android 模板

`android/` 是可直接由 Android Studio 打开的应用工程。它使用 SDL2 的
`SDLActivity` 承载 maque 运行时，并保持“游戏即资源”：Java 层只选择场景，实际加载仍走
`mq_run_scene("asset://...")`。

## 默认行为

- Gradle 把仓库 [`scenes/`](../scenes/) 的**全部内容**作为 APK assets 根目录打包。
- `generateMaqueSceneIndex` 在构建前递归扫描所有 `*.scn`，生成
  `maque/scenes.json`；路径按字典序固定，构建结果可复现。
- 首页/场景列表默认**竖屏**；进入场景后运行页切到**横屏**（保持场景宽高比），
  点左下角 `Home` 回到竖屏主页（保留当前场景索引）。
- 场景运行页底部保留 `Home` / `Prev` / `Next`；`Prev`/`Next` 按索引循环切换。
- 运行页右上角有触屏 `F2` / `F4`：F2 开关 stats 面板，F4 开关引擎选项面板
  （与桌面同一条热键路径；Android 首次启动会把 `scenes/fonts/*.ttf` 复制到内部存储，
  供 ImGui 加载中文字体）。
- 触摸统一从 `renderSurface` 的 `MotionEvent` 归一化后走 JNI `nativeTouch` →
  `mq_platform_android_touch` 入引擎队列，不经过 SDL 的本地坐标换算（旋转、沉浸式、
  多窗口下会错位）；只有 ImGui **真正命中窗口**的指针会被面板消费，面板外空白仍交给
  场景/UI；面板内空白区域也可拖动（不限于标题栏）。
- F2/F4 的默认位置在 Android 上额外下移 24dp，避开通知栏/刘海；用户拖动后的
  窗口位置写入内部存储 `files/imgui.ini`，跨面板关闭和 App 重启都会恢复。
- F4 提供 **RenderFactor (3D)**（100% / 75% / 50% / 25%）与 **ImGuiFactor**
  （100% / 150% / 200% / 300% / 400% / 500%）。前者只把 3D PBR 画到低分辨率离屏 target
  再合成，2D/UI/字体/ImGui 保持全分辨率；后者只放大/缩小 ImGui 面板和字体（>400% 使用紧凑字形集）。
  两者写入 `files/engine_options.ini`，重启恢复；触控坐标始终按主 target。
- 真机默认 **wgpu 直呈**（不再每帧 GPU→CPU 回读；F2 实测 60fps / render ~1ms）。
  直呈 attach/config 失败会回退离屏 CPU 路径；临时强制 CPU：
  `adb shell setprop debug.maque.present software`（Waydroid 模拟器仍默认 CPU，
  用同一属性可强制 wgpu）。
- Waydroid 开发：默认 APK 是 `arm64-v8a`；在 x86_64 Waydroid 上构建/安装用
  `./gradlew :app:assembleDebug -PmaqueAbis=x86_64`，再
  `adb connect 192.168.240.112:5555`（首次需把宿主 `~/.android/adbkey.pub` 写进
  Waydroid `/data/misc/adb/adb_keys`）。
- APK 资源由 VFS 的 `asset://` 只读后端读取，场景引用的脚本、纹理、音频和子场景仍按
  场景目录解析。
- 真机：SDL SurfaceView 只负责输入/生命周期，独立 RGBA Render SurfaceView 专供 wgpu；默认
  `render_scale=1` 时直接把场景绘制到全尺寸 ANativeWindow swapchain，不创建最终中间颜色纹理。
  只有输出倍率使渲染目标与 surface 不同时才用 blit shader 缩放；全程 GPU、无 readback/memcpy。
  Surface 句柄变化时自动 reattach，acquired texture 在 present 后释放。
- AVD：wgpu-native 在 Android Emulator SurfaceView 上持续返回 swapchain acquire timeout，模板按
  `ro.kernel.qemu=1` 明确使用 `ANativeWindow_lock` CPU fallback；保持窗口自动渲染分辨率，但将
  Surface 提交节流到约 15 Hz，用于功能调试，不代表真机性能。
- Android 默认不启动 SDL 文本输入，因此不会自动弹软键盘；需要时显式设置 `MAQUE_TEXT_INPUT=1`。
- 3D 场景的主相机支持触摸手势：**单指拖动环绕、双指拖动平移、双指捏合缩放**。
  触摸事件仍走 `SDL_FINGER* → pointer_type=Touch`，2D 场景的脚本/UI pointer 回调不受影响；
  `ctest -R touch_orbit_e2e` 是桌面/CI 的注入回归。

## 前置条件

- Android Studio，Android SDK 35，NDK `27.3.13750724`，CMake `3.22.1`。
- JDK 17（Android Gradle Plugin 8.9 要求）。
- Node.js；首次在 [`modules/core/data/`](../modules/core/data/) 执行 `npm install`。
- SDL2 源码。执行其一：

```powershell
powershell -ExecutionPolicy Bypass -File android\setup-deps.ps1
```

```bash
sh ./android/setup-deps.sh
```

脚本固定获取 SDL `release-2.32.10` 到 `android/.deps/SDL`，并同时下载、校验
wgpu-native `v29.0.1.1` 的 Android `.so`（见下一节）。也可通过
`-PmaqueSdl2Dir=<目录>` 指向其它兼容 SDL2 checkout。

## wgpu-native

模板默认只构建 `arm64-v8a`，需要与仓库头文件匹配的 wgpu-native `v29.0.1.1`：

```text
android/app/src/main/jniLibs/arm64-v8a/libwgpu_native.so
```

`setup-deps` 脚本会按 `MAQUE_ABIS`（默认 `arm64-v8a`）下载对应的 Release、校验
SHA-256 后放进 `jniLibs/`；文件已存在且校验通过时直接跳过：

```bash
MAQUE_ABIS=arm64-v8a sh ./android/setup-deps.sh
```

需要手工放置（或使用其它版本）时，从官方 Release 下载并解压：

```powershell
Invoke-WebRequest `
  https://github.com/gfx-rs/wgpu-native/releases/download/v29.0.1.1/wgpu-android-aarch64-release.zip `
  -OutFile .deps\wgpu-android-aarch64-release.zip
Expand-Archive .deps\wgpu-android-aarch64-release.zip .deps\wgpu-android-aarch64-release
New-Item -ItemType Directory -Force android\app\src\main\jniLibs\arm64-v8a | Out-Null
Copy-Item .deps\wgpu-android-aarch64-release\lib\libwgpu_native.so `
  android\app\src\main\jniLibs\arm64-v8a\
```

```bash
curl -L -o .deps/wgpu-android-aarch64-release.zip \
  https://github.com/gfx-rs/wgpu-native/releases/download/v29.0.1.1/wgpu-android-aarch64-release.zip
unzip .deps/wgpu-android-aarch64-release.zip -d .deps/wgpu-android-aarch64-release
mkdir -p android/app/src/main/jniLibs/arm64-v8a
cp .deps/wgpu-android-aarch64-release/lib/libwgpu_native.so \
  android/app/src/main/jniLibs/arm64-v8a/
```

该二进制是本机依赖，不入库。不要使用 `wgpu-linux-aarch64` 包替代 Android 包。

本地 x86_64 AVD 调试时，用同一脚本取 x86_64 的 `.so`
（`android/app/src/main/jniLibs/x86_64/libwgpu_native.so`），然后构建：

```bash
MAQUE_ABIS=arm64-v8a,x86_64 sh ./android/setup-deps.sh
cd android
./gradlew -PmaqueAbis=x86_64 :app:assembleDebug
```

`maqueAbis` 接受逗号分隔列表；不指定时仍只构建 `arm64-v8a`。

## 构建

1. 用 Android Studio 打开 [`android/`](./)。
2. 等待 Gradle Sync 完成。
3. 选择 `app`，构建或运行 Debug 变体。

命令行可使用工程自带的 Gradle Wrapper：

```bash
cd android
./gradlew :app:assembleDebug
```

Windows 使用 `gradlew.bat :app:assembleDebug`。

APK 位于 `android/app/build/outputs/apk/debug/app-debug.apk`。安装到已授权手机：

```bash
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n org.maque.player/.MainActivity
```

## 工程落点

- [`app/build.gradle`](app/build.gradle)：`scenes/` assets 接入、场景索引生成、NDK 参数。
- [`MainActivity.java`](app/src/main/java/org/maque/player/MainActivity.java)：场景列表与启动。
- [`MaqueActivity.java`](app/src/main/java/org/maque/player/MaqueActivity.java)：SDL 运行页与循环切换。
- [`main.cpp`](app/src/main/cpp/main.cpp)：薄宿主，只把 Java 传入的 `asset://` 路径交给运行时。

增加或删除 `scenes/` 中的场景后直接重建 APK，不需要维护手写清单。若本机存在
`scenes/khronos/*/assets` 等未入库派生资源，它们也会按“全部内容”的规则进入 APK；发布前应关注 APK 体积。
