// audio_e2e.js —— 脚本音频 API 端到端（docs/30）：
//   ① audio_play 从 VFS 加载 wav（场景目录 = VFS 根，`beep.wav` 就在旁边）
//   ② audio_state / audio_playhead 由**模拟 dt** 推进（CI 用 MAQUE_AUDIO=null，不依赖声卡）
//   ③ 非 loop 播完 → 自动 Stopped；audio_stop 后状态为 0
let voice = 0;
let frame = 0;

function on_start() {
    log('audio_e2e: enabled=' + audio_enabled());
    voice = audio_play('beep.wav', 0.5, 0, 'sfx');
    log('audio_e2e: voice=' + (voice > 0 ? 1 : 0));
    log('audio_e2e: state0=' + audio_state(voice));
    log('audio_e2e: head0=' + audio_playhead(voice));
}

function update(dt) {
    ++frame;
    if (frame === 3) {
        log('audio_e2e: head3=' + audio_playhead(voice));
        const id = entity_find('speaker');
        log('audio_e2e: comp_state3=' + node_get_number(id, 'AudioSource', 'state'));
        log('audio_e2e: comp_head3=' + node_get_number(id, 'AudioSource', 'playhead'));
        const id2 = entity_find('speaker_ogg');
        log('audio_e2e: ogg_state3=' + node_get_number(id2, 'AudioSource', 'state'));
        log('audio_e2e: ogg_head3=' + node_get_number(id2, 'AudioSource', 'playhead'));
    }
    if (frame === 30) {
        log('audio_e2e: state30=' + audio_state(voice));
        log('audio_e2e: head30=' + audio_playhead(voice));
        const id = entity_find('speaker');
        log('audio_e2e: comp_state30=' + node_get_number(id, 'AudioSource', 'state'));
        log('audio_e2e: comp_head30=' + node_get_number(id, 'AudioSource', 'playhead'));
        const id2 = entity_find('speaker_ogg');
        log('audio_e2e: ogg_state30=' + node_get_number(id2, 'AudioSource', 'state'));
        log('audio_e2e: ogg_head30=' + node_get_number(id2, 'AudioSource', 'playhead'));
    }
    if (frame === 40) {
        audio_stop(voice);
        log('audio_e2e: state_after_stop=' + audio_state(voice));
        log('audio_e2e: stopped');
        quit();
    }
}
