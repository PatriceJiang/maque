import createMaque from './maque_web.mjs';

function cString(mod, text) {
  const bytes = new TextEncoder().encode(text);
  const ptr = mod._malloc(bytes.length + 1);
  mod.HEAPU8.set(bytes, ptr);
  mod.HEAPU8[ptr + bytes.length] = 0;
  return ptr;
}

export async function runMaqueP0({ onFrame, onError } = {}) {
  const mod = await createMaque({
    locateFile: (file) => new URL(file, import.meta.url).href,
  });
  const scenePath = cString(mod, '/scenes/anim/anim.scn');
  const scene = mod._mq_host_scene_create(scenePath, 0);
  mod._free(scenePath);
  if (!scene) throw new Error('mq_host_scene_create failed');

  const fixed = 1 / 60;
  const maxSteps = 8;
  let accumulator = 0;
  let previous = performance.now() / 1000;

  function cleanup() {
    if (scene) mod._mq_host_scene_destroy(scene);
  }

  function frame(nowMs) {
    try {
      const now = nowMs / 1000;
      accumulator += Math.min(Math.max(now - previous, 0), 0.25);
      previous = now;
      let steps = 0;
      while (accumulator >= fixed && steps < maxSteps) {
        if (mod._mq_host_step(scene) !== 0) throw new Error('mq_host_step failed');
        accumulator -= fixed;
        ++steps;
      }
      if (steps === maxSteps) accumulator = 0;
      onFrame?.({
        frame: mod._mq_host_frame(scene),
        checksum: BigInt.asUintN(64, mod._mq_host_checksum(scene)),
      });
      if (!mod._mq_host_should_quit(scene)) requestAnimationFrame(frame);
      else cleanup();
    } catch (error) {
      cleanup();
      onError?.(error);
    }
  }

  requestAnimationFrame(frame);
  return { mod, scene };
}
