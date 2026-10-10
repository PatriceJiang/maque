// ball.js —— 下落弹跳球。游戏逻辑全在脚本；引擎提供"实体/组件"世界与渲染。
// 脚本只操作实体组件(entity_spawn/entity_set_pos/entity_destroy)，
// 渲染由引擎从 sprite 组件提取（世界坐标 y 向上，引擎渲染时翻转）。
const gravity     = 980.0;   // px/s^2
const restitution = 0.6;     // 恢复系数
const radius      = 8.0;
const ground      = radius;
const removeDelay = 5.0;     // 静止后移除延迟（秒）

let id = -1;
let x = 0.0, y = 200.0, vy = 0.0;
let resting = false, restTime = 0.0, done = false;

function on_start() {
    // 引擎接口经"声明 + 生成"的绑定调用（docs/21）：math.clamp / math.lerp
    log("math.clamp(5,0,3)=" + math.clamp(5, 0, 3)
        + " math.lerp(0,10,0.25)=" + math.lerp(0, 10, 0.25)
        + " math.saturate(-3)=" + math.saturate(-3));
    // 实体由场景资源(.scn)提供；脚本只驱动它（游戏即资源）
    if (entity_count() > 0) {
        id = 0;
        x = entity_x(id);
        y = entity_y(id);
        log("ball: use scene entity id=" + id + " at (" + x + "," + y + ")");
    } else {
        x = screen_w() * 0.5;
        id = entity_spawn(x, y, radius, 0.4, 0.8, 1.0);
        log("ball: spawn entity id=" + id);
    }
}

function update(dt) {
    if (done) return;

    if (!resting) {
        vy -= gravity * dt;       // 自由落体
        y  += vy * dt;
        if (y <= ground) {        // 触地 → 弹跳
            y = ground;
            vy = -vy * restitution;
            if (Math.abs(vy) < 20.0) { vy = 0.0; resting = true; }   // 高度→0，静止
        }
    } else {
        restTime += dt;
        if (restTime >= removeDelay) {
            entity_destroy(id);   // 静止 5s 后销毁实体
            log("ball: destroy entity id=" + id);
            done = true;
            quit();
            return;
        }
    }

    entity_set_pos(id, x, y);     // 更新 transform 组件
}
