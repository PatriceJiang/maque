// audio_e2e.js 鈥斺€?鑴氭湰闊抽 API 绔埌绔紙docs/30锛夛細
//   鈶?audio_play 浠?VFS 鍔犺浇 wav锛堝満鏅洰褰?= VFS 鏍癸紝`beep.wav` 灏卞湪鏃佽竟锛?
//   鈶?audio_state / audio_playhead 鐢?*妯℃嫙 dt** 鎺ㄨ繘锛圕I 鐢?MAQUE_AUDIO=null锛屼笉渚濊禆澹板崱锛?
//   鈶?闈?loop 鎾畬 鈫?鑷姩 Stopped锛沘udio_stop 鍚庣姸鎬佷负 0
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
        log('audio_e2e: comp_state3=' + node_get_number(id, 'audio.AudioSource', 'state'));
        log('audio_e2e: comp_head3=' + node_get_number(id, 'audio.AudioSource', 'playhead'));
        const id2 = entity_find('speaker_ogg');
        log('audio_e2e: ogg_state3=' + node_get_number(id2, 'audio.AudioSource', 'state'));
        log('audio_e2e: ogg_head3=' + node_get_number(id2, 'audio.AudioSource', 'playhead'));
    }
    if (frame === 30) {
        log('audio_e2e: state30=' + audio_state(voice));
        log('audio_e2e: head30=' + audio_playhead(voice));
        const id = entity_find('speaker');
        log('audio_e2e: comp_state30=' + node_get_number(id, 'audio.AudioSource', 'state'));
        log('audio_e2e: comp_head30=' + node_get_number(id, 'audio.AudioSource', 'playhead'));
        const id2 = entity_find('speaker_ogg');
        log('audio_e2e: ogg_state30=' + node_get_number(id2, 'audio.AudioSource', 'state'));
        log('audio_e2e: ogg_head30=' + node_get_number(id2, 'audio.AudioSource', 'playhead'));
    }
    if (frame === 40) {
        audio_stop(voice);
        log('audio_e2e: state_after_stop=' + audio_state(voice));
        log('audio_e2e: stopped');
        quit();
    }
}
