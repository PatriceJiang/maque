// ball.js —— 弹跳球 + **点击生成球**（游戏逻辑全在脚本；引擎提供"实体/组件"世界与渲染）。
// 脚本只操作实体组件（spawn_ball / entity_set_pos / entity_destroy），渲染由引擎从 sprite 组件提取
// （世界坐标 y 向上，引擎渲染时翻转）。
//
// 行为：
//   ① 场景自带的球自由落体 → 触地弹跳（恢复系数）→ 静止；
//   ② **在窗口内按下鼠标**：在按下处生成一颗球并带向上初速（像"扔球"）；
//   ③ 每颗球静止 5 秒后被销毁；**场上没有球时** demo 结束（quit）。
//      因此不点击时行为与旧版完全一致（单球落定后退出）；持续点击就能一直玩。
//
// 输入坐标：引擎已把窗口像素映射为**世界坐标**（窗口可缩放显示世界，见 docs/05），
// 脚本直接用 (x, y)，无需关心窗口尺寸。
const gravity     = 980.0;   // px/s^2
const restitution = 0.6;     // 恢复系数
const groundR     = 8.0;     // 球心最低 y（半径）
const removeDelay = 5.0;     // 静止后移除延迟（秒）
const clickSpeed  = 220.0;   // 点击生成时的向上初速

// 场上每颗球的状态（脚本侧模拟；实体本身只存 position 组件）
const balls = [];
let clicks = 0;

function addBall(bid, bx, by, bvy) {
    balls.push({ id: bid, x: bx, y: by, vy: bvy, resting: false, restTime: 0.0 });
}

function on_start() {
    // 引擎接口经"声明 + 生成"的绑定调用（docs/21）：math.clamp / math.lerp
    log("math.clamp(5,0,3)=" + math.clamp(5, 0, 3)
        + " math.lerp(0,10,0.25)=" + math.lerp(0, 10, 0.25)
        + " math.saturate(-3)=" + math.saturate(-3));
    // 实体由场景资源(.scn)提供；脚本只驱动它（游戏即资源）
    if (entity_count() > 0) {
        const id = 0;
        const x = entity_x(id), y = entity_y(id);
        addBall(id, x, y, 0.0);
        log("ball: use scene entity id=" + id + " at (" + x + "," + y + ")");
    } else {
        const x = screen_w() * 0.5;
        const id = spawn_ball(x, 200.0, 0.0, 0.0, groundR);
        addBall(id, x, 200.0, 0.0);
        log("ball: spawn entity id=" + id);
    }
    log("ball: 点击窗口可在按下处生成球");
}

// 按下鼠标（引擎按回调表派发；坐标为世界坐标）
function on_pointer_down(target, x, y) {
    const id = spawn_ball(x, y, 0.0, clickSpeed, groundR);
    if (id < 0) return;
    addBall(id, x, y, clickSpeed);
    ++clicks;
    log("ball: click #" + clicks + " spawn id=" + id
        + " at (" + Math.round(x) + "," + Math.round(y) + ") balls=" + balls.length);
}

function update(dt) {
    for (let i = balls.length - 1; i >= 0; --i) {
        const b = balls[i];
        if (!b.resting) {
            b.vy -= gravity * dt;       // 自由落体
            b.y  += b.vy * dt;
            if (b.y <= groundR) {       // 触地 → 弹跳
                b.y = groundR;
                b.vy = -b.vy * restitution;
                if (Math.abs(b.vy) < 20.0) { b.vy = 0.0; b.resting = true; }
            }
            entity_set_pos(b.id, b.x, b.y);
        } else {
            b.restTime += dt;
            if (b.restTime >= removeDelay) {
                entity_destroy(b.id);   // 静止 5s 后销毁实体
                log("ball: destroy entity id=" + b.id + " (剩余 " + (balls.length - 1) + ")");
                balls.splice(i, 1);
            }
        }
    }
    // 场上没有球 → demo 结束（不点击时与旧行为一致）
    if (balls.length === 0) {
        log("ball: 全部静止并移除，退出（clicks=" + clicks + "）");
        quit();
    }
}
