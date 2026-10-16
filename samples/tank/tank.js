/* Tank Duel —— 俯视坦克对战，玩法全部在这个脚本里。
 *
 * 分工（这就是"引擎即浏览器、游戏即资源"）：
 *   · 引擎负责：窗口/输入事件（已映射为**世界坐标**）、渲染（从 sprite 组件提取）、
 *     节点树与父子关系、UI 控件值、字体排版、热重载、控制面、确定性 RNG。
 *   · 本脚本负责：驾驶、瞄准、开火、敌人 AI、炮弹飞行与**命中判定**、计分、波次、HUD。
 *     引擎不需要知道"什么是坦克""打中了要扣血" —— 那些是游戏的事。
 *
 * 坐标约定：世界坐标 **y 向上**（引擎渲染时翻到屏幕坐标），角度制、0° = +x、逆时针为正。
 *   所以"向上"= 90°，"向右"= 0°。车的朝向 `ang` 与炮塔朝向 `aim` 是**两个独立自由度**。
 *
 * 为什么命中判定用脚本侧圆/AABB 而不是物理引擎（与 flappy 同一个理由，docs/15 §3）：
 *   物理求解器是**离散窄相、无 CCD**：炮弹单帧位移 = 360/60 = 6px 虽小于车身半径，
 *   但薄掩体（木箱边缘）在高速下会隧穿；而玩法需要"命中盒比视觉略小"的宽容度，
 *   这些在脚本里逐帧判定最直接、最可控。引擎的物理接触查询（physics_contact_*）
 *   仍是对外能力，用法见 scenes/contact/。
 *
 * 确定性（docs/10）：只用固定 dt（引擎给的 1/60）、引擎 rng()、无墙钟、无异步
 *   → 同一输入序列必得同一轨迹（tests/tank_e2e.sh 用两次回放的 world_trail 断言）。
 *
 * 引擎现状（本版实测，详见 samples/tank/README.md「引擎现状」一节）：
 *   · `node_set_angle` **不会旋转** sprite 图元（`Scene::extract` 从不写 renderer_item.rotation）
 *     → 所有"朝向"都用**几何摆位**表达：炮管画成一串沿瞄准方向排布的小方块，
 *       车头用一个前向标记块。这样任何角度都能看出炮口指向哪。
 *   · `node_set_active(0)` **不会隐藏** sprite，`node_set_scale` / `node_set_opacity`
 *     对 sprite 也无效 → 池化实体用"挪到屏幕外"来闲置，大小直接改 width/height/radius。
 *   · 动态 spawn 的实体默认没有贴图（装配期预热只覆盖场景实体）→ 池化实体一律**纯色**。
 *
 * 测试钩子（只影响输入/朝向，判定与计分仍走真实代码路径）：
 *   · MAQUE_TANK_AUTOPLAY=1   ：脚本自驱动（自动转向最近敌人、预判瞄准、到点开火），
 *                               玩家阵亡后自动重开 → e2e 能在无输入下跑完整条链路。
 *   · MAQUE_TANK_KILL_AT=<帧> ：该帧把炮口对准最近敌人并**强制开火一次**（绕过冷却），
 *                               之后 90 帧内持续瞄准该敌人 → 确定性单次击杀验证。
 */

/* ============================ 1. 常量（改这里就能整局改手感） ============================ */

const W = 640.0, H = 480.0;
const HUD_TOP = 428.0;          // 世界 y > HUD_TOP 是 HUD 带（坦克不进，观感干净）
const DEG = Math.PI / 180.0;

/* 玩家 */
const P_ACCEL = 640.0;          // 速度变化率 px/s²（避免瞬间启停）
const P_MOVE = 132.0;           // 前进极速 px/s
const P_BACK = 78.0;            // 倒车极速
const P_TURN = 150.0;           // 车体转向速度 度/s
const P_TURRET_TURN = 280.0;    // 炮塔转向速度 度/s
const P_RADIUS = 17.0;          // 判定半径（视觉 44x34 → 略小，手感宽容）
const P_MAX_HP = 100.0;

/* 炮弹 */
const B_SPEED = 360.0;
const B_R = 3.5;
const B_LIFE = 2.2;             // 秒（超时回收，等价于射程 ~790px）
const P_DMG = 20.0;             // 玩家弹伤害（敌坦 40 血 → 2 发）
const P_FIRE_CD = 0.42;
const P_MAX_LIVE = 6;           // 同时在飞的玩家炮弹上限

/* 敌人 */
const E_RADIUS = 16.0;
const E_BASE_HP = 40.0;
const E_HP_PER_WAVE = 12.0;     // 每波 +12 血（难度曲线）
const E_SPEED = 66.0;
const E_TURN = 110.0;
const E_TURRET_TURN = 150.0;
const E_SIGHT = 300.0;          // 发现玩家
const E_RANGE = 320.0;          // 开火距离
const E_FIRE_CD = 1.5;          // 基础冷却（再加 rng 抖动）
const E_EB_SPEED = 250.0;
const E_DMG = 10.0;
const E_MAX_LIVE = 14;
const E_KEEP_FAR = 250.0;       // 交战时保持距离
const E_KEEP_NEAR = 150.0;

/* 池大小（**池化复用**：绝不每帧 spawn/destroy） */
const POOL_PB = 12;             // 玩家炮弹
const POOL_EB = 24;             // 敌方炮弹
const POOL_PART = 48;           // 爆炸/火花粒子
const POOL_WAVE = 6;            // 爆炸冲击波
const POOL_TRACK = 20;          // 履带印（场景实体，声明在坦克之前 → 画在车下面）
const TRACK_EVERY = 6;          // 每 6 帧落一对履带印
const TRACK_LIFE = 4.5;         // 履带印淡出时长（秒）

/* 敌军炮管的**本色**（场景里贴图带的就是锈红）—— 闪白时要按本色乘系数还原 */
const E_BARREL_TINT = { r: 1.0, g: 0.78, b: 0.62 };

const PARK_X = -9999.0, PARK_Y = -9999.0;   // 场景里"闲置"实体的初始坐标（脚本启动时会 active=0）

/* 键盘键码：真实设备走 platform 的**归一化**键码（空格=2、W=87…），
 * sim/录制回放注入的是原始码（空格=32、W=119…）—— 两套都认，见 README。 */
