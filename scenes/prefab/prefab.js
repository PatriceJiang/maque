(function () {
  'use strict';
  globalThis.on_start = function () {
    log('prefab.js ready: entities=' + entity_count());
    if (input_key(1) > 0) quit();
  };
  globalThis.update = function () { /* 静态展示：prefab 已由引擎实例化 */ };
})();
