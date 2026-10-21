/* 官方 spine demo 移植 —— `demo-*.scn` 与 `all.scn` 共用本脚本。
 *
 * 依据：`.deps/official/DEMO-SPEC.md`（从上游 spine-runtimes 4.2 的 cocos2d-x/GLFW/SDL 入口提取的
 * 精确行为：素材、缩放、起始动画、排队动画、换皮肤、默认混合、背景色）与 `.deps/official/cocos/*Example.cpp`。
 *
 * 分工（保持 task-4 建立的解耦约定）：
 *   · 宿主 main.cpp 只调 mq_run_scene()；场景描述**素材与布局**；本脚本描述**行为**。
 *   · 引擎照旧不认识"demo/动画名"等概念，只提供 spine_ 与 node_/ui_ 原语。
 *
 * 场景识别：每个 demo 场景里有一个**标记实体**，名字就是配置表 DEMOS 的键（如 `demo.spineboy`）。
 * 这样脚本不需要额外的场景元数据通道（`cfg()` 只读标量，读不到 title 字符串）。
 *
 * 键位（引擎规范键空间，见 AGENTS.md）：↑=8 ↓=9 空格=2 + 字母（大写 ASCII）。
 *   ↑/↓ 上一个/下一个动画（沿用 task-4 的交互）、空格 暂停/继续、N 下一个 demo、
 *   鼠标点击 = 官方 demo 的「点一次开调试骨骼 → 再点 0.3 倍速 → 再点切下一个 demo」三段行为
 *   （官方后两级已复刻；第一级"调试骨骼绘制"引擎没有，如实打印 UNSUPPORTED 而不是假装）。
 *
 * 官方 11 个 demo 的**点击链路**（cocos2d-x 参考实现的 replaceScene 顺序）：
 *   Spineboy → Separator → Goblins → Raptor → Tank → Coin → MixAndMatch → Batching → Sequence → IK
 *   → Physics →（回到 Spineboy）；两个官方 C++ 入口 demo（glfw / sdl）接在 Physics 之后便于一次点完。
 */

const DEMO_DIR = env('SPINE_DEMO_DIR') || 'samples/spine/';

/* ---------------------------------------------------------------- 官方行为表
 * scale  = 官方骨架缩放（DEMO-SPEC 的「骨架缩放」列）
 * anim   = 官方起始动画；queue = 排队动画 [名字, 循环, 延迟]（官方 addAnimation）
 * mix    = 官方 setMix 列表 [from, to, seconds]；defaultMix = 官方 setDefaultMix
 * skin   = 官方起始皮肤（Goblins: "goblin"）
 * compose= 官方运行时合成皮肤（MixAndMatch 的 Skin::addSkin 列表）
 * bg     = 官方背景色（cocos2d 系列 LayerColor(128,128,128) → 0.502；SDL 入口是 (94,93,96)）
 * cellScale = all.scn 总览网格里用的缩放（官方缩放会让大骨架超出格子，如实标注"总览用缩放"）
 */
