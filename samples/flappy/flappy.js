/* Flappy Bird —— 玩法全部在这个脚本里。
 *
 * 分工（这就是"引擎即浏览器、游戏即资源"）：
 *   · 引擎负责：窗口/输入事件（已映射为**世界坐标**）、渲染（从 sprite 组件提取）、
 *     碰撞求解与**接触查询**（physics_contact_*，供需要真实物理时使用）、热重载、控制面。
 *   · 本脚本负责：重力与速度、拍翅、水管生成与回收、碰撞判定、计分、状态机、HUD。
 *   引擎不需要知道"什么是水管""撞了要死" —— 那些是游戏的事。
 *
 * 坐标约定：世界坐标 **y 向上**（引擎渲染时翻到屏幕坐标），所以"下坠"= y 减小。
 *
 * 碰撞为什么用脚本侧 AABB 而不是物理引擎：
 *   1) flappy 判定要"薄水管 + 高速穿行"，离散窄相会**隧穿**（本会话实测：每帧位移
 *      超过碰撞体厚度就直接穿过去）。AABB 逐帧判定没有这个问题，也不需要 CCD。
 *   2) 手感需要判定盒比视觉小一圈（forgiveness），这在脚本里调最方便。
 *   `physics_contact_*` 仍是对外能力，`scenes/contact/` 有它的用法演示与 e2e 断言。
 *
 * 确定性（docs/10）：只用固定 dt、引擎 rng()、无墙钟/无异步 → 同样的输入序列
 *   必得同样的轨迹（tests/flappy_e2e.sh 用 MAQUE_SIM 回放同一串点击来断言这一点）。 */

/* ---- 设计分辨率的常量（改这里就能整局改手感） ---- */
const W = 320.0, H = 240.0;
const GROUND_Y = 20.0;          // 地面顶面高度（世界 y）
const BIRD_X = 84.0;            // 鸟固定在屏幕左侧，世界向左卷动
const BIRD_R = 9.0;             // 视觉半径
const BIRD_HIT_R = 7.0;         // 判定半径（略小 → 手感宽容）

const GRAVITY = -1400.0;        // px/s²（负 = 向下）
const FLAP_VY = 380.0;          // 拍翅瞬时上冲速度
const MAX_FALL = -520.0;        // 下落速度上限（防隧穿 + 手感）

const PIPE_W = 30.0;            // 水管宽（视觉）
const PIPE_CAP_W = 36.0, PIPE_CAP_H = 14.0;
const GAP_MIN = 78.0, GAP_MAX = 96.0;   // 缺口高度区间（随分数收窄 → 难度曲线）
const GAP_MARGIN = 34.0;        // 缺口离天花板/地面至少留这么高
const PIPE_SPACING = 132.0;     // 相邻水管的水平间距
const SCROLL_SPEED = 62.0;      // 世界卷动速度 px/s（随分数略增）
const SPEED_MAX = 96.0;

/* ---- 竖直管身用几段矩形拼（贴图 30x16，sampler 是 ClampToEdge 不能平铺） ---- */
const PIPE_SEG = 16.0;

/* ---- 运行时状态 ---- */
let bird = -1;
let groundIds = [], grassIds = [];
let hud = -1;
let pipes = [];                 // { top, bot, x, gapY, gapH, scored }
let state = 'ready';            // ready | playing | dead
let by = 130.0, bvy = 0.0;
let score = 0, best = 0;
let nextPipeX = 0.0;
let deadTimer = 0.0;

/* ---- 工具 ---- */
function clamp(v, lo, hi) { return v < lo ? lo : (v > hi ? hi : v); }

/** 在 (x,y) 生成一个矩形 sprite 实体（中心锚点），返回 id。
 *  动态实体不走装配期的资源预解析，所以这里显式给颜色（r/g/b，域 0..1），
 *  不依赖贴图 —— 见下方 makePipeColumn 的说明。 */
