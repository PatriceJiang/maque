// ball.js —— 一个下落弹跳球。全部游戏逻辑都在这里；引擎只提供通用能力
// (screen_w/screen_h/cfg/draw_sprite/rng/quit)。世界坐标 y 向上。
const gravity     = 980.0;   // px/s^2
const restitution = 0.6;     // 恢复系数
const radius      = 8.0;
const removeDelay = 5.0;     // 静止后移除延迟（秒）

let x = 0.0, y = 200.0, vx = 0.0, vy = 0.0;
let resting = false, restTime = 0.0, done = false;

function on_start() {
    x = screen_w() * 0.5;
    log("ball: spawn at (" + x + "," + y + ")");
}

function update(dt) {
    if (done) return;

    if (!resting) {
        vy -= gravity * dt;          // 自由落体
        y  += vy * dt;
        x  += vx * dt;
        if (y <= radius) {           // 触地 → 弹跳
            y = radius;
            vy = -vy * restitution;
            if (Math.abs(vy) < 20.0) { vy = 0.0; resting = true; }   // 高度→0，静止
        }
    } else {
        restTime += dt;
        if (restTime >= removeDelay) { done = true; log("ball: removed"); quit(); return; }
    }

    // 屏幕坐标 y 向下 → 翻转
    draw_sprite(x, screen_h() - y, radius, 0.4, 0.8, 1.0);
}