const DEMOS = {
    'demo.spineboy': {
        title: 'Spineboy', skel: '/art/spineboy-pro.json', atlas: '/art/spineboy.atlas',
        scale: 0.6, anim: 'walk', cellScale: 0.34, bg: [0.502, 0.502, 0.502],
        queue: [['jump', false, 1.0], ['run', true, 0]],
        mix: [['walk', 'jump', 0.4], ['jump', 'run', 0.4]],
        events: true, jumpedLog: true,
        desc: '官方 Spineboy：动画事件回调（start/interrupt/end/complete/dispose/event）+ 排队 walk→jump→run'
    },
    'demo.separator': {
        title: 'SkeletonRendererSeparator', skel: '/art/spineboy-pro.json', atlas: '/art/spineboy.atlas',
        scale: 0.6, anim: 'walk', cellScale: 0.34, bg: [0.502, 0.502, 0.502],
        mix: [['walk', 'jump', 0.4]],
        /* 官方：backNode 画 [rear-upper-arm .. rear-shin]，frontNode 画 [neck .. 末尾]，中间夹红条。
         * 我们的实现：**两个实体各持一个实例**（官方是"一个骨架 + 两个渲染节点"，引擎没有分次绘制
         * 同一实例的 API），两边同速同帧步进 → 观感与官方一致（代价是骨架数据/内存各一份）。 */
        slotsBack: ['rear-upper-arm', 'rear-shin'], slotsFront: ['neck', null], mirror: 'front',
        desc: '官方 Separator：把 spineboy 的槽区间拆成"后半身 / 前景"两层，中间夹一个红条'
    },
    'demo.goblins': {
        title: 'Goblins', skel: '/art/goblins-pro.json', atlas: '/art/goblins.atlas',
        scale: 1.5, anim: 'walk', cellScale: 0.42, bg: [0.502, 0.502, 0.502],
        skin: 'goblin',
        desc: '官方 Goblins：换皮肤 setSkin("goblin") 后播放 walk'
    },
    'demo.raptor': {
        title: 'Raptor', skel: '/art/raptor-pro.json', atlas: '/art/raptor.atlas',
        scale: 0.5, anim: 'walk', cellScale: 0.28, bg: [0.502, 0.502, 0.502],
        queue: [['gun-grab', false, 2.0]], queueTrack: 1, twoColor: true,
        desc: '官方 Raptor：track0 walk + track1 延迟 2s 的 gun-grab；官方还开了两色着色（TintBlack）'
    },
    'demo.tank': {
        title: 'Tank', skel: '/art/tank-pro.skel', atlas: '/art/tank.atlas',
        scale: 0.5, anim: 'shoot', cellScale: 0.12, bg: [0.502, 0.502, 0.502],
        desc: '官方 Tank：播放 shoot'
    },
    'demo.coin': {
        title: 'Coin', skel: '/art/coin-pro.skel', atlas: '/art/coin.atlas',
        scale: 1.0, anim: 'animation', cellScale: 0.5, bg: [0.502, 0.502, 0.502],
        desc: '官方 Coin：播放 animation（金币旋转）'
    },
    'demo.mixandmatch': {
        title: 'MixAndMatch', skel: '/art/mix-and-match-pro.skel', atlas: '/art/mix-and-match.atlas',
        scale: 0.5, anim: 'dance', cellScale: 0.28, bg: [0.502, 0.502, 0.502],
        compose: ['skin-base', 'nose/short', 'eyelids/girly', 'eyes/violet', 'hair/brown',
                  'clothes/hoodie-orange', 'legs/pants-jeans', 'accessories/bag', 'accessories/hat-red-yellow'],
        desc: '官方 MixAndMatch：用 Skin::addSkin 运行时合成 9 个部件皮肤，再播 dance'
    },
    'demo.batching': {
        title: 'Batching', skel: '/art/spineboy-pro.json', atlas: '/art/spineboy.atlas',
        scale: 0.6, anim: 'walk', cellScale: 0.16, bg: [0.502, 0.502, 0.502],
        count: 50, mix: [['walk', 'jump', 0.2], ['jump', 'run', 0.2]],
        queue: [['jump', true, 'random0to3'], ['run', true, 0]],
        desc: '官方 Batching：同屏 50 个 spineboy（共享图集）各自 walk + 随机延迟 jump + run'
    },
    'demo.sequence': {
        title: 'Sequence', skel: '/art/dragon-ess.skel', atlas: '/art/dragon-pma.atlas',
        scale: 1.0, anim: 'flying', cellScale: 0.28, bg: [0.502, 0.502, 0.502],
        sequence: true,
        desc: '官方 Sequence：dragon 的 flying —— 序列帧（Sequence）附件随时间换帧'
    },
    'demo.ik': {
        title: 'IK', skel: '/art/spineboy-pro.json', atlas: '/art/spineboy.atlas',
        scale: 0.6, anim: 'walk', cellScale: 0.34, bg: [0.502, 0.502, 0.502],
        track1: 'aim', bone: 'crosshair',
        desc: '官方 IK：track0 walk + track1 aim，指针位置驱动 crosshair 骨骼 → IK 手臂跟随'
    },
    'demo.physics': {
        title: 'Physics', skel: '/art/celestial-circus-pro.skel', atlas: '/art/celestial-circus.atlas',
        scale: 0.2, anim: '', cellScale: 0.30, bg: [0.502, 0.502, 0.502],
        drag: true,
        desc: '官方 Physics：celestial-circus 的物理约束（骨骼链下垂/摆动），拖动平移整组'
    },
    'demo.glfw': {
        title: 'spine-glfw（官方 C++ 入口）', skel: '/art/spineboy-pro.skel', atlas: '/art/spineboy-pma.atlas',
        scale: 0.3, anim: 'portal', cellScale: 0.26, bg: [0.0, 0.0, 0.0],
        queue: [['run', true, 0]], defaultMix: 0.2, boneYDown: true,
        desc: '官方 spine-glfw 入口：portal→run 排队，defaultMix 0.2，窗口 800x600，缩放 0.3'
    },
    'demo.sdl': {
        title: 'spine-sdl（官方 C++ 入口）', skel: '/art/spineboy-pro.json', atlas: '/art/spineboy-pma.atlas',
        scale: 0.5, anim: 'portal', cellScale: 0.36, bg: [94 / 255, 93 / 255, 96 / 255],
        queue: [['run', true, 0]], defaultMix: 0.2, boneYDown: true,
        desc: '官方 spine-sdl 入口：portal→run，defaultMix 0.2，背景 (94,93,96)，缩放 0.5'
    },
    /* all.scn 的"标记"配置：只用于识别场景种类，每个格子的缩放/动画由 CELLS 表逐个套用 */
    'demo.all': {
        title: '全部官方 demo 总览', skel: '', atlas: '',
        scale: 1.0, anim: '', cellScale: 1.0, bg: [0.10, 0.11, 0.14],
        desc: '11 个官方 cocos2d-x demo + 2 个官方 C++ 入口 demo，同屏网格（每个 demo 另有独立场景）'
    }
};