function makeRect(x, y, w, h, r, g, b, z) {
    const id = entity_spawn();
    if (id < 0) return -1;
    node_add_component(id, 'scn.Transform2D');
    node_add_component(id, 'scn.Sprite');
    entity_set_pos(id, x, y);
    node_set_number(id, 'scn.Sprite', 'width', w);
    node_set_number(id, 'scn.Sprite', 'height', h);
    node_set_number(id, 'scn.Sprite', 'r', r);
    node_set_number(id, 'scn.Sprite', 'g', g);
    node_set_number(id, 'scn.Sprite', 'b', b);
    return id;
}

/* 一列水管 = { head, segs }，整体一起平移；height <= 0 时返回 null（该侧没有柱子）。
 *
 * 为什么用纯色而不是贴图：渲染提取是只读的、只查材质/贴图缓存，而缓存是在**装配期**
 * 按场景实体预热的；脚本运行时生成的实体拿不到那次预热，material 字符串赋了也不会
 * 解析 → 图元退化成 sprite 的默认蓝色（已实测：动态水管全蓝，场景内同材质的静态管正常绿）。
 * 这是引擎侧的一个真实缺口（运行期资源解析），在它修好之前，游戏侧用纯色是稳妥做法：
 * 颜色走的是每条图元本来就带的 r/g/b，不依赖任何缓存。 */
function makePipeColumn(x, baseY, height, up) {
    if (height <= 0.5) return null;
    const headH = PIPE_CAP_H;
    const bodyH = height - headH;
    let headY, bodyBase;
    if (up) {
        headY = baseY + headH * 0.5;            // 管口在下方
        bodyBase = baseY + headH;
    } else {
        headY = baseY + height - headH * 0.5;   // 管口在上方
        bodyBase = baseY;
    }
    /* 管口比管身亮一点，做出"管帽"的层次感 */
    const head = makeRect(x, headY, PIPE_CAP_W, headH, 0.42, 0.80, 0.34, 5);
    const segs = [];
    let y = bodyBase;
    while (y < bodyBase + bodyH - 0.5) {
        const h = Math.min(PIPE_SEG, bodyBase + bodyH - y);
        if (h <= 0.5) break;
        /* 每段交替深浅 → 管身有竖向条纹（比纯色更像水管） */
        segs.push(makeRect(x, y + h * 0.5, PIPE_W, h,
                           (segs.length % 2 === 0) ? 0.32 : 0.38, 0.70, 0.26, 4));
        y += h;
    }
    return { head: head, segs: segs };
}

/** 摧毁一列水管的所有实体。 */
function destroyPipeColumn(col) {
    if (!col) return;
    if (col.head >= 0) entity_destroy(col.head);
    for (let i = 0; i < col.segs.length; ++i) entity_destroy(col.segs[i]);
}

function destroyPipe(p) {
    destroyPipeColumn(p.bot);
    destroyPipeColumn(p.top);
}

/** 缺口中心：距地面/天花板各留 GAP_MARGIN。用**引擎 rng**（可复现，docs/10）。 */
function rollGapY(gapH) {
    const lo = GROUND_Y + GAP_MARGIN + gapH * 0.5;
    const hi = H - GAP_MARGIN - gapH * 0.5;
    if (hi <= lo) return (lo + hi) * 0.5;
    return lo + (hi - lo) * rng();
}

function gapHeightFor(s) {
    const g = GAP_MAX - (GAP_MAX - GAP_MIN) * Math.min(1.0, s / 12.0);
    return g;
}

function scrollSpeedFor(s) {
    return Math.min(SPEED_MAX, SCROLL_SPEED + s * 2.0);
}

/** 生成一根水管（上下两段），缺口中心 y = gapY。 */
function spawnPipe(x, gapY, gapH) {
    const bottomTop = GROUND_Y;
    const bottomH = gapY - gapH * 0.5 - bottomTop;
    const topBot = gapY + gapH * 0.5;
    const topH = H - topBot;
    const bot = makePipeColumn(x, bottomTop, bottomH, false);
    const top = makePipeColumn(x, topBot, topH, true);
    pipes.push({ top: top, bot: bot, x: x, gapY: gapY, gapH: gapH, scored: false });
}

