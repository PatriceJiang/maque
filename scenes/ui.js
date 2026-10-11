// ui.js —— 输入驱动的玩法（docs/19）：所有交互都来自确定性事件流。
// 热重载约定（docs/14 §6）：脚本以 IIFE 形态定义全局回调 → 每次重载得到**全新词法作用域**，
// 回调被整体替换、let/const 不冲突；跨重载状态经 globalThis.__maque 显式迁移。
(function () {
  'use strict';

  var S = globalThis.__maque || (globalThis.__maque = { score: 0, spawnBtn: -1, clearBtn: -1 });

  function set_label_text(text) {
    var id = entity_find('ScoreLabel');
    if (id >= 0) set_label(id, text);
  }

  globalThis.on_start = function () {
    S.spawnBtn = entity_find('SpawnButton');
    S.clearBtn = entity_find('ClearButton');
    log('ui.js ready: spawn=' + S.spawnBtn + ' clear=' + S.clearBtn + ' score=' + S.score);
  };

  // 点击生成球；点击"清空"移除所有球（事件 → 脚本 → 引擎 API）
  globalThis.on_click = function () {
    var t = input_target();
    if (t < 0) return;
    if (t === S.spawnBtn) {
      var x = input_x();
      var y = screen_h() - input_y();
      var vx = (rng() - 0.5) * 220.0;
      var vy = -180.0 - rng() * 160.0;
      spawn_ball(x, y, vx, vy, 8.0);
      S.score = S.score + 1;                        // 状态在 globalThis 载体里 → 重载后保留
      set_label_text('score: ' + S.score);
    } else if (t === S.clearBtn) {
      ball_clear();
      S.score = 0;
      set_label_text('score: 0');
    }
  };

  globalThis.update = function (dt) {
    bounce_step(dt);
    if (input_key(1) > 0) quit();                   // Esc 退出（归一化键码：Esc = 1）
  };

  globalThis.on_pointer_down = function () { /* 供 AI/CI 注入验证 */ };
})();
