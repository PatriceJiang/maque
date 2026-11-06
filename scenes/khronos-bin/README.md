# khronos-bin

与 `scenes/khronos` 相同的画廊（`index.scn` + `gallery.js`）与每个模型的 **相机/灯光/场景 env**，
但模型改为 runtime 直读的源 `.glb`（`assets/<Model>.glb`），不再经过离线 `.mesh.json` 资产。

用途：对比两种加载路径的速度（`tools/bench_khronos_load.py`）。

生成：`python tools/make_khronos_bin_scenes.py`（缺失 GLB 时会调用 `npx @gltf-transform/cli copy`）。
本目录的 `assets/*.glb` 是派生资产（Git LFS 管理）；`EXT_texture_webp` 模型在生成时
用 Pillow 转成 JPEG/PNG 并重写 GLB（运行时直读无 WebP 解码器）；
`KHR_materials_pbrSpecularGlossiness` 模型逐像素转换成 baseColor + metallicRoughness；
非三角 primitive（点/线/strip/fan）展开成三角形（同离线 importer 近似）。
HDRI 环境：runtime 按场景目录自动探测 `<repo>/.deps/khronos-env`；有 env_texture 的场景默认显示天空盒（`env_background=false` 可关闭），缺失时回退 analytic IBL。