function setHud(text) {
    if (hud >= 0 && entity_valid(hud)) ui_set_text(hud, text);
}

function resetWorld() {
    for (let i = 0; i < pipes.length; ++i) destroyPipe(pipes[i]);
    pipes = [];
    by = 130.0; bvy = 0.0;
    score = 0;
    deadTimer = 0.0;
    resetGround();
    entity_set_pos(bird, BIRD_X, by);
    node_set_angle(bird, 0.0);
    node_set_active(bird, 1);
    /* 开局先摆两根，玩家一进来就有东西可躲 */
    let x = W + 40.0;
    for (let i = 0; i < 2; ++i) {
        const g = gapHeightFor(0);
        spawnPipe(x, rollGapY(g), g);
        x += PIPE_SPACING;
    }
    nextPipeX = x;
}

/* 地面/草皮：两张同图并排，向左平移到 -W 后整体 +2W 回位（无缝循环） */
function resetGround() {
    groundIds = [entity_find('ground'), entity_find('ground2')];
    grassIds = [entity_find('grass'), entity_find('grass2')];
    for (let i = 0; i < 2; ++i) {
        entity_set_pos(groundIds[i], 160.0 + i * W, 10.0);
        entity_set_pos(grassIds[i], 160.0 + i * W, 23.0);
    }
}

function scrollGround(dx) {
    for (let i = 0; i < 2; ++i) {
        if (groundIds[i] < 0) continue;
        let x = entity_x(groundIds[i]) - dx;
        if (x <= -W * 0.5) x += W * 2.0;
        entity_set_pos(groundIds[i], x, 10.0);
        let gx = entity_x(grassIds[i]) - dx;
        if (gx <= -W * 0.5) gx += W * 2.0;
        entity_set_pos(grassIds[i], gx, 23.0);
    }
}

/* ---- 鸟 × 水管：圆 vs 两个矩形（管身按视觉宽度，判定用略窄的宽容盒） ---- */
function circleHitsRect(cx, cy, r, rx, ry, rw, rh) {
    const halfW = rw * 0.5, halfH = rh * 0.5;
    const nx = clamp(cx, rx - halfW, rx + halfW);
    const ny = clamp(cy, ry - halfH, ry + halfH);
    const dx = cx - nx, dy = cy - ny;
    return dx * dx + dy * dy <= r * r;
}

function pipeColumnsHit(p) {
    const x = p.x;
    const gapLo = p.gapY - p.gapH * 0.5;
    const gapHi = p.gapY + p.gapH * 0.5;
    /* 下柱：从地面到 gapLo；上柱：从 gapHi 到天花板 */
    if (circleHitsRect(BIRD_X, by, BIRD_HIT_R, x, (GROUND_Y + gapLo) * 0.5,
                       PIPE_W, gapLo - GROUND_Y)) return true;
    if (circleHitsRect(BIRD_X, by, BIRD_HIT_R, x, (gapHi + H) * 0.5,
                       PIPE_W, H - gapHi)) return true;
    return false;
}

function updatePipes(dt, speed) {
    const dx = speed * dt;
    for (let i = 0; i < pipes.length; ++i) {
        const p = pipes[i];
        p.x -= dx;
        movePipeColumn(p.bot, -dx);
        movePipeColumn(p.top, -dx);
        /* 计分：水管中心被鸟越过 */
        if (!p.scored && p.x < BIRD_X) {
            p.scored = true;
            score += 1;
            setHud(String(score));
        }
    }
    /* 回收挪出屏幕的 */
    while (pipes.length > 0 && pipes[0].x < -PIPE_CAP_W) {
        destroyPipe(pipes[0]);
        pipes.shift();
    }
    /* 补充新的：从最后一根的 x 往前排，间隔恒定 */
    let last = nextPipeX;
    if (pipes.length > 0) last = pipes[pipes.length - 1].x;
    while (last < W + PIPE_SPACING) {
        last += PIPE_SPACING;
        const g = gapHeightFor(score);
        spawnPipe(last, rollGapY(g), g);
    }
    nextPipeX = last;
}

