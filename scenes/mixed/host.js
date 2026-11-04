// host.js —— JS 根场景（docs/34 P0 新路径：单 VM 模块 + 每挂载点实例）。
// 子场景 worker.lua 走旧路径（Lua 尚未模块化，P2）；两者用 scene_emit 做跨语言请求/响应。
// scene_emit(instance, event, value) 按**事件名**在目标实例上找同名处理器（docs/32 §16）。
globalThis.__mixed_host_evals = (globalThis.__mixed_host_evals || 0) + 1;

export default function () {
  var S = { frame: 0, asks: 0, replies: 0, workers: [] };

  function on_start() {
    // 新路径的 this 上下文：node=-1（场景级）、package=id@version。
    log('MIX_HOST_START node=' + this.node + ' package=' + this.package +
        ' evals=' + globalThis.__mixed_host_evals);
    S.workers = [];
    for (var i = 0; i < scene_instance_count(); i++) {
      var id = scene_instance_at(i);
      if (id != scene_self()) S.workers.push(id);
    }
    S.workers.sort(function (a, b) { return a - b; });
    log('MIX_HOST_WORKERS=' + S.workers.join(','));
  }

  function update(dt) {
    S.frame++;
    if (S.frame === 2) {
      // JS → Lua：每个子实例发一个 ask（按实例 id 升序，确定性）。
      for (var i = 0; i < S.workers.length; i++) {
        S.asks++;
        scene_emit(S.workers[i], 'ask', 100 + i);
      }
    }
    if (S.frame === 8) log('MIX_HOST_SUMMARY asks=' + S.asks + ' replies=' + S.replies);
    if (S.frame === 8) {
      var st = entity_find('Status');
      if (st >= 0) node_set_string(st, 'scn.Label', 'text',
        'JS asks=' + S.asks + ' / Lua replies=' + S.replies);
    }
  }

  // Lua → JS：子实例 scene_emit(0, 'reply', v) → 同名方法（事件名 = 处理器名）。
  function reply(id, value) {
    S.replies++;
    log('MIX_HOST_REPLY from=' + id + ' value=' + value + ' replies=' + S.replies);
  }

  return { on_start, update, reply };
}
