// ui.js —— 输入驱动的玩法（docs/19）：所有交互都来自确定性事件流。
// 脚本只表达玩法：命中/分发由引擎完成（脚本经 input_target / ui_hit_at 读结果）。
let score = 0;
let spawnBtn = -1;
let clearBtn = -1;
const DT = 1.0 / 60.0;

function on_start() {
  spawnBtn = entity_find("SpawnButton");
  clearBtn = entity_find("ClearButton");
  log("ui.js ready: spawn=" + spawnBtn + " clear=" + clearBtn);
}

// 点击生成球；点击"清空"移除所有球（事件 → 脚本 → 引擎 API）
function on_click() {
  const t = input_target();
  if (t < 0) return;
  if (t === spawnBtn) {
    const x = input_x();
    const y = screen_h() - input_y();
    const vx = (rng() - 0.5) * 220.0;
    const vy = -180.0 - rng() * 160.0;
    spawn_ball(x, y, vx, vy, 8.0);
    score = score + 1;
    set_label_text("score: " + score);
  } else if (t === clearBtn) {
    ball_clear();
    score = 0;
    set_label_text("score: 0");
  }
}

function set_label_text(text) {
  const id = entity_find("ScoreLabel");
  if (id >= 0) set_label(id, text);
}

function update(dt) {
  bounce_step(dt);
  if (input_key(1) > 0) quit();         // Esc 退出（归一化键码：Esc = 1）
}

function on_pointer_down() { /* 供 AI/CI 注入验证 */ }
