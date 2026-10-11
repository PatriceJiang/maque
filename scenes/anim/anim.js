// anim.js —— 只负责退出条件；动画与粒子由引擎按 role 推进（docs/15 §2/§4）。
// 控制面驱动（AI/CI）时不自带帧数退出，由调用方决定步进多少帧。
(function () {
  'use strict';
  globalThis.on_start = function () { log('anim.js ready'); };
  globalThis.update = function () {
    // 模拟时间以引擎为准（Runtime.frame / MAQUE_MAX_FRAMES）；脚本不自持帧计数器，
    // 这样回滚（World.restore）后时间位置自动一致（docs/10 D3）。
    if (input_key(1) > 0) quit();          // Esc = 1
  };
})();