/* all.scn 的网格单元顺序（12 个骨架一排一个格子；batching 在总览里只放 1 个代表） */
const CELLS = ['demo.spineboy', 'demo.goblins', 'demo.coin', 'demo.raptor',
               'demo.tank', 'demo.mixandmatch', 'demo.sequence', 'demo.ik',
               'demo.physics', 'demo.separator', 'demo.glfw', 'demo.sdl'];

/* 点击链路（官方 replaceScene 顺序 + 两个 C++ 入口接在后面） */
const CHAIN = ['demo.spineboy', 'demo.separator', 'demo.goblins', 'demo.raptor', 'demo.tank',
               'demo.coin', 'demo.mixandmatch', 'demo.batching', 'demo.sequence', 'demo.ik',
               'demo.physics', 'demo.glfw', 'demo.sdl'];

/* 引擎能力探测：task-5/task-6 已把下表多数 API 落地；探测是为了**旧引擎上也不炸**，
 * 缺哪个就如实打印 UNSUPPORTED（而不是静默不生效）。 */
const HAS = {
    setAnimation: typeof spine_set_animation === 'function',
    addAnimation: typeof spine_add_animation === 'function',
    setMix:       typeof spine_set_mix === 'function',
    defaultMix:   typeof spine_set_default_mix === 'function',
    events:       typeof spine_event_count === 'function',
    trackAnim:    typeof spine_track_animation === 'function',
    boneSetWorld: typeof spine_bone_set_world_position === 'function',
    boneFind:     typeof spine_bone_find === 'function',
    updateWorld:  typeof spine_update_world_transform === 'function',
    slotsRange:   typeof spine_set_slots_range === 'function',
    slotIndex:    typeof spine_slot_index === 'function',
    skinCompose:  typeof spine_skin_compose === 'function',
    skinAttach:   typeof spine_skin_attachment_count === 'function',
    twoColor:     typeof spine_set_two_color === 'function',
    physTranslate: typeof spine_physics_translate === 'function',
    seqIndex:     typeof spine_sequence_index === 'function',
    nodeOn:       typeof node_on === 'function'
};

let demoKey = '';          // 标记实体名（DEMOS 的键）
let cfg = null;            // 该 demo 的配置
let marker = -1;           // 标记实体 id
let members = [];          // 参与本场景的全部骨架实体（单骨架 = [marker]，batching = 50，all = 12）
let mirror = -1;           // Separator 的"前景"实体（同配置镜像播放）
let hud = -1, hudBase = '';
let names = [];            // 当前"主"骨架的动画清单
let idx = 0;               // 主骨架当前动画下标
let paused = false;
let speed = 1.0;
let clicks = 0;            // 官方三段点击：调试骨骼 → 0.3 倍速 → 下一个 demo
let unsupported = [];      // 如实记录的"引擎暂不支持"项
let label = '';            // 当前 demo 的显示名（HUD/日志用）
let lastPointer = null;    // Physics 拖动用的上一个指针位置（官方 lastMousePosition）
let seqSlot = -1;          // Sequence：监视的槽下标
let seqFrame0 = -1;        // Sequence：本帧下标（用于打印"真的换帧了"）
const SEQ_WATCH_SLOT = 'head';   // dragon 的 flying 里带 sequence 的槽（实测 pages=5 的槽）

function log2(msg) { log('spine[' + demoKey.replace('demo.', '') + '] ' + msg); }
function fmt(x) { return Math.round(x * 100) / 100; }
function clearTo(rgb) { clear(rgb[0], rgb[1], rgb[2]); }
function note(what) { if (unsupported.indexOf(what) < 0) unsupported.push(what); }

/** 找到本场景的标记实体（DEMOS 的键之一） */
function findDemoKey() {
    for (const k in DEMOS) if (entity_find(k) >= 0) return k;
    return '';
}

/** 主骨架：标记实体本身；all.scn 用 cell0 */
function primaryEntity() {
    if (demoKey === 'demo.all') { const c = entity_find('cell0'); return c >= 0 ? c : marker; }
    return members.length ? members[0] : marker;
}

function skelInfo(id) {
    const n = spine_animation_count(id);
    const list = [];
    for (let i = 0; i < n; ++i) list.push(spine_animation_name(id, i));
    return list;
}
function skinInfo(id) {
    const n = spine_skin_count(id);
    const list = [];
    for (let i = 0; i < n; ++i) list.push(spine_skin_name(id, i));
    return list;
}