const K_SPACE = [2, 32];
const K_W = [87, 119], K_A = [65, 97], K_S = [83, 115], K_D = [68, 100];

/* ============================ 2. 运行时状态 ============================ */

let player = null;
let enemies = [];
let pbullets = [], ebullets = [], parts = [], waves = [];
let tracks = [], trackIdx = 0, trackAge = [];
let crates = [];                // {x,y,hw,hh} 掩体（从场景读，场景是唯一真相）
let spawns = [];                // 出生点（同样从场景初始位置读）

let hud = { hp: -1, score: -1, wave: -1, enemy: -1, hpbar: -1, hpfill: -1, state: -1, hint: -1 };

let state = 'play';             // play | clear | dead
let wave = 1, aliveCount = 0, score = 0, best = 0;
let deadTimer = 0.0, clearTimer = 0.0, trackTimer = 0;
let totalKills = 0, totalScore = 0;      // 跨局累计（只在日志里用，供 e2e 断言）

let pointerHave = false;                             // 是否收到过指针事件（没收到前炮塔朝车头）
let fireQueue = false;

let autoplay = false;
let killAtFrame = 0, aimHookFrames = 0;

/* ============================ 3. 工具 ============================ */

function clamp(v, lo, hi) { return v < lo ? lo : (v > hi ? hi : v); }

/** 角度差归一化到 (-180, 180]。 */
function angDiff(a, b) {
    let d = (a - b) % 360.0;
    if (d > 180.0) d -= 360.0;
    if (d <= -180.0) d += 360.0;
    return d;
}

/** 朝目标角转动，单帧最多 maxStep 度（角度制）。 */
function turnToward(cur, want, maxStep) {
    const d = angDiff(want, cur);
    if (d > maxStep) return cur + maxStep;
    if (d < -maxStep) return cur - maxStep;
    return want;
}

function dist2(ax, ay, bx, by) { const dx = ax - bx, dy = ay - by; return dx * dx + dy * dy; }
function dist(ax, ay, bx, by) { return Math.sqrt(dist2(ax, ay, bx, by)); }

function approach(v, target, step) {
    if (v < target) return Math.min(target, v + step);
    if (v > target) return Math.max(target, v - step);
    return v;
}

/** 按住判断：多套键码任一成立即算按下（真实设备 / sim 注入的码不同）。 */
function held(codes) {
    for (let i = 0; i < codes.length; ++i) if (input_key(codes[i])) return true;
    return false;
}

/* ============================ 4. 实体工厂（池化：只建一次，之后复用） ============================ */

