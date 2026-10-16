// storage.js —— 键值存储演示（docs/25）：计数**跨进程持久**（默认 file://<场景目录>/.maque），
// 同一份逻辑也有 storage.lua（两个后端共用 DSL 生成的同一套绑定，docs/14/21）。
//   MAQUE_UID / MAQUE_SESSION 决定键空间 <uid>/<session>/（默认 default/main）；
//   MAQUE_STORAGE 覆盖模式（如 mem:// 或 https://… 或 ws://…）。
'use strict';

const KEY = 'demo/count';

function refresh_texts(n) {
    const info = entity_find('Info');
    if (info >= 0) {
        node_set_string(info, 'scn.Label', 'text',
                        'storage ' + storage.info() + '  count=' + n);
    }
    const keys = entity_find('Keys');
    if (keys >= 0) node_set_string(keys, 'scn.Label', 'text', 'keys: ' + storage.keys());
}

function on_start() {
    const prev = storage.get(KEY);
    const n = (prev === '' ? 0 : (parseInt(prev, 10) || 0)) + 1;
    storage.set(KEY, String(n));
    storage.set('demo/last_seen', String(n));                 // 第二个键：列表里能看到嵌套键
    refresh_texts(n);
    log('storage_demo count=' + n + ' keys=' + storage.keys()
        + ' has=' + storage.exists(KEY) + ' mode=' + storage.info());
    if (input_key(1) > 0) quit();
}

function update() {}