/** 实测打印一个骨架的动画/皮肤/图集页清单（报告与 README 以此为准） */
function logSkeleton(tag, id) {
    const t = [];
    for (let i = 0; i < spine_pages(id); ++i) t.push(spine_page_texture(id, i));
    const p = tag ? tag + ' ' : '';
    log2(p + 'animations ' + spine_animation_count(id) + ' [' + skelInfo(id).join(', ') + ']');
    log2(p + 'skins ' + spine_skin_count(id) + ' [' + skinInfo(id).join(', ') + ']');
    log2(p + 'pages ' + spine_pages(id) + ' textures [' + t.join(',') + ']');
}

/* ------------------------------------------------ 皮肤：官方换皮肤 / 运行时合成 */
function applyCompose(c, id) {
    if (!c.compose) return;
    if (HAS.skinCompose) {
        const rc = spine_skin_compose(id, c.compose.join(','));
        const n = HAS.skinAttach ? spine_skin_attachment_count(id, -1) : -1;
        log2('compose 皮肤 [' + c.compose.join(', ') + '] rc=' + rc + ' 附件数=' + n +
             '（官方 Skin::addSkin 逐件并入；附件数 0 = 没合上）');
    } else {
        note('Skin::addSkin');
        log2('UNSUPPORTED 运行时合成皮肤 [' + c.compose.join(', ') + '] —— 引擎暂无 addSkin/compose API');
        /* 兜底：mix-and-match 的 default 皮肤**没有任何附件**（实测 0 顶点），
         * 不兜底就完全看不见人；用数据里现成的整体皮肤顶上。 */
        const rc = spine_set_skin(id, 'full-skins/girl');
        log2('兜底：改用现成皮肤 "full-skins/girl" rc=' + rc);
    }
}

/** 皮肤要在**播放动画之前**装好：顺序反了动画的槽时间轴会按旧皮肤解析（实测不可见）。 */
function applySkin(c, id) {
    if (c.skin) {
        const rc = spine_set_skin(id, c.skin);
        if (rc !== 0) log2('setSkin("' + c.skin + '") rc=' + rc);
    }
    applyCompose(c, id);
    if (c.twoColor) {
        /* 官方这些 demo 都开了两色着色（TintBlack）。本引擎的网格**恒定**走两色着色
         * （没有 dark 通道的素材 dark=(0,0,0,1)，与"关闭"逐字节等价 —— task-6 的 gfx 证据），
         * 所以这个开关是诚实的空操作：输出不变，但语义已满足。 */
        if (HAS.twoColor) { spine_set_two_color(id, 1); log2('two-color tint = on（官方 setTwoColorTint(true)；本引擎网格恒为两色，输出不变）'); }
        else { note('twoColor'); log2('UNSUPPORTED 两色着色 API（本引擎网格恒为两色，输出不受影响）'); }
    }
}

/* ------------------------------------------------ 起始动画 / 官方缩放 */
function startAnim(c, id) {
    const a = c.anim ? spine_animation_index(id, c.anim) : -1;
    if (a >= 0) {
        if (HAS.setAnimation) spine_set_animation(id, 0, c.anim, 1);
        else spine_play(id, c.anim, 1);
        return a;
    }
    if (c.anim) log2('animation "' + c.anim + '" not found; fallback to index 0');
    const n = spine_animation_count(id);
    if (n > 0) { const nm = spine_animation_name(id, 0); spine_play(id, nm, 1); return 0; }
    return -1;
}

/** 给一个骨架实体套用官方配置：缩放 → 皮肤（官方/合成）→ 起始动画 */
function applyDemo(id, c, scaleOverride) {
    node_set_scale(id, scaleOverride === undefined ? c.scale : scaleOverride);
    applySkin(c, id);
    return startAnim(c, id);
}

/* ------------------------------------------------ 官方 setMix / setDefaultMix / 排队动画 */
function applyMix(c, id) {
    if (c.defaultMix) {
        if (HAS.defaultMix) { spine_set_default_mix(id, c.defaultMix); log2('defaultMix=' + c.defaultMix + '（官方 setDefaultMix）'); }
        else { note('setDefaultMix'); log2('UNSUPPORTED setDefaultMix(' + c.defaultMix + ')'); }
    }
    if (!c.mix) return;
    if (HAS.setMix) {
        for (let i = 0; i < c.mix.length; ++i) spine_set_mix(id, c.mix[i][0], c.mix[i][1], c.mix[i][2]);
        log2('mix: ' + c.mix.map(function (m) { return m[0] + '->' + m[1] + ' ' + m[2] + 's'; }).join(', '));
    } else {
        note('setMix');
        log2('UNSUPPORTED setMix(' + c.mix.map(function (m) { return m[0] + '->' + m[1]; }).join(', ') + ') —— 过渡为硬切');
    }
}