function movePipeColumn(col, dx) {
    if (!col) return;
    if (col.head >= 0) entity_set_pos(col.head, entity_x(col.head) + dx, entity_y(col.head));
    for (let i = 0; i < col.segs.length; ++i) {
        const s = col.segs[i];
        entity_set_pos(s, entity_x(s) + dx, entity_y(s));
    }
}

function die() {
    state = 'dead';
    deadTimer = 0.0;
    if (score > best) best = score;
    setHud('SCORE ' + score + '   BEST ' + best + '   TAP TO RETRY');
    log('flappy: died at score ' + score + ' frame ' + director_frame());
    log('flappy: score=' + score + ' best=' + best);
}

function flap() {
    if (state === 'ready') { state = 'playing'; setHud('0'); }
    if (state === 'playing') bvy = FLAP_VY;
}

/** 重开一局（保留 best）。死亡动画播完才可以重开，避免误触秒重开。 */
function restart() {
    state = 'ready';
    resetWorld();
    setHud('TAP / SPACE');
    log('flappy: restart');
}

/* ---- 输入回调：签名来自 schema/script.yaml 的 callbacks（引擎按声明派发）----
 * 点击/触摸/空格/上箭头都算"拍翅"。死亡状态下任意输入重开。 */
function on_pointer_down(target, x, y) {
    if (state === 'dead') { if (deadTimer > 0.4) restart(); return; }
    flap();
}

function on_key_down(target, key) {
    /* 只认空格(32)与上箭头(1073741906，SDL 的 Up)，避免按任意键都拍翅 */
    if (key !== 32 && key !== 1073741906) return;
    if (state === 'dead') { if (deadTimer > 0.4) restart(); return; }
    flap();
}

function on_start() {
    bird = entity_find('bird');
    hud = entity_find('hud');
    if (bird < 0) { log('flappy: bird entity missing'); quit(); return; }
    /* 测试钩子：宿主/测试可用控制面 `Scene.emit <birdId> "crash" 1` 触发一次"必然撞管"，
     * 走的仍是 update 里的真实判定路径（不是直接调 die()），所以能抓到判定失效。 */
    node_on(bird, 'crash', 'on_crash_event');
    /* 回归用：MAQUE_FLAPPY_CRASH_AT=<帧号> → 该帧起自动把鸟送进水管。
     * 为什么要这个：e2e 需要"脚本已经跑起来之后"才注入（控制面的 MAQUE_CONTROL 是
     * **帧循环之前**执行的，stdin 模式也一样，所以没法在运行中注入）。 */
    const crashRate = Number(env('MAQUE_FLAPPY_CRASH_AT'));
    if (isFinite(crashRate) && crashRate > 0) {
        crashHookFrame = crashRate;
        log('flappy: crash hook armed at frame ' + crashRate);
    }
    followGap = env('MAQUE_FLAPPY_FOLLOW_GAP') === '1';
    resetWorld();
    setHud('TAP / SPACE');
    log('flappy: ready  ' + W + 'x' + H);
}

let crashHookFrame = 0;
let followGap = false;

/* 由 Scene.emit(id, "crash", v) 触发（docs/19 §8 的节点事件路径，跨语言通用）。 */
function on_crash_event(id, value) {
    if (value <= 0.0) return;
    if (state === 'ready') { state = 'playing'; setHud('0'); }
    log('flappy: crash event -> ' + force_crash_into_pipe());
}

