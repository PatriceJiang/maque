// game.js —— 玩法/呈现（JS）。AI 在 ai.lua：两者**只通过事件总线**协作（跨语言）。
// 事件语义（docs/18/19）：node_on(id, 事件, "处理器名") 注册；node_emit_number(id, 事件, 数值) 触发。
//   载荷 = 两个数值（target 实体 id + value）→ 跨语言只传标量（事件即消息）。
// 派发顺序 = 场景里 scripts 的声明顺序（先 game.js 后 ai.lua）→ 确定性。
(function () {
  'use strict';
  const S = (globalThis.__maque = globalThis.__maque || {});
  const N = 6;
  const entities = [];

  globalThis.on_start = function () {
    for (let i = 0; i < N; ++i) {
      const id = entity_spawn();
      node_add_component(id, 'scn.Transform2D');
      node_add_component(id, 'scn.Sprite');
      node_set_position(id, 40 + i * 40, 120);
      node_set_number(id, 'scn.Sprite', 'width', 18);
      node_set_number(id, 'scn.Sprite', 'height', 18);
      node_set_number(id, 'scn.Sprite', 'r', 0.45);
      node_set_number(id, 'scn.Sprite', 'g', 0.75);
      node_set_number(id, 'scn.Sprite', 'b', 1.0);
      entities.push(id);
    }
    S.title = entity_find('Title');
    S.status = entity_find('Status');
    S.frames = 0;
    S.decisions = 0;

    // 注册"本语言"要处理的事件（处理器名在任意 VM 里都能被总线找到 → 天然跨语言）
    node_on(S.title, 'ai_ready', 'on_ai_ready');       // ← 由 ai.lua 触发
    node_on(S.status, 'ai_target', 'on_ai_target');    // ← 由 ai.lua 触发
    log('ml_js_ready n=' + N);
    // 注意：此刻 ai.lua 的 on_start 还没跑（顺序 = 声明顺序），它尚未注册处理器 →
    // 首个跨语言事件放到第一帧 update 再发（此时两个脚本都已初始化）。
    S.init_sent = false;
  };

  // Lua → JS：AI 就绪
  globalThis.on_ai_ready = function (target, count) {
    log('ml_js_got_ai_ready count=' + count);
  };

  // Lua → JS：AI 决定的高度 → JS 负责把某个实体移过去（呈现归 JS）
  globalThis.on_ai_target = function (target, y) {
    const i = S.decisions % entities.length;
    node_set_position(entities[i], 40 + i * 40, y);
    S.decisions++;
    if (S.decisions % 10 === 0) log('ml_js_applied=' + S.decisions + ' y=' + Math.round(y));
  };

  globalThis.update = function (dt) {
    S.frames++;
    if (!S.init_sent) {                                // 第一帧：两个脚本都已注册 → 跨语言初始化
      S.init_sent = true;
      node_emit_number(S.title, 'ai_init', N);         // JS → Lua（跨语言）
    }
    if (S.frames % 30 === 0) node_emit_number(S.status, 'ai_tick', S.frames);   // JS → Lua 周期性请求
    if (S.frames % 60 === 0)
      node_set_string(S.status, 'scn.Label', 'text',
        'JS 帧 ' + S.frames + '   AI 决策 ' + S.decisions + '（Lua）');
    if (S.frames === 150) log('ml_summary applied=' + S.decisions + ' frames=' + S.frames);
  };
})();