function applyQueue(c, id) {
    if (!c.queue) return;
    if (HAS.addAnimation) {
        for (let i = 0; i < c.queue.length; ++i) {
            const q = c.queue[i];
            const delay = (q[2] === 'random0to3') ? (rng() * 3.0) : q[2];
            const rc = spine_add_animation(id, (c.queueTrack || 0), q[0], q[1] ? 1 : 0, delay);
            if (rc !== 0) log2('addAnimation("' + q[0] + '", track=' + (c.queueTrack || 0) + ', delay=' + fmt(delay) + ') rc=' + rc);
        }
        log2('queue: ' + c.queue.map(function (q) { return q[0]; }).join(' → '));
    } else {
        note('addAnimation');
        log2('UNSUPPORTED addAnimation(' + c.queue.map(function (q) { return q[0]; }).join(', ') +
             ') —— 当前只播 ' + c.anim);
    }
}

/* ------------------------------------------------ Separator：槽区间分层（两个实体镜像） */
function setupSeparator(c) {
    mirror = entity_find(c.mirror);
    if (mirror < 0) { log2('缺少镜像实体 "' + c.mirror + '"（scene 里应有第二个 SpineSkeleton）'); return; }
    members.push(mirror);
    applyDemo(mirror, c);                            // 同一个骨架、同一份配置 → 同帧同速
    if (!HAS.slotsRange || !HAS.slotIndex) { note('slotsRange'); log2('UNSUPPORTED 槽区间渲染'); return; }
    const backFrom = spine_slot_index(marker, c.slotsBack[0]);
    const backTo = spine_slot_index(marker, c.slotsBack[1]);
    const frontFrom = spine_slot_index(mirror, c.slotsFront[0]);
    const backOk = spine_set_slots_range(marker, backFrom, backTo);
    const frontOk = spine_set_slots_range(mirror, frontFrom, -1);
    log2('槽区间：back=[' + backFrom + '..' + backTo + '] rc=' + backOk +
         '（' + c.slotsBack[0] + '..' + c.slotsBack[1] + '）  front=[' + frontFrom + '..-1] rc=' + frontOk +
         '（' + c.slotsFront[0] + '..末尾）—— 中间的红条由场景里的 between 实体提供');
}

/* ------------------------------------------------ batching：50 个骨架 */
function setupBatching(c) {
    members = [marker];
    for (let i = 1; i < c.count; ++i) {
        const id = entity_find('batch' + i);
        if (id < 0) break;
        members.push(id);
    }
    /* 官方位置：x ∈ [10%,90%] 内容宽、y ∈ [0,70%] 内容高，逐个体随机（引擎 rng → 可复现） */
    const W = screen_w(), H = screen_h();
    const xmin = W * 0.10, xmax = W * 0.90, ymax = H * 0.70;
    for (let i = 0; i < members.length; ++i) {
        const id = members[i];
        node_set_position(id, xmin + rng() * (xmax - xmin), rng() * ymax);
        applyDemo(id, c, c.scale);
        applyMix(c, id);
        applyQueue(c, id);                            // 每个骨架各自随机延迟 jump
    }
    log2('batching: ' + members.length + ' 个骨架（官方 NUM_SKELETONS=' + c.count + '），位置/延迟用引擎 rng() 复刻');
}

/* ------------------------------------------------ all.scn：总览网格 */
function setupAll() {
    members = [];
    for (let i = 0; i < CELLS.length; ++i) {
        const id = entity_find('cell' + i);
        if (id < 0) { log2('cell' + i + ' 缺失（all.scn 与 CELLS 表不同步？）'); continue; }
        const c = DEMOS[CELLS[i]];
        members.push(id);
        /* 每个格子走**与独立 demo 场景同一条**装配路径（缩放→皮肤→动画）。少任何一步都会"看起来不对"：
         * 实测漏掉皮肤时 Goblins 只剩长矛+盾、MixAndMatch 整格空。 */
        applyDemo(id, c, c.cellScale);
        logSkeleton(CELLS[i].replace('demo.', '') + ':', id);
    }
    log2('all: ' + members.length + '/' + CELLS.length + ' 个骨架已就位（逐个打印了清单）');
}

/* ------------------------------------------------ 动画事件（官方 5 类监听器 + 用户事件）
 * 引擎把 spine 事件翻译成脚本事件（docs/14 事件总线）：名字 spine_start/interrupt/end/complete/
 * dispose/event，处理器值 = 轨下标；用户事件的细节用 pull 面 spine_event_* 取。
 * 日志格式刻意与官方一致：`0 start: walk` / `0 interrupt` / `0 event: name, int, float, str`。 */
