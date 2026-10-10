# M4 · VFS 与资源加载

> 上游：`docs/04`（VFS/多协议）、`docs/20`（运行时与资源）。
> 目标：资源（场景/脚本/后续纹理字体）统一经**虚拟文件系统**加载。

## 1. M4a · VFS（已实现）

- 新增 `modules/io/vfs`（`maque::vfs`）：
  - `create/destroy`、`mount(mountpoint, source)`、`exists`、`read`（返回分配缓冲）、`free`。
  - **最长前缀匹配 + 后挂载覆盖**；`file://` 后端（本地目录）。
- 运行时改为**经 VFS 加载**场景与脚本：把场景目录挂为根 `/`，`read("/ball.scn")`、`read("/ball.js")`。
- 单测：挂载/读取/最长前缀覆盖/缺失文件 → 通过。

## 2. 验收（实测）

```
runtime: scene 'scenes/ball.scn' script='ball.js' ...
runtime: loaded 1 entities from scene
[game] ball: use scene entity id=0 at (160,200)
runtime: exited after 436 frames (quit=1)
```
ctest：`vfs` + 原有用例，共 **9/9** 通过。

## 3. 遗留 / 下一步

- **更多后端**：`mem://`、`res://`、`zip://`、`https://`（docs/04）。
- **异步读**（coroutine，docs/05）。
- **纹理/字体资源**：经 VFS 加载图像/字体，WebGPU 纹理 + 文本渲染（M4b）。
- **内容哈希校验**（远程，ADR-0030）。
