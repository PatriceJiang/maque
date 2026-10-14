// prefab.js —— 静态展示：prefab 已由引擎实例化（docs/12）；脚本只做就绪日志与退出键。
'use strict';

function on_start() {
  log('prefab.js ready: entities=' + entity_count());
  if (input_key(1) > 0) quit();
}
function update() { /* 静态展示：prefab 已由引擎实例化 */ }