function on_spine_start(id, track) {
    const an = HAS.trackAnim ? spine_track_animation(id, track) : '';
    log2(track + ' start: ' + an);
    if (cfg && cfg.jumpedLog && an === 'jump') log2('jumped!（官方 setTrackStartListener(jumpEntry)）');
}
function on_spine_interrupt(id, track) { log2(track + ' interrupt'); }
function on_spine_end(id, track) { log2(track + ' end'); }
function on_spine_complete(id, track) { log2(track + ' complete'); }
function on_spine_dispose(id, track) { log2(track + ' dispose'); }
function on_spine_user_event(id) {
    const n = spine_event_count(id);
    for (let i = 0; i < n; ++i) {
        if (spine_event_type(id, i) !== 5) continue;
        log2(spine_event_track(id, i) + ' event: ' + spine_event_name(id, i) + ', ' + spine_event_int(id, i) +
             ', ' + spine_event_float(id, i) + ', ' + spine_event_string(id, i));
    }
}

function registerSpineEvents(id) {
    if (!HAS.events || !HAS.nodeOn) {
        note('events');
        log2('UNSUPPORTED 动画事件回调（官方 5 类监听器 + 用户事件）');
        return;
    }
    node_on(id, 'spine_start', 'on_spine_start');
    node_on(id, 'spine_interrupt', 'on_spine_interrupt');
    node_on(id, 'spine_end', 'on_spine_end');
    node_on(id, 'spine_complete', 'on_spine_complete');
    node_on(id, 'spine_dispose', 'on_spine_dispose');
    node_on(id, 'spine_event', 'on_spine_user_event');
    log2('已注册 5 类动画事件 + 用户事件（spine_start/interrupt/end/complete/dispose/event）');
}

/* ------------------------------------------------ IK / Physics：指针驱动 */
function pointerToSkeleton(x, y) {
    /* 指针是世界坐标（y 向上）；骨架世界 = 实体世界位置 + 骨架子空间 × 节点缩放 */
    const s = node_scale(marker);
    return [(x - node_position_x(marker)) / s, (y - node_position_y(marker)) / s];
}

function ikFollow(x, y) {
    if (!cfg.bone) return;
    if (!HAS.boneFind || !HAS.boneSetWorld || !HAS.updateWorld) { note('bone API'); return; }
    const b = spine_bone_find(marker, cfg.bone);
    if (b < 0) { note('bone:' + cfg.bone); return; }
    const p = pointerToSkeleton(x, y);
    spine_bone_set_world_position(marker, b, p[0], p[1]);   // 官方：worldToLocal + setX/Y
    spine_update_world_transform(marker);                    // 官方：updateWorldTransform(Physics_Update)
}

function physicsDrag(x, y) {
    if (!cfg.drag || !HAS.physTranslate) return;
    if (lastPointer === null) { lastPointer = [x, y]; return; }   // 官方 firstUpdate 跳过第一帧
    const s = node_scale(marker);
    spine_physics_translate(marker, -(x - lastPointer[0]) / s, -(y - lastPointer[1]) / s);
    lastPointer = [x, y];
}

/* ------------------------------------------------ HUD / 交互 */
function refreshHud() {
    if (hud < 0 || !entity_valid(hud)) return;
    const id = primaryEntity();
    const cur = spine_current_animation(id);
    let extra = '';
    if (cfg && cfg.sequence && HAS.seqIndex && seqSlot >= 0) {
        const f = spine_sequence_index(id, seqSlot);
        if (f !== seqFrame0) { seqFrame0 = f; log2('sequence 帧变了 → ' + f + '（槽 #' + seqSlot + ' "' + spine_slot_name(id, seqSlot) + '"）'); }
        extra = '  seq=' + f;
    }
    ui_set_text(hud, hudBase + '   [' + (names.length ? (idx + 1) : 0) + '/' + names.length + '] ' +
                     (cur || '-') + '  t=' + fmt(spine_time(id)) + 's  x' + fmt(speed) + extra +
                     (paused ? '  PAUSED' : '') +
                     (unsupported.length ? '  UNSUPPORTED:' + unsupported.join(',') : ''));
}

function play(i) {
    if (!names.length) return;
    idx = ((i % names.length) + names.length) % names.length;
    const targets = mirror >= 0 ? [primaryEntity(), mirror] : [primaryEntity()];
    for (let k = 0; k < targets.length; ++k) {
        const id = targets[k];
        if (HAS.setAnimation) spine_set_animation(id, 0, names[idx], 1);
        else spine_play(id, names[idx], 1);
    }
    refreshHud();
    log2('anim=' + names[idx] + ' idx=' + idx + '/' + names.length + ' engine=' + spine_current_animation(primaryEntity()));
}

function setPaused(on) {
    paused = on;
    for (let i = 0; i < members.length; ++i) node_set_number(members[i], 'anim.SpineSkeleton', 'speed', on ? 0 : speed);
    refreshHud();
}

function nextDemo() {
    const i = CHAIN.indexOf(demoKey);
    const nxt = CHAIN[(i + 1) % CHAIN.length];
    const path = DEMO_DIR + 'demo-' + nxt.replace('demo.', '') + '.scn';
    log2('loadScene ' + path);
    director_load_scene(path);
}