/** 矩形 sprite（横竖都是**完整宽高** —— scn.Sprite 没有 half_w/half_h）。 */
function makeRect(x, y, w, h, r, g, b) {
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

/** 圆形 sprite：只给 radius（width/height 一旦 > 0 就变成矩形）。 */
function makeCircle(x, y, radius, r, g, b) {
    const id = entity_spawn();
    if (id < 0) return -1;
    node_add_component(id, 'scn.Transform2D');
    node_add_component(id, 'scn.Sprite');
    entity_set_pos(id, x, y);
    node_set_number(id, 'scn.Sprite', 'radius', radius);
    node_set_number(id, 'scn.Sprite', 'r', r);
    node_set_number(id, 'scn.Sprite', 'g', g);
    node_set_number(id, 'scn.Sprite', 'b', b);
    return id;
}

/** 闲置 = active=0。**不能**靠隐藏父节点：引擎只检查实体自身的 active，不级联到子节点
 *  （实测：隐藏父节点后子节点照常绘制）——所以池化实体逐个自己开关。 */
function park(id) { if (id >= 0) node_set_active(id, 0); }

function unpark(id) { if (id >= 0) node_set_active(id, 1); }

/** 乘法着色：贴图直连的 sprite 才有效（走 material 的 r/g/b 会被材质的 base_color 覆盖）。 */
function setTintRGB(id, r, g, b) {
    node_set_number(id, 'scn.Sprite', 'r', r);
    node_set_number(id, 'scn.Sprite', 'g', g);
    node_set_number(id, 'scn.Sprite', 'b', b);
}

/** 整车着色：k=1 还原本色，k>1 命中闪白（车体/炮塔/炮管一起亮）。
 *  敌坦炮管的**本色**带锈红（场景里就是带色贴图），所以按各部件本色乘 k。 */
function tintTank(t, k) {
    setTintRGB(t.hull, k, k, k);
    setTintRGB(t.turret, k, k, k);
    setTintRGB(t.barrel, k * t.bt.r, k * t.bt.g, k * t.bt.b);
}

/** 血条填充：左端对齐 → 宽度 + 中心 x 一起改。 */
function setBar(id, fullW, ratio, localY) {
    const w = fullW * clamp(ratio, 0.0, 1.0);
    node_set_number(id, 'scn.Sprite', 'width', w > 0.6 ? w : 0.6);
    entity_set_pos(id, -fullW * 0.5 + w * 0.5, localY);
}

/* ============================ 5. 坦克（玩家与敌人共用的表现层） ============================ */

/* 部件相对车体中心的距离：与 tank.scn 里"朝上(90°)"的初始摆位一致。
 * 炮管的**位置**每帧按朝向摆到 (cos,sin)×d（引擎的层级只累积旋转，不旋转子节点位置，
 * 见 README「引擎现状」）；炮管的**朝向**则由 node_set_angle 沿父链累积得到。 */
const P_BARREL_D = 12.0, P_MUZZLE_D = 28.0, P_NOSE_D = 15.0, P_HP_Y = 26.0, P_HP_W = 36.0;
const E_BARREL_D = 11.0, E_MUZZLE_D = 27.0, E_NOSE_D = 14.0, E_HP_Y = 24.0, E_HP_W = 32.0;

/* 贴图的"天然朝向"是**上**（炮管/车体贴图都按朝上画），而 `node_set_angle` 是
 * "以天然朝向为 0 的逆时针旋转" → 想让精灵指向世界角 θ，要设 `θ - 90`。
 * 实测：同一张朝上炮管贴图，0/90/180/270 分别指向 上/左/下/右。 */
function aimToSpriteAngle(worldDeg) { return worldDeg - 90.0; }

function findParts(prefix) {
    return {
        shadow: entity_find('shadow_' + prefix),
        hull: entity_find(prefix + '_hull'),
        nose: entity_find(prefix + '_nose'),
        turret: entity_find(prefix + '_turret'),
        barrel: entity_find(prefix + '_barrel'),
        flash: entity_find(prefix + '_flash'),
        hpfill: entity_find(prefix + '_hpfill'),
        hpback: entity_find(prefix + '_hpback')
    };
}

function partsOk(p) {
    return p.shadow >= 0 && p.hull >= 0 && p.turret >= 0 && p.nose >= 0 &&
           p.barrel >= 0 && p.flash >= 0 && p.hpfill >= 0 && p.hpback >= 0;
}

/** 部件列表（用于整车的显示/隐藏：引擎只检查**实体自身**的 active，不级联到子节点）。 */
function tankParts(t) {
    return [t.shadow, t.hull, t.nose, t.turret, t.barrel, t.flash, t.hpback, t.hpfill];
}

function setTankShown(t, on) {
    const parts = tankParts(t);
    for (let i = 0; i < parts.length; ++i) node_set_active(parts[i], on ? 1 : 0);
}

/** 把一辆坦克摆到它自己的位置上。
 *  车体是根实体 → 世界坐标；部件是子节点 → 局部坐标（引擎不旋转子节点位置，只累积旋转）。
 *  旋转分工（父子层级）：
 *    · 车体：`ang - 90`（贴图朝上 → 转到车体朝向）
 *    · 炮塔：`aim - ang`（**相对车体**，累积后世界朝向 = aim —— 这就是 WoT 类要的层级语义）
 *    · 炮管/炮口焰：自身角度 0，靠父链累积拿到 aim；位置由脚本按 aim 摆到炮塔外
 *    · 血条：`90 - ang` 反向抵消车体旋转，保持在屏幕上水平 */
function placeTank(t, barrelD, muzzleD, noseD, hpY, hpW) {
    setTankShown(t, true);
    const a = t.aim * DEG, b = t.ang * DEG;
    entity_set_pos(t.shadow, t.x, t.y - 3.0);
    entity_set_pos(t.hull, t.x, t.y);
    node_set_angle(t.hull, aimToSpriteAngle(t.ang));
    entity_set_pos(t.nose, Math.cos(b) * noseD, Math.sin(b) * noseD);
    node_set_angle(t.turret, angDiff(t.aim, t.ang));
    entity_set_pos(t.barrel, Math.cos(a) * barrelD, Math.sin(a) * barrelD);
    if (t.flashT > 0.0) entity_set_pos(t.flash, Math.cos(a) * muzzleD, Math.sin(a) * muzzleD);
    else node_set_active(t.flash, 0);                     // 炮口焰只在开火后 0.07s 出现
    node_set_angle(t.hpfill, -aimToSpriteAngle(t.ang));
    node_set_angle(t.hpback, -aimToSpriteAngle(t.ang));
    setBar(t.hpfill, hpW, t.hp / t.maxHp, hpY);
}

/** 收车（阵亡/待命）：整车隐藏。
 *  逐个部件设 active=0（引擎只隐藏实体自身，父节点隐藏**不会**级联到子节点 —— 实测）。 */
function hideTank(t) {
    setTankShown(t, false);
}

/* ============================ 6. 炮弹 / 粒子池 ============================ */

function makeBulletPool(n, radius, r, g, b) {
    const out = [];
    for (let i = 0; i < n; ++i) {
        const id = makeCircle(0.0, 0.0, radius, r, g, b);
        park(id);
        out.push({ id: id, alive: false, x: 0, y: 0, vx: 0, vy: 0, life: 0, team: 0 });
    }
    return out;
}

function takeBullet(pool) {
    for (let i = 0; i < pool.length; ++i) if (!pool[i].alive) return pool[i];
    return null;
}

function liveCount(pool) {
    let n = 0;
    for (let i = 0; i < pool.length; ++i) if (pool[i].alive) n++;
    return n;
}

function spawnBullet(pool, x, y, vx, vy, team, life) {
    const b = takeBullet(pool);
    if (!b) return 0;
    b.alive = true; b.x = x; b.y = y; b.vx = vx; b.vy = vy; b.life = life; b.team = team;
    entity_set_pos(b.id, x, y);
    unpark(b.id);
    return 1;
}

function killBullet(b) { b.alive = false; park(b.id); }

/** 取一个空闲的冲击波槽（爆炸时"放大 + 淡出"的那一圈）。 */
function takeWave() {
    for (let i = 0; i < waves.length; ++i) if (!waves[i].alive) return waves[i];
    return null;
}

function explode(x, y, n, power) {
    const w = takeWave();
    if (w) {
        w.alive = true; w.x = x; w.y = y; w.life = 0.32; w.maxLife = 0.32;
        entity_set_pos(w.id, x, y);
        node_set_scale(w.id, 0.5);
        node_set_opacity(w.id, 0.85);
        unpark(w.id);
    }
    for (let i = 0; i < n; ++i) {
        let p = null;
        for (let j = 0; j < parts.length; ++j) if (!parts[j].alive) { p = parts[j]; break; }
        if (!p) return;
        const ang = rng() * 360.0 * DEG;
        const sp = power * (0.35 + rng() * 0.65);
        p.alive = true;
        p.x = x; p.y = y;
        p.vx = Math.cos(ang) * sp; p.vy = Math.sin(ang) * sp;
        p.life = 0.28 + rng() * 0.30; p.maxLife = p.life;
        const c = rng();
        if (c < 0.42) { p.r = 1.00; p.g = 0.62; p.b = 0.18; }
        else if (c < 0.78) { p.r = 1.00; p.g = 0.88; p.b = 0.36; }
        else { p.r = 0.42; p.g = 0.36; p.b = 0.30; }
        node_set_number(p.id, 'scn.Sprite', 'r', p.r);
        node_set_number(p.id, 'scn.Sprite', 'g', p.g);
        node_set_number(p.id, 'scn.Sprite', 'b', p.b);
        node_set_scale(p.id, 1.0);
        node_set_opacity(p.id, 1.0);
        entity_set_pos(p.id, p.x, p.y);
        unpark(p.id);
    }
}

/** 掩体的 AABB 列表（从场景读：位置 + sprite 的 width/height）。 */
function readCrates() {
    const out = [];
    for (let i = 1; i <= 5; ++i) {
        const id = entity_find('crate' + i);
        if (id < 0) continue;
        out.push({
            x: entity_x(id), y: entity_y(id),
            hw: node_get_number(id, 'scn.Sprite', 'width') * 0.5,
            hh: node_get_number(id, 'scn.Sprite', 'height') * 0.5
        });
    }
    return out;
}

/** 圆 vs AABB：把圆心夹到盒内取最近点，比距离。 */
function circleHitsRect(cx, cy, r, rc) {
    const nx = clamp(cx, rc.x - rc.hw, rc.x + rc.hw);
    const ny = clamp(cy, rc.y - rc.hh, rc.y + rc.hh);
    return dist2(cx, cy, nx, ny) <= r * r;
}

function hitsAnyCrate(x, y, r) {
    for (let i = 0; i < crates.length; ++i) if (circleHitsRect(x, y, r, crates[i])) return true;
    return false;
}

/** 把圆推出所有掩体（取最小穿透轴）→ 坦克不会开进木箱里。 */
function pushOutOfCrates(x, y, r) {
    for (let i = 0; i < crates.length; ++i) {
        const c = crates[i];
        if (!circleHitsRect(x, y, r, c)) continue;
        const dx = x - c.x, dy = y - c.y;
        const ox = (c.hw + r) - Math.abs(dx);
        const oy = (c.hh + r) - Math.abs(dy);
        if (ox < oy) x += (dx >= 0 ? ox : -ox);
        else y += (dy >= 0 ? oy : -oy);
    }
    return { x: x, y: y };
}

/* ============================ 7. 开火 / 伤害 / 击毁 ============================ */

function firePlayer(force) {
    if (!player.alive) return 0;
    if (!force && player.cd > 0.0) return 0;
    if (liveCount(pbullets) >= P_MAX_LIVE) return 0;
    const a = player.aim * DEG;
    const mx = player.x + Math.cos(a) * P_MUZZLE_D;
    const my = player.y + Math.sin(a) * P_MUZZLE_D;
    if (!spawnBullet(pbullets, mx, my, Math.cos(a) * B_SPEED, Math.sin(a) * B_SPEED, 0, B_LIFE)) return 0;
    player.cd = P_FIRE_CD;
    player.flashT = 0.07;
    return 1;
}

function fireEnemy(e) {
    if (liveCount(ebullets) >= E_MAX_LIVE) return 0;
    const a = e.aim * DEG;
    const mx = e.x + Math.cos(a) * E_MUZZLE_D;
    const my = e.y + Math.sin(a) * E_MUZZLE_D;
    if (!spawnBullet(ebullets, mx, my, Math.cos(a) * E_EB_SPEED, Math.sin(a) * E_EB_SPEED, 1, B_LIFE)) return 0;
    e.cd = E_FIRE_CD + rng() * 0.7;
    e.flashT = 0.07;
    return 1;
}

function damageEnemy(e, dmg, hitX, hitY) {
    e.hp -= dmg;
    e.hitT = 0.11;
    tintTank(e, 2.0);                                     // 命中闪白（0.11s 后还原）
    explode(hitX, hitY, 3, 70.0);
    log('tank: hit enemy=' + e.idx + ' hp=' + Math.round(e.hp) + ' frame=' + director_frame());
    if (e.hp <= 0.0) killEnemy(e);
}

function killEnemy(e) {
    e.alive = false;
    e.hp = 0.0;
    aliveCount--;
    score += 100; totalScore += 100; totalKills++;
    explode(e.x, e.y, 9, 190.0);
    if (e.idx >= 1 && e.idx <= wrecks.length) {
        entity_set_pos(wrecks[e.idx - 1], e.x, e.y);
        unpark(wrecks[e.idx - 1]);
    }
    hideTank(e);
    log('tank: kill idx=' + e.idx + ' total=' + totalKills + ' score=' + score +
        ' total_score=' + totalScore + ' alive=' + aliveCount + ' frame=' + director_frame());
    if (aliveCount <= 0) {
        state = 'clear';
        clearTimer = 2.0;
        log('tank: wave ' + wave + ' cleared frame=' + director_frame());
    }
}

function damagePlayer(dmg) {
    player.hp -= dmg;
    player.hitT = 0.11;
    tintTank(player, 2.0);
    explode(player.x, player.y, 3, 70.0);
    log('tank: player hit hp=' + Math.round(player.hp) + ' frame=' + director_frame());
    if (player.hp <= 0.0) {
        player.hp = 0.0;
        player.alive = false;
        explode(player.x, player.y, 14, 210.0);
        entity_set_pos(wreckPlayer, player.x, player.y);      // 玩家残骸留在原地
        unpark(wreckPlayer);
        hideTank(player);
        park(hud.hpfill);
        state = 'dead';
        deadTimer = 0.0;
        if (score > best) best = score;
        log('tank: player dead score=' + score + ' best=' + best + ' total_kills=' + totalKills +
            ' frame=' + director_frame());
    }
}

/* ============================ 8. 波次 / 重开 ============================ */

let wrecks = [];
let wreckPlayer = -1;

function pickPatrolPoint() {
    for (let i = 0; i < 8; ++i) {
        const x = 60.0 + rng() * (W - 120.0);
        const y = 70.0 + rng() * (HUD_TOP - 130.0);
        if (!hitsAnyCrate(x, y, E_RADIUS + 8.0)) return { x: x, y: y };
    }
    return { x: 320.0, y: 240.0 };
}

function resetEnemy(e, sp) {
    e.x = sp.x; e.y = sp.y;
    e.ang = rng() * 360.0;
    e.aim = e.ang;
    e.vel = 0.0;
    e.hp = E_BASE_HP + (wave - 1) * E_HP_PER_WAVE;
    e.maxHp = e.hp;
    e.cd = 0.6 + rng() * 1.0;
    e.alive = true;
    e.flashT = 0.0;
    e.hitT = 0.0;
    e.wp = pickPatrolPoint();
    const p = pushOutOfCrates(e.x, e.y, E_RADIUS);
    e.x = clamp(p.x, 24.0, W - 24.0);
    e.y = clamp(p.y, 30.0, HUD_TOP - 20.0);
    tintTank(e, 1.0);
    setTankShown(e, true);
}

function spawnWave() {
    /* 出生点 = 场景里 5 辆敌坦的**初始位置**（场景是唯一真相），按引擎 RNG 洗牌后分配。
     * 被分到玩家鼻子底下（< 180px）的点会被推到最远的那个点 → 开局不贴脸。 */
    const idx = [];
    for (let i = 0; i < spawns.length; ++i) idx.push(i);
    for (let i = idx.length - 1; i > 0; --i) {
        const j = Math.floor(rng() * (i + 1)) % (i + 1);
        const t = idx[i]; idx[i] = idx[j]; idx[j] = t;
    }
    for (let k = 0; k < enemies.length; ++k) {
        const e = enemies[k];
        const sp = spawns[idx[k % idx.length]];
        resetEnemy(e, sp);
        if (state === 'dead' || dist(e.x, e.y, player.x, player.y) < 180.0) {
            /* 玩家在附近就换到离玩家最远的出生点 */
            let bestSp = spawns[0], bestD = -1.0;
            for (let i = 0; i < spawns.length; ++i) {
                const d = dist2(spawns[i].x, spawns[i].y, player.x, player.y);
                if (d > bestD) { bestD = d; bestSp = spawns[i]; }
            }
            resetEnemy(e, bestSp);
        }
    }
    aliveCount = enemies.length;
    log('tank: wave ' + wave + ' start enemies=' + aliveCount + ' hp=' + Math.round(enemies[0].maxHp));
}

function resetBullets() {
    for (let i = 0; i < pbullets.length; ++i) killBullet(pbullets[i]);
    for (let i = 0; i < ebullets.length; ++i) killBullet(ebullets[i]);
    for (let i = 0; i < parts.length; ++i) { parts[i].alive = false; park(parts[i].id); }
    for (let i = 0; i < waves.length; ++i) { waves[i].alive = false; park(waves[i].id); }
    for (let i = 0; i < wrecks.length; ++i) park(wrecks[i]);
    park(wreckPlayer);
    for (let i = 0; i < tracks.length; ++i) { park(tracks[i]); trackAge[i] = -1.0; }
    trackIdx = 0;
}

function restart() {
    score = 0;
    wave = 1;
    state = 'play';
    deadTimer = 0.0;
    clearTimer = 0.0;
    player.hp = player.maxHp;
    player.alive = true;
    player.x = 320.0; player.y = 90.0;
    player.ang = 90.0; player.aim = 90.0; player.vel = 0.0;
    player.cd = 0.0; player.flashT = 0.0; player.hitT = 0.0;
    tintTank(player, 1.0);
    setTankShown(player, true);
    resetBullets();
    spawnWave();
    ui_set_text(hud.state, '');
    log('tank: restart frame=' + director_frame());
}

/* ============================ 9. 每帧推进 ============================ */

function nearestEnemy() {
    let best = null, bd = 1e18;
    for (let i = 0; i < enemies.length; ++i) {
        const e = enemies[i];
        if (!e.alive) continue;
        const d = dist2(e.x, e.y, player.x, player.y);
        if (d < bd) { bd = d; best = e; }
    }
    return best;
}

/** 预判瞄准点：从 (sx,sy) 以 speed 发射的炮弹，飞行时间里目标 t 会移动到的位置。
 *  **只影响朝向**（谁瞄谁是这个函数的参数，别搞反）——命中仍是真实判定。 */
function leadPoint(sx, sy, t, speed) {
    const tof = dist(sx, sy, t.x, t.y) / speed;
    const a = t.ang * DEG;
    return { x: t.x + Math.cos(a) * t.vel * tof, y: t.y + Math.sin(a) * t.vel * tof };
}

/** 自驱动（MAQUE_TANK_AUTOPLAY=1）：把"人怎么开这一局"编码成输入。 */
function autoplayInput() {
    const out = { fwd: 0, turn: 0, fire: false, aim: null };
    const t = nearestEnemy();
    if (!t) return out;
    const d = dist(t.x, t.y, player.x, player.y);
    const bodyWant = Math.atan2(t.y - player.y, t.x - player.x) / DEG;
    const diff = angDiff(bodyWant, player.ang);
    if (diff > 10.0) out.turn = 1;
    else if (diff < -10.0) out.turn = -1;
    if (Math.abs(diff) < 30.0) out.fwd = d > E_KEEP_FAR ? 1 : (d < E_KEEP_NEAR ? -1 : 0);
    if (player.hp < 35.0) out.fwd = -1;                  // 血少就撤（只是输入）
    const lp = leadPoint(player.x, player.y, t, B_SPEED);
    out.aim = lp;
    const aimWant = Math.atan2(lp.y - player.y, lp.x - player.x) / DEG;
    out.fire = d < E_RANGE && Math.abs(angDiff(aimWant, player.aim)) < 9.0;
    return out;
}

function updatePlayer(dt) {
    if (!player.alive) return;

    let inFwd = 0, inTurn = 0, inFire = false, aimTo = null;

    if (autoplay) {
        const a = autoplayInput();
        inFwd = a.fwd; inTurn = a.turn; inFire = a.fire;
        if (a.aim) aimTo = a.aim;
    } else {
        if (held(K_W)) inFwd = 1; else if (held(K_S)) inFwd = -1;
        if (held(K_A)) inTurn = 1; else if (held(K_D)) inTurn = -1;
        if (input_down() || held(K_SPACE)) inFire = true;
    }

    /* 测试钩子：KILL_AT 之后的 150 帧持续把炮口对准最近敌人（只改朝向），
     * 对准之后按**正常冷却**继续开火 —— 单发 20 伤害，敌坦 40 血，两发即击毁。 */
    if (aimHookFrames > 0) {
        aimHookFrames--;
        const t = nearestEnemy();
        if (t) {
            aimTo = leadPoint(player.x, player.y, t, B_SPEED);
            const err = Math.abs(angDiff(Math.atan2(aimTo.y - player.y, aimTo.x - player.x) / DEG, player.aim));
            if (err < 6.0 && player.cd <= 0.0) inFire = true;
        }
    }

    /* 车体：A/D 原地转，W/S 沿车体朝向走 */
    if (player.cd > 0.0) player.cd -= dt;                 // 冷却推进（开火门槛）
    player.ang += inTurn * P_TURN * dt;
    const target = inFwd > 0 ? P_MOVE : (inFwd < 0 ? -P_BACK : 0.0);
    player.vel = approach(player.vel, target, P_ACCEL * dt);
    const a = player.ang * DEG;
    let nx = player.x + Math.cos(a) * player.vel * dt;
    let ny = player.y + Math.sin(a) * player.vel * dt;
    nx = clamp(nx, 22.0, W - 22.0);
    ny = clamp(ny, 24.0, HUD_TOP - 18.0);
    const p = pushOutOfCrates(nx, ny, P_RADIUS);
    player.x = p.x; player.y = p.y;

    /* 炮塔：默认指向**指针位置**（引擎给的已是世界坐标），钩子/自动模式下指向预判点 */
    if (aimTo) {
        player.aim = turnToward(player.aim, Math.atan2(aimTo.y - player.y, aimTo.x - player.x) / DEG,
                                P_TURRET_TURN * dt);
    } else if (pointerHave) {
        player.aim = turnToward(player.aim, Math.atan2(input_y() - player.y, input_x() - player.x) / DEG,
                                P_TURRET_TURN * dt);
    }

    if (inFire || fireQueue) { firePlayer(false); }
    fireQueue = false;

    /* 履带印：每 TRACK_EVERY 帧在两条履带位置各落一个，环形复用；
     * 旧印靠**透明度淡出**（引擎现在支持 sprite 透明度）→ 车辙自然消失，不用硬回收。 */
    trackTimer++;
    if (trackTimer >= TRACK_EVERY && Math.abs(player.vel) > 12.0) {
        trackTimer = 0;
        const px = -Math.sin(a), py = Math.cos(a);        // 车体法线
        for (let s = -1; s <= 1; s += 2) {
            const k = trackIdx % tracks.length;
            trackIdx++;
            entity_set_pos(tracks[k], player.x + px * 11.0 * s - Math.cos(a) * 4.0,
                                      player.y + py * 11.0 * s - Math.sin(a) * 4.0);
            trackAge[k] = 0.0;
            node_set_opacity(tracks[k], 0.75);
            node_set_angle(tracks[k], aimToSpriteAngle(player.ang));
            unpark(tracks[k]);
        }
    }
    for (let i = 0; i < tracks.length; ++i) {
        if (trackAge[i] < 0.0) continue;
        trackAge[i] += dt;
        const o = 1.0 - trackAge[i] / TRACK_LIFE;
        if (o <= 0.0) { trackAge[i] = -1.0; park(tracks[i]); continue; }
        node_set_opacity(tracks[i], o * 0.75);
    }

    player.flashT = Math.max(0.0, player.flashT - dt);
    if (player.hitT > 0.0) {
        player.hitT -= dt;
        if (player.hitT <= 0.0) tintTank(player, 1.0);
    }
    placeTank(player, P_BARREL_D, P_MUZZLE_D, P_NOSE_D, P_HP_Y, P_HP_W);
}

function updateEnemies(dt) {
    for (let i = 0; i < enemies.length; ++i) {
        const e = enemies[i];
        if (!e.alive) continue;
        const d = dist(e.x, e.y, player.x, player.y);
        const toPlayer = Math.atan2(player.y - e.y, player.x - e.x) / DEG;
        let turretWant = e.ang, drive = 0.0;

        if (player.alive && d < E_SIGHT) {
            /* 发现玩家：车体转向、保持距离、预判瞄准 */
            e.ang = turnToward(e.ang, toPlayer, E_TURN * 0.6 * dt);
            drive = d > E_KEEP_FAR ? E_SPEED : (d < E_KEEP_NEAR ? -E_SPEED * 0.8 : 0.0);
            const lp = leadPoint(e.x, e.y, player, E_EB_SPEED);
            turretWant = Math.atan2(lp.y - e.y, lp.x - e.x) / DEG;
        } else {
            /* 巡逻：走到路点就换一个（rng → 可复现） */
            if (dist(e.x, e.y, e.wp.x, e.wp.y) < 26.0) e.wp = pickPatrolPoint();
            const wpA = Math.atan2(e.wp.y - e.y, e.wp.x - e.x) / DEG;
            e.ang = turnToward(e.ang, wpA, E_TURN * dt);
            if (Math.abs(angDiff(wpA, e.ang)) < 40.0) drive = E_SPEED;
            turretWant = e.ang;
        }

        e.aim = turnToward(e.aim, turretWant, E_TURRET_TURN * dt);
        e.vel = approach(e.vel, drive, 420.0 * dt);
        const a = e.ang * DEG;
        let nx = clamp(e.x + Math.cos(a) * e.vel * dt, 24.0, W - 24.0);
        let ny = clamp(e.y + Math.sin(a) * e.vel * dt, 30.0, HUD_TOP - 20.0);
        const p = pushOutOfCrates(nx, ny, E_RADIUS);
        e.x = p.x; e.y = p.y;

        /* 开火：在射程内 + 炮口对准（误差 < 8°）+ 冷却结束 */
        e.cd -= dt;
        if (player.alive && d < E_RANGE && e.cd <= 0.0 &&
            Math.abs(angDiff(toPlayer, e.aim)) < 8.0) {
            fireEnemy(e);
        }

        e.flashT = Math.max(0.0, e.flashT - dt);
        if (e.hitT > 0.0) {
            e.hitT -= dt;
            if (e.hitT <= 0.0) tintTank(e, 1.0);
        }
        placeTank(e, E_BARREL_D, E_MUZZLE_D, E_NOSE_D, E_HP_Y, E_HP_W);
    }
}

function updateBullets(dt) {
    /* 玩家弹 → 敌坦 */
    for (let i = 0; i < pbullets.length; ++i) {
        const b = pbullets[i];
        if (!b.alive) continue;
        b.x += b.vx * dt; b.y += b.vy * dt; b.life -= dt;
        if (b.life <= 0.0 || b.x < 0.0 || b.x > W || b.y < 0.0 || b.y > H) { killBullet(b); continue; }
        if (hitsAnyCrate(b.x, b.y, B_R)) { explode(b.x, b.y, 2, 60.0); killBullet(b); continue; }
        let hit = false;
        for (let j = 0; j < enemies.length; ++j) {
            const e = enemies[j];
            if (!e.alive) continue;
            if (dist2(b.x, b.y, e.x, e.y) <= (E_RADIUS + B_R) * (E_RADIUS + B_R)) {
                damageEnemy(e, P_DMG, b.x, b.y);
                killBullet(b);
                hit = true;
                break;
            }
        }
        if (!hit) entity_set_pos(b.id, b.x, b.y);
    }
    /* 敌方弹 → 玩家 */
    for (let i = 0; i < ebullets.length; ++i) {
        const b = ebullets[i];
        if (!b.alive) continue;
        b.x += b.vx * dt; b.y += b.vy * dt; b.life -= dt;
        if (b.life <= 0.0 || b.x < 0.0 || b.x > W || b.y < 0.0 || b.y > H) { killBullet(b); continue; }
        if (hitsAnyCrate(b.x, b.y, B_R)) { explode(b.x, b.y, 2, 60.0); killBullet(b); continue; }
        if (player.alive && dist2(b.x, b.y, player.x, player.y) <= (P_RADIUS + B_R) * (P_RADIUS + B_R)) {
            damagePlayer(E_DMG);
            killBullet(b);
            continue;
        }
        entity_set_pos(b.id, b.x, b.y);
    }
}

function updateParticles(dt) {
    for (let i = 0; i < parts.length; ++i) {
        const p = parts[i];
        if (!p.alive) continue;
        p.life -= dt;
        if (p.life <= 0.0) { p.alive = false; park(p.id); continue; }
        p.x += p.vx * dt; p.y += p.vy * dt;
        p.vx *= (1.0 - 1.8 * dt); p.vy *= (1.0 - 1.8 * dt);
        const k = p.life / p.maxLife;
        node_set_scale(p.id, 0.30 + 0.90 * k);            // 缩放/透明都走节点属性（引擎已支持）
        node_set_opacity(p.id, k * 0.95 + 0.05);
        entity_set_pos(p.id, p.x, p.y);
    }
    for (let i = 0; i < waves.length; ++i) {
        const w = waves[i];
        if (!w.alive) continue;
        w.life -= dt;
        if (w.life <= 0.0) { w.alive = false; park(w.id); continue; }
        const k = 1.0 - w.life / w.maxLife;
        node_set_scale(w.id, 0.5 + 2.6 * k);              // 火球扩散
        node_set_opacity(w.id, 0.85 * (1.0 - k));
    }
}

function updateHud() {
    ui_set_text(hud.hp, 'HP ' + Math.round(player.hp));
    ui_set_text(hud.score, 'SCORE ' + score);
    ui_set_text(hud.wave, 'WAVE ' + wave);
    ui_set_text(hud.enemy, 'ENEMY ' + aliveCount);
    /* 血条：ProgressBar 是引擎侧的真相（脚本写 ui_set_value → 引擎钳制到 0..1）。
     * 引擎的 ui_step 只会把填充子节点的 **ui.Control.offset_max.x** 改成 full×value，
     * 而 sprite 提取只读 sprite 自己的 width/height（两个字段不互通），
     * 所以这里把引擎回读到的 value 同步到 sprite 宽度上 —— 状态与画面同源，不是各算一份。 */
    ui_set_value(hud.hpbar, player.hp / player.maxHp);
    const v = node_get_number(hud.hpbar, 'ui.ProgressBar', 'value');
    setBar(hud.hpfill, 180.0, v, 0.0);
    const msg = state === 'dead' ? ('GAME OVER  -  CLICK OR SPACE TO RESTART')
              : (state === 'clear' ? ('WAVE ' + wave + ' CLEARED') : '');
    ui_set_text(hud.state, msg);
}

/* MAQUE_TANK_TRACE=1：每 60 帧打一行战况（调试用；不影响判定） */
let traceOn = false, traceFrame = 0;

function traceState(frame) {
    if (!traceOn || frame < traceFrame) return;
    traceFrame = frame + 60;
    let s = 'tank: trace frame=' + frame + ' p=(' + Math.round(player.x) + ',' + Math.round(player.y) +
            ') ang=' + Math.round(player.ang) + ' aim=' + Math.round(player.aim) +
            ' hp=' + Math.round(player.hp) + ' alive=' + aliveCount + ' state=' + state +
            ' pb=' + liveCount(pbullets) + ' eb=' + liveCount(ebullets);
    for (let i = 0; i < enemies.length; ++i) {
        const e = enemies[i];
        if (!e.alive) { s += ' e' + e.idx + '=dead'; continue; }
        s += ' e' + e.idx + '=(' + Math.round(e.x) + ',' + Math.round(e.y) + ',d' +
             Math.round(dist(e.x, e.y, player.x, player.y)) + ',cd' + e.cd.toFixed(1) +
             ',aim' + Math.round(e.aim) + ',err' +
             Math.round(angDiff(Math.atan2(player.y - e.y, player.x - e.x) / DEG, e.aim)) + ')';
    }
    log(s);
}

function update(dt) {
    if (!player) return;
    const frame = director_frame();
    traceState(frame);

    /* ---- 测试钩子：到帧**立即把炮口对准最近敌人**并强制开火一次 ----
     * 只改朝向与开火时机（绕过冷却）；之后 150 帧继续盯住该敌人，
     * 后续射击走**正常冷却**（firePlayer(false)）→ 命中/扣血/击毁/计分全是真实路径。 */
    if (killAtFrame > 0 && frame >= killAtFrame) {
        killAtFrame = 0;
        aimHookFrames = 150;
        const t = nearestEnemy();
        if (t) {
            const lp0 = leadPoint(player.x, player.y, t, B_SPEED);
            player.aim = Math.atan2(lp0.y - player.y, lp0.x - player.x) / DEG;
        }
        player.cd = 0.0;
        const fired = firePlayer(true);
        log('tank: kill hook fired frame=' + frame + ' fired=' + fired +
            ' aim=' + Math.round(player.aim) + ' target=' + (t ? t.idx : -1));
    }

    if (state === 'dead') {
        deadTimer += dt;
        updateParticles(dt);
        updateBullets(dt);                    // 让残余炮弹自然飞完/回收
        if (autoplay && deadTimer > 1.0) restart();
        return;
    }

    updatePlayer(dt);
    updateEnemies(dt);
    updateBullets(dt);
    updateParticles(dt);

    if (state === 'clear') {
        clearTimer -= dt;
        if (clearTimer <= 0.0) {
            wave++;
            state = 'play';
            spawnWave();
        }
    }
    updateHud();
}

/* ============================ 10. 输入回调（签名来自 schema/script.yaml） ============================ */

function on_pointer_move(target, x, y) { pointerHave = true; }

function on_pointer_down(target, x, y) {
    pointerHave = true;
    if (state === 'dead') { if (deadTimer > 0.4) restart(); return; }
    fireQueue = true;
}

function on_key_down(target, key) {
    let space = false;
    for (let i = 0; i < K_SPACE.length; ++i) if (key === K_SPACE[i]) space = true;
    if (key === 3 || key === 13) space = true;             // 回车也算确认
    if (!space) return;
    if (state === 'dead') { if (deadTimer > 0.4) restart(); return; }
    fireQueue = true;
}

/* ============================ 11. 启动 ============================ */

function on_start() {
    /* 场景实体（名字唯一；先声明先画：地面 → 装饰 → 履带印 → 坦克 → 残骸 → HUD） */
    player = findParts('player');
    const enemyParts = [];
    for (let i = 1; i <= 5; ++i) enemyParts.push(findParts('e' + i));
    wrecks = [];
    for (let i = 1; i <= 5; ++i) wrecks.push(entity_find('wreck' + i));
    wreckPlayer = entity_find('wreck_player');

    if (!partsOk(player)) { log('tank: player entities missing'); quit(); return; }
    for (let i = 0; i < enemyParts.length; ++i) {
        if (!partsOk(enemyParts[i])) { log('tank: enemy entities missing at ' + (i + 1)); quit(); return; }
    }

    /* HUD */
    hud.hp = entity_find('hud_hp');
    hud.score = entity_find('hud_score');
    hud.wave = entity_find('hud_wave');
    hud.enemy = entity_find('hud_enemy');
    hud.hpbar = entity_find('hud_hpbar');
    hud.hpfill = entity_find('hud_hpfill');
    hud.state = entity_find('hud_state');
    hud.hint = entity_find('hud_hint');

    crates = readCrates();
    for (let i = 1; i <= POOL_TRACK; ++i) {
        const id = entity_find('track' + i);
        if (id >= 0) { tracks.push(id); trackAge.push(-1.0); park(id); }
    }
    for (let i = 0; i < wrecks.length; ++i) park(wrecks[i]);
    park(wreckPlayer);

    /* 池化实体：**只在这里 spawn 一次**（之后靠 active 开关复用，绝不每帧 spawn/destroy） */
    pbullets = makeBulletPool(POOL_PB, 4.0, 1.00, 0.86, 0.34);
    ebullets = makeBulletPool(POOL_EB, 4.5, 1.00, 0.42, 0.30);
    for (let i = 0; i < POOL_PART; ++i) {
        const id = makeCircle(0.0, 0.0, 4.5, 1.0, 0.7, 0.3);
        park(id);
        parts.push({ id: id, alive: false, x: 0, y: 0, vx: 0, vy: 0, life: 0, maxLife: 1, r: 1, g: 1, b: 1 });
    }
    for (let i = 0; i < POOL_WAVE; ++i) {
        const id = makeCircle(0.0, 0.0, 13.0, 1.0, 0.84, 0.52);
        park(id);
        waves.push({ id: id, alive: false, x: 0, y: 0, life: 0, maxLife: 1 });
    }

    /* 坦克对象：玩家 + 5 辆敌坦（部件 id 来自场景，状态在这里） */
    const p = { shadow: player.shadow, hull: player.hull, turret: player.turret, nose: player.nose,
                barrel: player.barrel, flash: player.flash, hpfill: player.hpfill, hpback: player.hpback,
                bt: { r: 1.0, g: 1.0, b: 1.0 },
                x: 320.0, y: 90.0, ang: 90.0, aim: 90.0, vel: 0.0,
                hp: P_MAX_HP, maxHp: P_MAX_HP, cd: 0.0, flashT: 0.0, hitT: 0.0, alive: true };
    player = p;

    for (let i = 0; i < enemyParts.length; ++i) {
        const r = enemyParts[i];
        enemies.push({ idx: i + 1, shadow: r.shadow, hull: r.hull, turret: r.turret, nose: r.nose,
                       barrel: r.barrel, flash: r.flash, hpfill: r.hpfill, hpback: r.hpback,
                       bt: E_BARREL_TINT,
                       x: entity_x(r.hull), y: entity_y(r.hull), ang: 90.0, aim: 90.0, vel: 0.0,
                       hp: E_BASE_HP, maxHp: E_BASE_HP, cd: 1.0, flashT: 0.0, hitT: 0.0,
                       alive: true, wp: { x: 320.0, y: 240.0 } });
        spawns.push({ x: entity_x(r.hull), y: entity_y(r.hull) });
    }

    autoplay = env('MAQUE_TANK_AUTOPLAY') === '1';
    traceOn = env('MAQUE_TANK_TRACE') === '1';
    const kf = Number(env('MAQUE_TANK_KILL_AT'));
    if (isFinite(kf) && kf > 0) {
        killAtFrame = kf;
        log('tank: kill hook armed at frame ' + kf);
    }

    ui_set_text(hud.hint, 'WASD DRIVE   POINTER AIM   CLICK/SPACE FIRE');
    /* 只读自省入口：控制面 `Scene.emit <玩家车体 id> "state" 1` → 打一行战况（docs/19 §8 节点事件） */
    node_on(player.hull, 'state', 'on_state_query');
    resetBullets();
    spawnWave();
    updateHud();
    log('tank: ready ' + W + 'x' + H + ' enemies=' + enemies.length +
        ' autoplay=' + (autoplay ? 1 : 0) + ' wave=' + wave);
}

/* 控制面/工具读取当前战况的只读入口（`Scene.emit(<玩家车体 id>, "state", 1)` 触发）。
 * 注册在玩家车体上：跨语言/热重载都通用，且**不进入**游戏循环。 */
function on_state_query(id, value) {
    if (value <= 0.0) return;
    log('tank: state ' + 'state=' + state + ' score=' + score + ' wave=' + wave +
        ' alive=' + aliveCount + ' hp=' + Math.round(player ? player.hp : -1) +
        ' kills=' + totalKills + ' frame=' + director_frame());
}
