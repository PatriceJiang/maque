// scenes/ball_demo.js —— 游戏逻辑由 quickjs 脚本驱动（M2）
// 引擎提供物理与渲染；脚本决定"生成球"与"何时移除"。
let ballId = -1;

function on_start() {
    const x = cfg("click.x");
    const y = cfg("click.y");
    ballId = spawn_ball(x, y);
    log("script: spawn ball id=" + ballId + " at (" + x + "," + y + ")");
}

function update(dt) {
    if (ballId < 0) return 0;
    // 高度降到 0（静止）后 5 秒移除
    if (ball_resting(ballId) > 0 && ball_rest_time(ballId) >= cfg("remove_delay")) {
        remove_ball(ballId);
        log("script: remove ball id=" + ballId);
        ballId = -1;
        return 1; // 已移除
    }
    return 0;
}
