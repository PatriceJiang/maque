// render_texture_2d.js —— 2D 相机离屏成帧：相机只拍 UI 层（layer_mask=2 + include_ui），
// 把 scn.Label 的逐帧旋转写进 RenderTexture，再由主画面的 Sprite 采样同一张 GPU 纹理。
'use strict';

const S = { t: 0.0, label: -1, camera: -1, centered: false };

function on_start() {
  S.label = entity_find('rotating_text');
  S.camera = entity_find('capture_camera_2d');
  log('render texture 2d=' + camera_texture(S.camera));
  camera_update(S.camera);
}

function update(dt) {
  S.t += dt;
  // 首次排版完成后才知道文本尺寸：用 x/y 偏移把文本块中心移到节点位置，
  // 这样 node_set_angle 的旋转轴心（节点位置）正好是文本中心。
  if (!S.centered) {
    const w = ui_text_width(S.label);
    const h = ui_text_height(S.label);
    if (w > 0.0 && h > 0.0) {
      node_set_number(S.label, 'scn.Label', 'x', -w * 0.5);
      node_set_number(S.label, 'scn.Label', 'y', h * 0.5);
      S.centered = true;
      log('render texture 2d text=' + w.toFixed(1) + 'x' + h.toFixed(1));
    }
  }
  node_set_angle(S.label, S.t * 60.0);
}
