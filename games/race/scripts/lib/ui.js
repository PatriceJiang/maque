/* 2D UI 小工具：动态建 Label / Sprite（菜单与 HUD 共用）。 */

export function label(x, y, size, text, rgb) {
  const id = entity_spawn();
  node_add_component(id, 'scn.Transform2D');
  node_set_number(id, 'scn.Transform2D', 'x', x);
  node_set_number(id, 'scn.Transform2D', 'y', y);
  node_add_component(id, 'scn.Label');
  node_set_number(id, 'scn.Label', 'font_size', size);
  node_set_string(id, 'scn.Label', 'text', text);
  setLabelColor(id, rgb);
  return id;
}

export function setLabelText(id, text) {
  node_set_string(id, 'scn.Label', 'text', text);
}

export function setLabelColor(id, rgb) {
  node_set_number(id, 'scn.Label', 'r', rgb[0]);
  node_set_number(id, 'scn.Label', 'g', rgb[1]);
  node_set_number(id, 'scn.Label', 'b', rgb[2]);
}

export function rect(x, y, w, h, rgb, alpha) {
  const id = entity_spawn();
  node_add_component(id, 'scn.Transform2D');
  node_set_number(id, 'scn.Transform2D', 'x', x);
  node_set_number(id, 'scn.Transform2D', 'y', y);
  node_add_component(id, 'scn.Sprite');
  node_set_number(id, 'scn.Sprite', 'width', w);
  node_set_number(id, 'scn.Sprite', 'height', h);
  node_set_number(id, 'scn.Sprite', 'r', rgb[0]);
  node_set_number(id, 'scn.Sprite', 'g', rgb[1]);
  node_set_number(id, 'scn.Sprite', 'b', rgb[2]);
  node_set_number(id, 'scn.Sprite', 'a', alpha === undefined ? 1.0 : alpha);
  return id;
}

export function moveTo(id, x, y) {
  node_set_number(id, 'scn.Transform2D', 'x', x);
  node_set_number(id, 'scn.Transform2D', 'y', y);
}
