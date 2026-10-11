// cocos.js —— 演示 Cocos Creator 风格的节点/组件/事件 API（docs/18 对照）
(function () {
  'use strict';
  const S = (globalThis.__maque = globalThis.__maque || { log: [] });

  function note(msg) { S.log.push(msg); log(msg); }

  // 事件处理（node_on 注册的处理器；引擎按名字回调）
  globalThis.on_slider_changed = function (id, value) {
    S.slider_events = (S.slider_events || 0) + 1;
    S.slider_value = value;
  };
  globalThis.on_mute_toggled = function (id, on) {
    S.mute_events = (S.mute_events || 0) + 1;
    S.muted = on;
  };

  globalThis.on_start = function () {
    // ---- 节点层级（Cocos setParent / children）----
    const canvas = entity_find('Canvas');
    const panel = entity_find('Panel');
    const bar = entity_find('Bar');
    const barFill = entity_find('BarFill');
    const volume = entity_find('Volume');
    const mute = entity_find('Mute');
    const spawned = entity_find('Spawned');

    node_set_parent(panel, canvas);
    node_set_parent(bar, panel);
    node_set_parent(barFill, bar);
    node_set_parent(volume, panel);
    node_set_parent(mute, panel);
    node_set_parent(spawned, canvas);

    S.ids = { canvas: canvas, panel: panel, bar: bar, barFill: barFill, volume: volume, mute: mute, spawned: spawned };
    S.panel_children = node_child_count(panel);
    S.canvas_children = node_child_count(canvas);
    S.parent_of_bar = node_parent(bar);
    S.child0_of_panel = node_child_at(panel, 0);
    S.find_child = node_find_child(panel, 'Volume');

    // ---- 节点属性（Cocos position/scale/angle/opacity/zIndex/active）----
    node_set_scale(spawned, 1.5);
    node_set_angle(spawned, 30.0);
    node_set_opacity(spawned, 0.5);
    node_set_z_index(spawned, 3);
    node_set_content_size(spawned, 40.0, 20.0);
    node_set_position(spawned, 240.0, 40.0);
    S.spawned_props = [node_scale(spawned), node_angle(spawned), node_opacity(spawned),
                       node_z_index(spawned), node_width(spawned), node_height(spawned)];

    // ---- 组件（Cocos addComponent / getComponent / property）----
    node_add_component(spawned, 'scn.Sprite');
    S.has_sprite = node_has_component(spawned, 'scn.Sprite');
    node_set_number(spawned, 'scn.Sprite', 'radius', 9.0);
    S.sprite_radius = node_get_number(spawned, 'scn.Sprite', 'radius');
    S.component_count = node_component_count(spawned);
    S.component0 = node_component_at(spawned, 0);

    // ---- 事件订阅（Cocos node.on）----
    node_on(volume, 'changed', 'on_slider_changed');
    node_on(mute, 'toggled', 'on_mute_toggled');

    // ---- UI 控件值 ----
    ui_set_value(bar, 0.25);
    S.bar_value = ui_get_value(bar);
    S.mute_checked = ui_get_checked(mute);

    // ---- 层级/布局：Panel 的垂直布局在帧内生效 ----
    S.layout_before = [entity_y(bar), entity_y(volume)];
    note('cocos.js ready');
  };

  globalThis.update = function () {
    S.frames = (S.frames || 0) + 1;
    // 进度条随时间推进（Cocos ProgressBar.value）
    ui_set_value(S.ids.bar, Math.min(1.0, 0.25 + S.frames * 0.01));
    // 自省快照供控制面读取
    if (S.frames === 3) {
      S.layout_after = [entity_y(S.ids.bar), entity_y(S.ids.volume)];
      S.fall_y_3 = entity_y(entity_find('Fall'));
    }
    if (S.frames === 40) {
      S.fall_y_40 = entity_y(entity_find('Fall'));
      S.paused = director_paused();
      S.bar_value_end = ui_get_value(S.ids.bar);
      S.engine_frame_end = director_frame();
      S.paused_flag = director_paused();
      S.slider_events = S.slider_events || 0;
      S.mute_events = S.mute_events || 0;
      // 结构化自省（e2e 断言用；单行 JSON）
      log('cocos_summary ' + JSON.stringify(S));
    }
    if (input_key(1) > 0) quit();
  };
})();
