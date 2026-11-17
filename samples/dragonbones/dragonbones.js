/* DragonBones 2D 演示 —— 玩法/交互全在这个脚本里（引擎不认识"动画清单""换皮肤"这些概念）。
 *
 * 分工：
 *   · 引擎（runtime）：按 anim.DragonBonesSkeleton 组件装配骨架（VFS 读 *_ske.json/*_tex.json →
 *     image 解码 → gfx 上传页纹理）、每帧 step + 出绘制数据、按批次提交网格图元；并提供 dragonbones_* 脚本 API。
 *   · 本脚本：列出动画、↑/↓ 切换、空格暂停、自适应缩放、HUD 文案。改这里不需要重编译引擎。
 *
 * 键码：引擎在平台边界统一归一化到规范键空间（空格=2、UP=8、DOWN=9；字母=大写 ASCII）。
 * 下面同时收 SDL 原始码（32 / 0x40000052 等）只是防御性冗余。
 *
 * 缩放：骨架单位 = 像素（mecha_1004d 站立约 600 单位高），放到 480x320 的设计分辨率里要缩。
 * 缩放走**节点 scale**（node_set_scale）→ 引擎据此设置实例变换（见 runtime dragonbones_step）。
 *
 * y 轴：DragonBones 是 flash 系 **y 向下**，与引擎 2D 渲染目标一致 —— 引擎不翻 y（spine 才翻）。 */

const KEY_UP = [8, 1073741906];
const KEY_DOWN = [9, 1073741905];
const KEY_SPACE = [2, 32];

let skel = -1, hud = -1;
let names = [];
let idx = 0;
let paused = false;
let label = 'dragonbones';

function hasKey(list, key) {
    for (let i = 0; i < list.length; ++i) if (list[i] === key) return true;
    return false;
}

function refreshHud() {
    if (hud < 0 || !entity_valid(hud)) return;
    ui_set_text(hud, label + ' ' + (names.length ? (idx + 1) : 0) + '/' + names.length +
                     ' ' + (names.length ? names[idx] : '-') + (paused ? '  PAUSED' : ''));
}

/** 播放第 i 个动画（环绕）。**引擎侧组件字段是唯一真相**：dragonbones_play 会同步写回组件。 */
function play(i) {
    if (!names.length) return;
    idx = ((i % names.length) + names.length) % names.length;
    const rc = dragonbones_play(skel, names[idx], 1);
    if (rc !== 0) { log('dragonbones: play failed rc=' + rc + ' name=' + names[idx]); return; }
    refreshHud();
    /* engine= 是**读回引擎实例的当前动画**（不是脚本自己记的），e2e 据此断言切换真的生效 */
    log('dragonbones: anim=' + names[idx] + ' idx=' + idx + '/' + names.length +
        ' engine=' + dragonbones_current_animation(skel) + ' t=' + dragonbones_time(skel));
}

function setPaused(on) {
    paused = on;
    node_set_number(skel, 'anim.DragonBonesSkeleton', 'speed', on ? 0 : 1);   // 速度 0 = 定格
    refreshHud();
}

function on_key_down(target, key) {
    if (hasKey(KEY_UP, key)) { play(idx - 1); return; }        // ↑ 上一个
    if (hasKey(KEY_DOWN, key)) { play(idx + 1); return; }      // ↓ 下一个
    if (hasKey(KEY_SPACE, key)) { setPaused(!paused); log('dragonbones: paused=' + paused); return; }
}

function on_start() {
    skel = entity_find('mecha');
    if (skel < 0) { log('dragonbones: no DragonBonesSkeleton entity in scene'); quit(); return; }
    label = entity_name(skel);
    hud = entity_find('hud');

    const n = dragonbones_animation_count(skel);
    for (let i = 0; i < n; ++i) names.push(dragonbones_animation_name(skel, i));
    log('dragonbones: animations ' + n + ' [' + names.join(', ') + ']');
    const pages = dragonbones_pages(skel);
    const texs = [];
    const sizes = [];
    for (let i = 0; i < pages; ++i) {
        texs.push(dragonbones_page_texture(skel, i));
        sizes.push(dragonbones_page_width(skel, i) + 'x' + dragonbones_page_height(skel, i));
    }
    log('dragonbones: skeleton armature=' + dragonbones_armature(skel) +
        ' pages=' + pages + ' page_textures [' + texs.join(',') + ']' +
        ' sizes [' + sizes.join(',') + '] skins=' + dragonbones_skin_count(skel) +
        ' bones=' + dragonbones_bone_count(skel) + ' slots=' + dragonbones_slot_count(skel));

    /* 起始动画：MAQUE_DRAGONBONES_ANIM=<名字> 可指定（截图/回归用），否则 idle，再退到第 0 个 */
    const want = env('MAQUE_DRAGONBONES_ANIM');
    let start = want ? dragonbones_animation_index(skel, want) : -1;
    if (start < 0) start = dragonbones_animation_index(skel, 'idle');
    if (start < 0) start = 0;
    play(start);

    /* 定格到指定时间（截图确定性：不依赖"跑多少帧"） */
    const t = Number(env('MAQUE_DRAGONBONES_TIME'));
    if (isFinite(t) && t > 0) { dragonbones_set_time(skel, t); log('dragonbones: set_time=' + t); }

    refreshHud();
    log('dragonbones: ready ' + screen_w() + 'x' + screen_h() + ' entity=' + label);
}

/* 自适应缩放：包围盒要等**第一次 step** 之后才有（脚本 update 在 step 之前）→
 * 这里等到 bounds 可用时算一次，把骨架缩进设计分辨率（480x320 里给地面/HUD 留边）。
 * 缩放走节点 scale（引擎按累计节点缩放设置实例变换，见 runtime dragonbones_step）。 */
let fitted = false;
function fitOnce() {
    if (fitted) return;
    const h = dragonbones_bounds(skel, 3);
    const w = dragonbones_bounds(skel, 2);
    if (h <= 1 || w <= 1) return;                    // 还没出绘制数据
    fitted = true;
    const fit = Number(env('MAQUE_DRAGONBONES_FIT'));
    let scale = isFinite(fit) && fit > 0 ? fit : Math.min(210.0 / h, 430.0 / w);
    node_set_scale(skel, scale);
    /* 素材的绑定姿态可能带整身旋转（各 demo 素材不同）→ 场景/脚本可用节点角度摆正 */
    const ang = Number(env('MAQUE_DRAGONBONES_ANGLE'));
    if (isFinite(ang)) node_set_angle(skel, ang);
    log('dragonbones: bounds=' + w.toFixed(1) + 'x' + h.toFixed(1) + ' scale=' + scale.toFixed(3));
}

function update(dt) {
    fitOnce();
    /* HUD 每帧刷新"播放头时间"（自省：能看见动画真的在推进） */
    if (!paused && names.length && director_frame() % 30 === 0) refreshHud();
}

/* 自省/控制面用：当前动画名（Scene.emit / 控制面可读脚本全局） */
function get_anim() { return names.length ? names[idx] : ''; }
function get_anim_index() { return idx; }
function get_anim_count() { return names.length; }
