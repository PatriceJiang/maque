// 先渲染 A（红色纹理），2 帧后切到同目录结构的 B（蓝色纹理，**相对路径相同**）。
// 回归：切场景时若不清 tex_cache（键=相对路径），B 会错误复用 A 的贴图。
var frames = 0;
function update(dt) {
  frames++;
  if (frames == 2) {
    var dir = scene_dir();
    if (dir.charAt(dir.length - 1) != '/') dir += '/';
    director_load_scene(dir + '../b/scene.scn');
  }
}
