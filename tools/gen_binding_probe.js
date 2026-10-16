#!/usr/bin/env node
'use strict'

/**
 * 脚本绑定探针生成器（docs/21 §6、docs/14）：从**机器可读单一真相** generated/maque_schema.json
 * 生成两份等价探针脚本（JS / Lua），逐条检查"这个 VM 里该 api 是否可调用"，并打印
 *     bindings_probe missing=<缺失数> checked=<检查数> [缺少的名字…]
 *
 * 用途：`ctest -R bindings_e2e` 用它证明 **JS 与 Lua 的接口绑定面完全一致**（同一份 DSL 声明 →
 * 同一套生成绑定 → 两个后端都实现全部注册方式）。新加 api 若只在某一侧可用/被生成器静默丢弃，
 * 这里立即失败（例：`(string)->string` 形态此前被丢弃过，见 parser.js 的 retStr 分支）。
 *
 * 用法：node tools/gen_binding_probe.js --schema-json <maque_schema.json> --out <dir>
 */

const fs = require('fs')
const path = require('path')

function parseArgs(argv) {
  const args = { schemaJson: null, out: null }
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i]
    if (a === '--schema-json') args.schemaJson = argv[++i]
    else if (a === '--out') args.out = argv[++i]
    else { console.error(`gen_binding_probe: 未知参数 "${a}"`); process.exit(2) }
  }
  if (!args.schemaJson || !args.out) {
    console.error('usage: node tools/gen_binding_probe.js --schema-json <json> --out <dir>')
    process.exit(2)
  }
  return args
}

function main() {
  const args = parseArgs(process.argv.slice(2))
  const doc = JSON.parse(fs.readFileSync(args.schemaJson, 'utf8'))
  // 可调用名：global 命名空间无前缀（host.log → log），其余 <ns>.<fn>
  const names = doc.apis.map((a) => (a.global ? a.fn : `${a.namespace}.${a.fn}`)).sort()
  const unique = [...new Set(names)]
  if (unique.length !== names.length) {
    console.error('gen_binding_probe: 发现重复的可调用名（绑定会互相覆盖）')
    process.exit(1)
  }
  const list = unique.map((n) => `'${n}'`).join(', ')

  const js = `// 生成物：DO NOT EDIT —— tools/gen_binding_probe.js 从 maque_schema.json（单一真相）生成。
// 检查本 VM 里每个引擎 api 是否可调用：missing 必须为 0（JS/Lua 绑定面一致）。
'use strict';

var API_NAMES = [${list}];

function api_ok(path) {
    var parts = path.split('.');
    var o = globalThis;
    for (var i = 0; i < parts.length; i++) {
        if (o == null) return false;
        o = o[parts[i]];
    }
    return typeof o === 'function';
}

function on_start() {
    var missing = [];
    for (var i = 0; i < API_NAMES.length; i++) {
        if (!api_ok(API_NAMES[i])) missing.push(API_NAMES[i]);
    }
    log('bindings_probe missing=' + missing.length + ' checked=' + API_NAMES.length
        + (missing.length ? ' [' + missing.join(',') + ']' : ''));
    quit();
}

function update() {}
`

  const lua = `-- 生成物：DO NOT EDIT —— tools/gen_binding_probe.js 从 maque_schema.json（单一真相）生成。
-- 检查本 VM 里每个引擎 api 是否可调用：missing 必须为 0（JS/Lua 绑定面一致）。
local API_NAMES = { ${unique.map((n) => `'${n}'`).join(', ')} }

local function api_ok(path)
  local o = _ENV or _G
  for seg in string.gmatch(path, '[^%.]+') do
    if type(o) ~= 'table' then return false end
    o = o[seg]
  end
  return type(o) == 'function'
end

function on_start()
  local missing = {}
  for i = 1, #API_NAMES do
    if not api_ok(API_NAMES[i]) then missing[#missing + 1] = API_NAMES[i] end
  end
  log('bindings_probe missing=' .. #missing .. ' checked=' .. #API_NAMES
      .. (#missing > 0 and (' [' .. table.concat(missing, ',') .. ']') or ''))
  quit()
end

function update() end
`

  fs.mkdirSync(args.out, { recursive: true })
  fs.writeFileSync(path.join(args.out, 'bindings_probe.js'), js)
  fs.writeFileSync(path.join(args.out, 'bindings_probe.lua'), lua)
  console.log(`gen_binding_probe: ${unique.length} 个 api → ${path.relative(process.cwd(), args.out) || args.out}`)
}

try {
  main()
} catch (err) {
  console.error(`gen_binding_probe: 失败：${err.message}`)
  process.exit(1)
}
