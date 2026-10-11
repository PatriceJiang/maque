// text.js —— Label/RichText 语义演示：排版度量、换行、富文本 run、动态改字号与文本
(function () {
  'use strict';
  const S = (globalThis.__maque = globalThis.__maque || { log: [] });

  globalThis.on_start = function () {
    S.title = entity_find('Title');
    S.wrapped = entity_find('Wrapped');
    S.centered = entity_find('Centered');
    S.rich = entity_find('Rich');
    // 节点位置（ui.* 组件以节点位置为文本左上角）
    node_set_position(S.centered, 100.0, 150.0);
    node_set_position(S.rich, 10.0, 130.0);
    S.wrapped_lines = node_get_number(S.wrapped, 'scn.Label', 'line_count');
    S.wrapped_w = node_get_number(S.wrapped, 'scn.Label', 'text_w');
    S.rich_h = node_get_number(S.rich, 'render.RichText', 'text_size');
    log('text.js ready');
  };

  globalThis.update = function () {
    S.frames = (S.frames || 0) + 1;
    if (S.frames === 4) {
      // 动态改文本与字号 → 下一帧重新排版
      node_set_string(S.title, 'scn.Label', 'text', 'LIVE UPDATE');
      node_set_number(S.title, 'scn.Label', 'font_size', 24.0);
    }
    if (S.frames === 8) {
      S.title_w = node_get_number(S.title, 'scn.Label', 'text_w');
      S.title_lines = node_get_number(S.title, 'scn.Label', 'line_count');
      S.rich_lines = node_get_number(S.rich, 'render.RichText', 'line_count');
      S.rich_comps = node_component_count(S.rich);
      S.rich_text = node_get_string(S.rich, 'render.RichText', 'text');
      S.rich_w = node_get_number(S.rich, 'render.RichText', 'font_size');
      S.wrapped_lines_end = node_get_number(S.wrapped, 'scn.Label', 'line_count');
      log('text_summary ' + JSON.stringify(S));
    }
    if (input_key(1) > 0) quit();
  };
})();