function update(dt) {
    if (state === 'dead') {
        deadTimer += dt;
        /* 死亡后让鸟自然坠落，落地即停 */
        bvy = Math.max(MAX_FALL, bvy + GRAVITY * dt);
        by += bvy * dt;
        if (by <= GROUND_Y + BIRD_R) { by = GROUND_Y + BIRD_R; bvy = 0.0; }
        entity_set_pos(bird, BIRD_X, by);
        scrollGround(SCROLL_SPEED * dt * 0.35);
        return;
    }

    /* ready：鸟在空中上下轻浮，等第一次输入 */
    if (state === 'ready') {
        by = 130.0 + Math.sin(director_frame() * 0.12) * 6.0;
        entity_set_pos(bird, BIRD_X, by);
        scrollGround(SCROLL_SPEED * dt * 0.5);
        return;
    }

    /* playing：重力 + 位置 */
    bvy = Math.max(MAX_FALL, bvy + GRAVITY * dt);
    by += bvy * dt;

    /* 撞天花板/地面 */
    if (by >= H - BIRD_R) { by = H - BIRD_R; die(); return; }
    if (by <= GROUND_Y + BIRD_R) { by = GROUND_Y + BIRD_R; die(); return; }

    const speed = scrollSpeedFor(score);
    scrollGround(speed * dt);
    updatePipes(dt, speed);

    /* 测试钩子：到点就把鸟送进水管（走真实判定路径） */
    if (crashHookFrame > 0 && director_frame() >= crashHookFrame) {
        crashHookFrame = 0;
        log('flappy: crash hook fired -> ' + force_crash_into_pipe());
    }

    /* 测试钩子：MAQUE_FLAPPY_FOLLOW_GAP=1 → 把鸟吸到"当前与它在 x 上重叠的那根管子"的缺口正中。
     * 用途：让 e2e 稳定验证**计分与滚动**通路（否则要手调拍翅节奏去穿随机缺口，
     * 那是"测试玩得准不准"，不是"引擎/游戏逻辑对不对"，必然脆弱）。
     * 注意：遍历**所有**管子而不是只看 pipes[0] —— 只看第一根会在"旧管还在屏上但已越过鸟"
     * 时漏掉正前方那根，鸟就会以旧管缺口的中心高度撞上新管（实测过，正是这个坑）。
     * 它只改鸟的 y，**不跳过任何判定**：碰撞、计分、难度曲线照常走。 */
    if (followGap) {
        for (let i = 0; i < pipes.length; ++i) {
            const p = pipes[i];
            if (p.x - PIPE_W * 0.5 <= BIRD_X + BIRD_HIT_R && p.x + PIPE_W * 0.5 >= BIRD_X - BIRD_HIT_R) {
                by = p.gapY;
                bvy = 0.0;
                break;
            }
        }
    }

    /* 撞水管 */
    for (let i = 0; i < pipes.length; ++i) {
        if (pipes[i].x + PIPE_W * 0.5 < BIRD_X - BIRD_HIT_R) continue;   // 已越过
        if (pipes[i].x - PIPE_W * 0.5 > BIRD_X + BIRD_HIT_R) break;      // 还没到
        if (pipeColumnsHit(pipes[i])) { entity_set_pos(bird, BIRD_X, by); die(); return; }
    }

    /* 姿态：上升抬头、下坠低头（纯观感） */
    const target = clamp(bvy / 420.0, -1.0, 1.0) * 34.0;
    node_set_angle(bird, target);
    entity_set_pos(bird, BIRD_X, by);
}

/* 测试钩子：把鸟瞬移到**第一根水管下柱的顶边**（圆心正好落在缺口下沿），
 * 半径会让它跨在柱子上 → 必然与下柱相交，随后 update 的判定自然判死。
 * 刻意不直接调用 die()：走真实判定路径，这个钩子才有鉴别力（能抓"碰撞判定失效"）。 */
function force_crash_into_pipe() {
    if (state !== 'playing') return 0;
    if (pipes.length === 0) return 0;
    const p = pipes[0];
    by = p.gapY - p.gapH * 0.5;             // 缺口下沿 = 下柱顶边
    bvy = 0.0;
    entity_set_pos(bird, BIRD_X, by);
    /* 把水管挪到鸟身上（x 对齐）→ 下一帧判定必然命中 */
    const dx = BIRD_X - p.x;
    p.x += dx;
    movePipeColumn(p.bot, dx);
    movePipeColumn(p.top, dx);
    return 1;
}

/* 测试钩子：当前分数（控制面/测试读取） */
function get_score() { return score; }
