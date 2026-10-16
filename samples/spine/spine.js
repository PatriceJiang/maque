/* Spine 2D 演示 —— 玩法/交互全在这个脚本里（引擎不认识"动画清单""切换"这些概念）。
 *
 * 分工：
 *   · 引擎（runtime）：按 anim.SpineSkeleton 组件装配骨架（VFS 读 .skel/.atlas → image 解码 →
 *     gfx 上传页纹理）、每帧 step + 出绘制数据、按批次提交网格图元；并提供 spine_* 脚本 API。
 *   · 本脚本：列出动画、↑/↓ 切换、空格暂停、HUD 文案。改这里不需要重编译引擎。
 *
 * 键码：引擎在平台边界（key_from_sdl）与输入模拟器边界（mq_sim_normalize_key）**统一归一化**到
 * 规范键空间（MQ_KEY_*：空格=2、回车=3、Tab=4、退格=5、左右=6/7、上下=8/9；字母=大写 ASCII）。
 * 脚本因此只需认规范码；下面同时收 SDL 原始码（32 / 0x40000052 等）只是防御性冗余 ——
 * 万一有人绕过引擎边界直接注入原始码，也不会静默按不动。
 * 注意：.sim.json 里的方向键要写 SDL 码 0x40000052/0x40000051 —— 规范值 UP=8/DOWN=9 与
 * SDL 的退格(8)/Tab(9) 同值，注入路径按 SDL 解释（引擎既定语义）。
 *
 * 缩放：骨架单位 = 像素（spineboy 站立约 663 单位高），放到 320x240 的设计分辨率里要缩。
 * 缩放走 **节拍 scale**（node_set_scale）→ 引擎按累计节点缩放设置实例变换（见 runtime spine_step），
 * 所以"多大"是场景/脚本的事，不是引擎硬编码的。 */

const FIT = { spineboy: 0.30, dragon: 0.26 };     // 每个骨架的适配缩放（骨架单位 → 设计像素）
const KEY_UP = [8, 1073741906];
const KEY_DOWN = [9, 1073741905];
const KEY_SPACE = [2, 32];

let skel = -1, hud = -1;
let names = [];
let idx = 0;
let paused = false;
let label = 'spine';

function hasKey(list, key) {
    for (let i = 0; i < list.length; ++i) if (list[i] === key) return true;
    return false;
}

function refreshHud() {
    if (hud < 0 || !entity_valid(hud)) return;
    /* 紧凑格式：320px 设计宽度下 "spineboy anim 4/11: idle-turn" 会被裁掉右边（实测），
     * 所以省掉 "anim"/冒号 —— 留出空间给 PAUSED 标记。 */
    ui_set_text(hud, label + ' ' + (names.length ? (idx + 1) : 0) + '/' + names.length +
                     ' ' + (names.length ? names[idx] : '-') + (paused ? '  PAUSED' : ''));
}

/** 播放第 i 个动画（环绕）。**引擎侧组件字段是唯一真相**：spine_play 会同步写回组件。 */
function play(i) {
    if (!names.length) return;
    idx = ((i % names.length) + names.length) % names.length;
    const rc = spine_play(skel, names[idx], 1);
    if (rc !== 0) { log('spine: play failed rc=' + rc + ' name=' + names[idx]); return; }
    refreshHud();
    /* engine= 是**读回引擎实例的当前动画**（不是脚本自己记的），e2e 据此断言切换真的生效 */
    log('spine: anim=' + names[idx] + ' idx=' + idx + '/' + names.length +
        ' engine=' + spine_current_animation(skel) + ' t=' + spine_time(skel));
}

function setPaused(on) {
    paused = on;
    node_set_number(skel, 'anim.SpineSkeleton', 'speed', on ? 0 : 1);   // 速度 0 = 定格
    refreshHud();
}

function on_key_down(target, key) {
    if (hasKey(KEY_UP, key)) { play(idx - 1); return; }        // ↑ 上一个
    if (hasKey(KEY_DOWN, key)) { play(idx + 1); return; }      // ↓ 下一个
    if (hasKey(KEY_SPACE, key)) { setPaused(!paused); log('spine: paused=' + paused); return; }
}

function on_start() {
    /* 找骨架实体：spineboy（单页图集）优先，其次 dragon（多页图集） */
    skel = entity_find('spineboy');
    if (skel < 0) skel = entity_find('dragon');
    if (skel < 0) { log('spine: no SpineSkeleton entity in scene'); quit(); return; }
    label = entity_name(skel);
    hud = entity_find('hud');

    const n = spine_animation_count(skel);
    for (let i = 0; i < n; ++i) names.push(spine_animation_name(skel, i));
    log('spine: animations ' + n + ' [' + names.join(', ') + ']');
    const pages = spine_pages(skel);
    const texs = [];
    for (let i = 0; i < pages; ++i) texs.push(spine_page_texture(skel, i));
    log('spine: skeleton pages=' + pages + ' page_textures [' + texs.join(',') + ']' +
        ' skins=' + spine_skin_count(skel) + ' skin0=' + spine_skin_name(skel, 0));

    /* 适配缩放（骨架单位 → 设计像素） */
    const fit = FIT[label];
    if (fit) node_set_scale(skel, fit);

    /* 起始动画：MAQUE_SPINE_ANIM=<名字> 可指定（截图/回归用），否则 idle，再退到第 0 个 */
    const want = env('MAQUE_SPINE_ANIM');
    let start = want ? spine_animation_index(skel, want) : -1;
    if (start < 0) start = spine_animation_index(skel, 'idle');
    if (start < 0) start = 0;
    play(start);

    /* 定格到指定时间（截图确定性：不依赖"跑多少帧"） */
    const t = Number(env('MAQUE_SPINE_TIME'));
    if (isFinite(t) && t > 0) { spine_set_time(skel, t); log('spine: set_time=' + t); }

    refreshHud();
    log('spine: ready ' + screen_w() + 'x' + screen_h() + ' entity=' + label);
}

function update(dt) {
    /* HUD 每帧刷新"播放头时间"（自省：能看见动画真的在推进） */
    if (!paused && names.length && director_frame() % 30 === 0) refreshHud();
}

/* 自省/控制面用：当前动画名（Scene.emit / 控制面可读脚本全局） */
function get_anim() { return names.length ? names[idx] : ''; }
function get_anim_index() { return idx; }
function get_anim_count() { return names.length; }
