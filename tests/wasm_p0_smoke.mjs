import { fileURLToPath, pathToFileURL } from 'node:url';

const [modulePath, scenePath = '/scenes/anim/anim.scn', frameText = '60'] = process.argv.slice(2);
if (!modulePath) {
  console.error('usage: node tests/wasm_p0_smoke.mjs <maque_web.mjs> [scene] [frames]');
  process.exit(2);
}

const moduleUrl = pathToFileURL(modulePath);
const { default: createMaque } = await import(moduleUrl.href);
const mod = await createMaque({
  locateFile: (file) => fileURLToPath(new URL(file, moduleUrl)),
});
const frames = Number.parseInt(frameText, 10);

function cString(text) {
  const bytes = new TextEncoder().encode(text);
  const ptr = mod._malloc(bytes.length + 1);
  mod.HEAPU8.set(bytes, ptr);
  mod.HEAPU8[ptr + bytes.length] = 0;
  return ptr;
}

const scenePtr = cString(scenePath);
const a = mod._mq_host_scene_create(scenePtr, 0);
const b = mod._mq_host_scene_create(scenePtr, 0);
mod._free(scenePtr);
if (!a || !b) throw new Error(`scene create failed: a=${a} b=${b}`);

const initialA = BigInt.asUintN(64, mod._mq_host_checksum(a));
const initialB = BigInt.asUintN(64, mod._mq_host_checksum(b));

for (let frame = 0; frame < frames; ++frame) {
  if (mod._mq_host_step(a) !== 0 || mod._mq_host_step(b) !== 0) {
    throw new Error(`step failed at frame ${frame}`);
  }
}

const frameA = mod._mq_host_frame(a);
const frameB = mod._mq_host_frame(b);
const sumA = BigInt.asUintN(64, mod._mq_host_checksum(a));
const sumB = BigInt.asUintN(64, mod._mq_host_checksum(b));
if (frameA !== BigInt(frames) || frameB !== BigInt(frames)) {
  throw new Error(`frame mismatch: ${frameA}/${frameB}, expected ${frames}`);
}
if (sumA !== sumB || sumA === 0n || sumA === initialA || sumB === initialB) {
  throw new Error(`checksum mismatch or no world progress: ${sumA}/${sumB}`);
}

mod._mq_host_scene_destroy(b);
mod._mq_host_scene_destroy(a);
console.log(`wasm_p0_smoke: frames=${frameA} checksum=${sumA} identical=yes`);
