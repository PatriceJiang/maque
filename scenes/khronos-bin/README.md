# khronos-bin

与 `scenes/khronos` 相同的画廊（`index.scn` + `gallery.js`）与每个模型的 **相机/灯光/场景 env**，
但模型改为 runtime 直读的源 `.glb`（`assets/<Model>.glb`），不再经过离线 `.mesh.json` 资产。

用途：对比两种加载路径的速度（`tools/bench_khronos_load.py`）；实测结果见 [BENCH.md](BENCH.md)。

生成：`python tools/make_khronos_bin_scenes.py`（缺失 GLB 时会调用 `npx @gltf-transform/cli copy`）。
本目录的 `assets/*.glb` 是派生资产，体积大，不提交（见 `.gitignore`）。
