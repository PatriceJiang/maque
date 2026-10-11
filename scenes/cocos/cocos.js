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
  globalThis.on_tween_done = function (id, value) { S.tween_done = (S.tween_done || 0) + 1; };
  globalThis.on_timer_once = function (dt) { S.timer_once = (S.timer_once || 0) + 1; };
  globalThis.on_timer_repeat = function (dt) { S.timer_repeat = (S.timer_repeat || 0) + 1; };

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
    node_on(spawned, 'tween_complete', 'on_tween_done');

    // ---- UI 控件值 ----
    ui_set_value(bar, 0.25);
    S.bar_value = ui_get_value(bar);
    S.mute_checked = ui_get_checked(mute);

    // ---- Cocos 常用 API 续：tween / schedule / find(path) ----
    S.found_path = node_find_path(canvas, 'Panel/Volume');
    tween_start(spawned, 'scn.Sprite.radius', 20.0, 0.5, 0);      // 0.5 秒内半径 9 → 20（线性）
    schedule_once('on_timer_once', 0.5);                          // 0.5 秒后触发一次
    schedule('on_timer_repeat', 0.25);                            // 每 0.25 秒触发
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
    if (S.frames === 44) {
      S.valid_before_destroy = node_valid(S.ids.spawned);
      S.schedules_before_unschedule = schedule_count();
      S.tween_radius_end = node_get_number(S.ids.spawned, 'scn.Sprite', 'radius');
      // node.destroy 只是排队：帧末才真正销毁（Cocos 语义）
      node_destroy(S.ids.spawned);
      S.valid_right_after_destroy_call = node_valid(S.ids.spawned);
      unschedule('on_timer_repeat');
      S.schedules_after_unschedule = schedule_count();
    }
    if (S.frames === 46) {
      S.destroyed = 1 - node_valid(S.ids.spawned);        // 帧边界已生效 → 1
    }
    if (S.frames === 50) {
      S.fall_y_40 = entity_y(entity_find('Fall'));
      S.bar_value_end = ui_get_value(S.ids.bar);
      S.engine_frame_end = director_frame();
      S.paused = director_paused();
      S.tween_radius = S.tween_radius_end;
      S.tween_left = tween_count(S.ids.spawned);
      S.slider_events = S.slider_events || 0;
      S.mute_events = S.mute_events || 0;
      log('cocos_summary ' + JSON.stringify(S));          // 单行 JSON（e2e 断言）
    }
    if (input_key(1) > 0) quit();
  };
})();
