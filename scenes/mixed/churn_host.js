// churn_host.js —— 内存回归：JS 根（P0 新路径）反复实例化/卸载 Lua 子场景（旧路径）。
// 每次 spawn 都会创建一个 Lua VM；unload 必须把它连同节点/上下文一起回收。
export default function () {
  var frame = 0, prev = -1, spawned = 0, failed = 0;

  function on_start() { log('MIX_CHURN_START'); }

  function update(dt) {
    frame++;
    if (frame < 2) return;
    if (prev >= 0) { scene_unload(prev); prev = -1; }
    var id = scene_instantiate(-1, 'worker.scn', '{"tag":"D"}');
    if (id < 0) {
      failed++;
      log('MIX_CHURN_FAIL frame=' + frame + ' err=' + scene_last_error());
    } else {
      prev = id;
      spawned++;
    }
    if (frame % 100 === 0)
      log('MIX_CHURN frame=' + frame + ' spawned=' + spawned + ' failed=' + failed +
          ' count=' + scene_instance_count());
  }

  return { on_start, update };
}
