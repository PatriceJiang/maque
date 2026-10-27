'use strict';
// 父场景脚本（docs/32）：能看到自己的实例树；动态实例化/卸载走帧边界 API。
var frames = 0, static_ids = [], dyn_id = -1, left = -1, right = -1;

function on_start() {
  log('PARENT_START children=' + scene_instance_count() + ' self=' + scene_self());
  left = node_find_path(-1, 'Left');
  right = node_find_path(-1, 'Right');
  log('PARENT_SHELLS left=' + left + ' right=' + right);
  node_on(left, 'from_child', 'parent_on_child');
  node_on(right, 'from_child', 'parent_on_child');
  for (var i = 0; i < scene_instance_count(); i++) {
    var inst = scene_instance_at(i);
    if (inst != scene_self()) static_ids.push(inst);
  }
  log('PARENT_STATIC=' + static_ids.join(','));
}

function parent_on_child(id, v) { log('PARENT_CHILD_EVENT target=' + id + ' v=' + v); }

function update(dt) {
  frames++;
  if (frames == 2) { log('PARENT_EMIT inst=' + static_ids[0]); scene_emit(static_ids[0], 'ping', 7); }
  if (frames == 4) {
    dyn_id = scene_instantiate(-1, 'child.scn');
    log('PARENT_SPAWN dyn=' + dyn_id + ' err=' + scene_last_error());
  }
  if (frames == 6) { scene_unload(static_ids[0]); log('PARENT_UNLOAD inst=' + static_ids[0]); }
  if (frames == 8) { scene_unload(dyn_id); log('PARENT_UNLOAD dyn=' + dyn_id); }
  if (frames == 9) { scene_emit(static_ids[1], 'report', 0); }
  if (frames == 10) {
    log('PARENT_FINAL children=' + scene_instance_count() + ' dyn_valid=' + scene_instance_valid(dyn_id));
  }
}