function on_pointer_down(target, x, y) {
    if (cfg && cfg.drag) { lastPointer = [x, y]; return; }   // Physics：拖动优先，不参与点击链路
    clicks++;
    if (clicks === 1) {
        note('debugBones');
        log2('UNSUPPORTED 调试骨骼绘制（官方第一击）—— 引擎无骨骼调试绘制');
    } else if (clicks === 2) {
        speed = 0.3;                                   // 官方第二击：setTimeScale(0.3)
        for (let i = 0; i < members.length; ++i) node_set_number(members[i], 'anim.SpineSkeleton', 'speed', 0.3);
        log2('timeScale=0.3（官方第二击）');
    } else {
        nextDemo();                                    // 官方第三击：replaceScene 下一个 demo
    }
    refreshHud();
}

function on_pointer_move(target, x, y) {
    if (cfg && cfg.bone) ikFollow(x, y);
    if (cfg && cfg.drag) physicsDrag(x, y);
}

function on_key_down(target, key) {
    if (key === 8 || key === 1073741906) { play(idx - 1); return; }
    if (key === 9 || key === 1073741905) { play(idx + 1); return; }
    if (key === 2 || key === 32) { setPaused(!paused); log2('paused=' + paused); return; }
    if (key === 78) { nextDemo(); return; }            // N：下一个 demo（键盘等价于第三击）
}

function on_start() {
    demoKey = findDemoKey();
    if (!demoKey) { log('spine[demo] 场景里没有 demo.* 标记实体（见 demos.js 的 DEMOS 表）'); quit(); return; }
    cfg = DEMOS[demoKey];
    marker = entity_find(demoKey);
    members = [marker];
    hud = entity_find('hud');
    clearTo(cfg.bg || [0.1, 0.1, 0.15]);
    label = cfg.title;

    if (demoKey === 'demo.all') {
        setupAll();
        names = skelInfo(members.length ? members[0] : marker);
    } else if (demoKey === 'demo.batching') {
        logSkeleton('', marker);                      // 50 个骨架共享同一份骨架数据，打印一次即可
        setupBatching(cfg);
        names = skelInfo(members[0]);
    } else {
        logSkeleton('', marker);
        idx = applyDemo(marker, cfg);
        if (idx < 0) idx = 0;
        applyMix(cfg, marker);
        applyQueue(cfg, marker);
        if (demoKey === 'demo.separator') setupSeparator(cfg);
        if (cfg.track1) {
            if (HAS.setAnimation) {
                const rc = spine_set_animation(marker, 1, cfg.track1, 1);
                log2('track1="' + cfg.track1 + '" rc=' + rc + '（官方 setAnimation(1,"aim",true)）');
            } else { note('multi-track'); log2('UNSUPPORTED 多轨（track1=' + cfg.track1 + '）'); }
        }
        if (cfg.events) registerSpineEvents(marker);
        if (cfg.sequence && HAS.slotIndex) {
            /* Sequence 挂在哪个槽上要**扫一遍槽表**才知道（spine 允许任意槽的附件是 Sequence 帧）；
             * 实测 dragon 的 head 槽没有 sequence（帧数=0），所以这里按事实找，而不是猜槽名。 */
            if (typeof spine_sequence_frames === 'function' && typeof spine_slot_count === 'function') {
                const ns = spine_slot_count(marker);
                for (let s = 0; s < ns; ++s) {
                    const nf = spine_sequence_frames(marker, s);
                    if (nf > 1) {
                        const path = (typeof spine_sequence_path === 'function') ? spine_sequence_path(marker, s) : '';
                        log2('发现 sequence 槽 #' + s + ' "' + spine_slot_name(marker, s) + '"：帧数=' + nf +
                             ' 起始帧=' + spine_sequence_index(marker, s) + ' 路径=' + path);
                        if (seqSlot < 0) { seqSlot = s; seqFrame0 = spine_sequence_index(marker, s); }
                    }
                }
            }
            if (seqSlot < 0) {
                log2('本骨架的槽上没有多帧 sequence（或引擎未暴露）—— 该 demo 的"换帧"行为无法自证');
                note('sequence');
            } else {
                log2('sequence 监视槽 #' + seqSlot + ' "' + spine_slot_name(marker, seqSlot) + '" 起始帧=' + seqFrame0);
            }
        }
    }

    /* HUD 基础文案由场景描述（本 demo 演示什么），脚本只追加实时状态 */
    if (hud >= 0 && entity_valid(hud)) hudBase = ui_get_text(hud);
    if (cfg.boneYDown) log2('官方入口用 y 向下坐标系；引擎世界坐标 y 向上 → 布局已换算');
    if (cfg.bone) log2('指针移动 → crosshair 骨骼 → IK 跟随（' +
                       (HAS.boneFind && HAS.boneSetWorld ? '已接线' : 'UNSUPPORTED') + '）');
    if (cfg.drag) log2('拖动 → physicsTranslate（' + (HAS.physTranslate ? '已接线' : 'UNSUPPORTED') + '）');

    refreshHud();
    log2('ready ' + screen_w() + 'x' + screen_h() + ' scale=' + cfg.scale + ' bg=' +
         cfg.bg.map(function (v) { return fmt(v); }).join(','));
}

