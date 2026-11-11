export default function () {
  function spot(x, rotX, intensity) {
    const id = entity_spawn();
    node_set_local_position(id, x, 5.0, 0.0);
    node_set_local_rotation(id, rotX, 0, 0);
    node_add_component(id, 'render.Light');
    node_set_number(id, 'render.Light', 'type', 2);
    node_set_number(id, 'render.Light', 'intensity', intensity);
    node_set_number(id, 'render.Light', 'range', 40.0);
    node_set_number(id, 'render.Light', 'inner_angle', 20.0);
    node_set_number(id, 'render.Light', 'outer_angle', 40.0);
    node_set_number(id, 'render.Light', 'shadow_mode', 0);
    node_set_number(id, 'render.Light', 'color.r', 1.0);
    node_set_number(id, 'render.Light', 'color.g', 0.9);
    node_set_number(id, 'render.Light', 'color.b', 0.7);
    return id;
  }

  function on_start() {
    // 左：-90° 朝下（-Z 约定）；中：+90°（+Z 约定）；右：-90° 高亮度
    spot(-12, -1.5708, 20);
    spot(0, 1.5708, 20);
    spot(12, -1.5708, 200);
    log('lightprobe spawned 3 spots');
  }
  return { on_start };
}
