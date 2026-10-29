# Maque Android 模板

`android/` 是可直接由 Android Studio 打开的应用工程。它使用 SDL2 的
`SDLActivity` 承载 maque 运行时，并保持“游戏即资源”：Java 层只选择场景，实际加载仍走
`mq_run_scene("asset://...")`。

## 默认行为

- Gradle 把仓库 [`scenes/`](../scenes/) 的**全部内容**作为 APK assets 根目录打包。
- `generateMaqueSceneIndex` 在构建前递归扫描所有 `*.scn`，生成
  `maque/scenes.json`；路径按字典序固定，构建结果可复现。
- 首页罗列索引中的全部场景。点击任意条目立即启动。
- 场景运行页底部保留 `Prev` / `Next`，按索引循环切换；最后一个场景的 `Next`
  回到第一个，第一个场景的 `Prev` 回到最后一个。
- APK 资源由 VFS 的 `asset://` 只读后端读取，场景引用的脚本、纹理、音频和子场景仍按
  场景目录解析。
- Android 默认使用离屏 WebGPU + SDL 呈现；窗口/Surface 的暂停、销毁和重建由 SDL2 管理。

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

脚本固定获取 SDL `release-2.32.10` 到 `android/.deps/SDL`。也可通过
`-PmaqueSdl2Dir=<目录>` 指向其它兼容 SDL2 checkout。

## wgpu-native

模板默认只构建 `arm64-v8a`，需要与仓库头文件匹配的 wgpu-native `v29.0.1.1`：

```text
android/app/src/main/jniLibs/arm64-v8a/libwgpu_native.so
```

从官方 Release 下载并解压：

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