function update(dt) {
    if (director_frame() % 20 === 0) refreshHud();
    imguiPanel();
}

/* ============================================================================
 * ImGui 调试面板（引擎侧即时模式 UI）
 * ----------------------------------------------------------------------------
 * 定位：**开发工具**，不是游戏 UI。游戏内 HUD 是场景里的 ui.* 控件（进快照、可回放）；
 * 这块面板是视图层 —— 不进快照 / 不进校验和，关掉不影响任何模拟结果。
 *
 * 引擎保证：面板在所有引擎图层（含 ui pass 的 layer=1）**之后**、present **之前**提交，
 * 所以永远画在最上层，且**截图里也有**（--shot 出来的图能看到面板）。
 *
 * 开关：引擎侧 `MAQUE_IMGUI=1`；未启用时 `imgui_enabled()` 为 0 → 下面全是空操作。
 * ========================================================================== */
const IMGUI_CELLS = 12;
let imguiPaused = false;
let imguiShowDemo = false;
let imguiCellIds = null;      // 惰性缓存：cell0..cell11 里真的有动画的那些

/** 收集总览场景里"真的有骨架动画"的格子（按 cell 序号稳定序） */
function imguiCells() {
    if (imguiCellIds) return imguiCellIds;
    const list = [];
    for (let i = 0; i < IMGUI_CELLS; ++i) {
        const id = entity_find('cell' + i);
        if (id >= 0 && spine_animation_count(id) > 0) list.push(id);
    }
    imguiCellIds = list;
    return list;
}

/** 一个格子的控制块：动画下拉 + 播放头 + 缩放 */
function imguiCellBlock(id, index) {
    const nm = entity_name(id);
    imgui_push_id(nm);
    imgui_text_colored(index + 1 + '. ' + nm + '  (' + spine_animation_count(id) + ' anims)',
                       0.85, 0.88, 0.95, 1.0);

    const n = spine_animation_count(id);
    const cur = spine_current_animation(id);
    let curIdx = spine_animation_index(id, cur);
    if (curIdx < 0) curIdx = 0;

    // 动画名清单（逗号分隔交给引擎侧 split）
    let names = '';
    for (let i = 0; i < n; ++i) names += (i ? ',' : '') + spine_animation_name(id, i);
    const pick = imgui_combo('anim##c', curIdx, names);
    if ((pick | 0) !== curIdx) spine_play(id, spine_animation_name(id, pick | 0), 1);

    // 播放头：拖动即定格（与确定性回放用的是同一个接口）
    const t = spine_time(id);
    const newT = imgui_slider_float('t##c', t, 0.0, 2.0);
    if (Math.abs(newT - t) > 1e-4) spine_set_time(id, newT);

    // 缩放（影响渲染、不影响模拟）
    const sc = node_scale(id);
    const newSc = imgui_drag_float('scale##c', sc, 0.01);
    if (Math.abs(newSc - sc) > 1e-4) node_set_scale(id, newSc);

    imgui_pop_id();
}

function imguiPanel() {
    if (!imgui_enabled()) return;

    /* 右上角：不遮住网格主体。headless（--shot）时显示空间 = 目标空间，1:1。 */
    imgui_set_pos(screen_w() - 316.0, 8.0);
    imgui_set_size(308.0, 470.0);
    if (!imgui_begin('Spine demos (ImGui)')) { imgui_end(); return; }

    imgui_text('12 skeletons / official 4.2 assets');
    imgui_text_disabled('drawn AFTER the engine UI pass');
    imgui_separator();

    /* ---- 全局控制 ---- */
    const paused = director_paused() != 0;
    if (imgui_checkbox('Pause all', paused) !== (paused ? 1 : 0)) {
        if (paused) director_resume(); else director_pause();
    }
    imgui_same_line();
    if (imgui_small_button('Reset')) {
        for (const id of imguiCells()) {
            node_set_scale(id, 1.0);
            spine_set_time(id, 0.0);
        }
    }
    imgui_same_line();
    if (imgui_small_button('ImGui demo')) imguiShowDemo = !imguiShowDemo;
    if (imguiShowDemo) imgui_show_demo();

    imgui_separator();
    imgui_text('Per-skeleton (' + imguiCells().length + ')');

    imgui_begin_child('cells', 292.0, 330.0, 1);
    const cells = imguiCells();
    for (let i = 0; i < cells.length; ++i) imguiCellBlock(cells[i], i);
    imgui_end_child();

    imgui_separator();
    imgui_text_disabled('frame ' + director_frame() + ' / cmds ' + imgui_last_cmd_count());

    imgui_end();
}

