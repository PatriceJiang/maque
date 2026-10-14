// ui.js —— 输入驱动的玩法（docs/19）：所有交互都来自确定性事件流。
// 脚本形态（docs/14 §3）：**普通顶层脚本**——回调是顶层函数声明，引擎按名字从全局对象取。
// 热重载（docs/14 §7）会**重建 VM**：脚本内状态（这里的 S，globalThis 上的东西同样）不跨重载保留；
// 需要跨热更保留的状态放世界侧（组件/字段），见 docs/14 §7。
'use strict';


const S = { score: 0, spawnBtn: -1, clearBtn: -1 };

function set_label_text(text) {
  var id = entity_find('ScoreLabel');
  if (id >= 0) set_label(id, text);
}

function on_start() {
  S.spawnBtn = entity_find('SpawnButton');
  S.clearBtn = entity_find('ClearButton');
  log('ui.js ready: spawn=' + S.spawnBtn + ' clear=' + S.clearBtn + ' score=' + S.score);
}

// 点击生成球；点击"清空"移除所有球（事件 → 脚本 → 引擎 API）
function on_click() {
  var t = input_target();
  if (t < 0) return;
  if (t === S.spawnBtn) {
    var x = input_x();
    var y = screen_h() - input_y();
    var vx = (rng() - 0.5) * 220.0;
    var vy = -180.0 - rng() * 160.0;
    spawn_ball(x, y, vx, vy, 8.0);
    S.score = S.score + 1;                        // 脚本内状态（热重载重建 VM → 清零，docs/14 §7）
    set_label_text('score: ' + S.score);
  } else if (t === S.clearBtn) {
    ball_clear();
    S.score = 0;
    set_label_text('score: 0');
  }
}

function update(dt) {
  physics_step();                                 // 固定步长物理（docs/15 §3）
  if (input_key(1) > 0) quit();                   // Esc 退出（归一化键码：Esc = 1）
}

function on_pointer_down() { /* 供 AI/CI 注入验证 */ }
