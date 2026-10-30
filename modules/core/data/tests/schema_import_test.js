'use strict'

/**
 * schema 目录 / import 机制单测（SCHEMA.md §1、parser.js 的 loadSchema/parseSchemaTree/checkSchemaImports）。
 *
 * 覆盖：合并顺序（依赖在前）、跨文件扩展 namespace、单文件（旧式）兼容、
 * 以及 import 的全部硬校验（不存在 / 越界 / 成环 / 同一文件重复 / 结构依赖未 import / 死文件）
 * 与未使用 import 告警。
 *
 * CLI: node tests/schema_import_test.js
 */

const fs = require('fs')
const os = require('os')
const path = require('path')

const {
  loadSchema, parseSchemaTree, checkSchemaImports, buildRegistry, validate, parseSchemaDocument,
} = require('../parser.js')

let pass = 0
const fails = []

function check(name, cond, detail) {
  if (cond) { pass++; return }
  fails.push(`${name}${detail ? ` — ${detail}` : ''}`)
}

function expectThrow(name, fn, needle) {
  try {
    fn()
    fails.push(`${name} — 期望报错，但没有`)
  } catch (err) {
    check(name, String(err.message).includes(needle), `实际: ${err.message}`)
  }
}

/* 在临时目录里铺一份 schema，返回入口目录 */
let rootSeq = 0
function mkSchema(files) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `maque-schema-${rootSeq++}-`))
  for (const [rel, text] of Object.entries(files)) {
    const abs = path.join(dir, rel)
    fs.mkdirSync(path.dirname(abs), { recursive: true })
    fs.writeFileSync(abs, text)
  }
  return dir
}

/* 完整跑一遍（load → parse → validate → import 校验），返回 {tree, namespaces, errors, warnings} */
function run(schemaArg) {
  const tree = loadSchema(schemaArg)
  const namespaces = parseSchemaTree(tree)
  const registry = buildRegistry(namespaces)
  for (const ns of namespaces) for (const t of ns.types) t.typeId = 0
  const { errors, warnings } = validate(namespaces, registry)
  const imports = checkSchemaImports(tree, namespaces)
  return { tree, namespaces, errors: errors.concat(imports.errors), warnings: warnings.concat(imports.warnings) }
}

const VEC = `- namespace: math
  version: 1
  class_list:
    - struct: Vec3
      size: 12
      fields:
        - { name: x, type: float, default: 0.0F }
        - { name: y, type: float, default: 0.0F }
        - { name: z, type: float, default: 0.0F }
`
const SPRITE = `- imports: [../vec.yaml]
- namespace: render
  version: 1
  class_list:
    - struct: Sprite
      fields:
        - { name: pos, type: math.Vec3 }
`

// ---- 1. 合并顺序：被 import 的文件先于 import 者（后序、去重、确定性）----
{
  const dir = mkSchema({
    'schema.yaml': '- imports: [render/sprite.yaml]\n',
    'render/sprite.yaml': SPRITE,
    'vec.yaml': VEC,
  })
  const { tree, namespaces, errors } = run(dir)
  check('后序合并（依赖在前）', namespaces.map((n) => n.name).join(',') === 'math,render',
    namespaces.map((n) => n.name).join(','))
  check('files 顺序 = 合并顺序', tree.files.map((f) => f.rel).join(',') === 'vec.yaml,render/sprite.yaml,schema.yaml',
    tree.files.map((f) => f.rel).join(','))
  check('来源标注（namespace）', namespaces[1].source === 'render/sprite.yaml', namespaces[1].source)
  check('来源标注（类型）', namespaces[1].types[0].source === 'render/sprite.yaml')
  check('无错误', errors.length === 0, errors.join(' | '))
  check('无未使用 import 告警', run(dir).warnings.length === 0, run(dir).warnings.join(' | '))
  check('deps 去重（菱形）', (() => {
    const d2 = mkSchema({
      'schema.yaml': '- imports: [a.yaml, b.yaml]\n',
      'a.yaml': `- imports: [common.yaml]\n- namespace: na
  version: 1
  class_list:
    - struct: A
      fields: [{ name, type: uint32_t }]
`,
      'b.yaml': `- imports: [common.yaml]\n- namespace: nb
  version: 1
  class_list:
    - struct: B
      fields: [{ name, type: uint32_t }]
`,
      'common.yaml': `- namespace: nc
  version: 1
  class_list:
    - struct: C
      fields: [{ name, type: uint32_t }]
`,
    })
    return run(d2).namespaces.map((n) => n.name).join(',') === 'nc,na,nb'
  })())
}

// ---- 2. 跨文件扩展同一 namespace（版本一致；module_abi/global 唯一）----
{
  const dir = mkSchema({
    'schema.yaml': '- imports: [a.yaml, b.yaml]\n',
    'a.yaml': `- namespace: shared
  version: 1
  class_list:
    - struct: A
      fields: [{ name, type: uint32_t }]
`,
    'b.yaml': `- namespace: shared
  version: 1
  class_list:
    - struct: B
      fields: [{ name, type: uint32_t }]
`,
  })
  const { namespaces, errors } = run(dir)
  check('扩展 namespace', namespaces.length === 1 && namespaces[0].types.length === 2,
    JSON.stringify(namespaces.map((n) => n.types.length)))
  check('扩展后无错误', errors.length === 0, errors.join(' | '))

  const bad = mkSchema({
    'schema.yaml': '- imports: [a.yaml, b.yaml]\n',
    'a.yaml': '- namespace: shared\n  version: 1\n',
    'b.yaml': '- namespace: shared\n  version: 2\n',
  })
  expectThrow('扩展 namespace 版本不一致 → 报错', () => run(bad), '版本不一致')
}

// ---- 3. 单文件（旧式 schema.yaml）仍可用 ----
{
  const dir = mkSchema({ 'legacy.yaml': VEC })
  const { namespaces } = run(path.join(dir, 'legacy.yaml'))
  check('单文件模式', namespaces.length === 1 && namespaces[0].name === 'math')
  check('parseSchemaDocument 直调（无来源）', parseSchemaDocument([{ namespace: 'x', class_list: [] }]).length === 1)
}

// ---- 4. 硬校验 ----
{
  const missing = mkSchema({ 'schema.yaml': '- imports: [nope.yaml]\n' })
  expectThrow('import 不存在 → 报错', () => run(missing), 'import 的文件不存在')

  const cycle = mkSchema({
    'schema.yaml': '- imports: [a.yaml]\n',
    'a.yaml': '- imports: [b.yaml]\n',
    'b.yaml': '- imports: [a.yaml]\n',
  })
  expectThrow('import 成环 → 报错', () => run(cycle), 'import 成环')

  const escape = mkSchema({
    'schema.yaml': '- imports: [../outside.yaml]\n',
    'sub/x.yaml': VEC,
  })
  fs.writeFileSync(path.join(path.dirname(escape), 'outside.yaml'), VEC)
  expectThrow('import 越出根目录 → 报错', () => run(escape), '越出 schema 根目录')

  const dup = mkSchema({
    'schema.yaml': '- imports: [a.yaml]\n',
    'a.yaml': `- namespace: math
  version: 1
  class_list: []
- namespace: math
  version: 1
  class_list: []
`,
  })
  expectThrow('同一文件重复 namespace → 报错', () => run(dup), '在同一文件内重复声明')

  const orphan = mkSchema({
    'schema.yaml': '- imports: [a.yaml]\n',
    'a.yaml': '- namespace: na\n  version: 1\n',
    'dead.yaml': VEC,
  })
  const orphanRun = run(orphan)
  check('目录内死文件 → 报错', orphanRun.errors.some((e) => e.includes('未被 import 覆盖')),
    orphanRun.errors.join(' | '))

  const badEntry = mkSchema({ 'schema.yaml': '- 42\n' })
  expectThrow('条目不是映射 → 报错', () => run(badEntry), '顶层条目必须是')

  const noEntry = mkSchema({ 'a.yaml': VEC })
  expectThrow('目录缺入口 → 报错', () => run(noEntry), '缺少入口文件 schema.yaml')

  const noImports = mkSchema({ 'x.yaml': '- imports: nope\n' })
  expectThrow('imports 非数组 → 报错', () => run(path.join(noImports, 'x.yaml')), 'imports 必须是路径数组')
}

// ---- 5. 结构依赖：字段类型引用的类型必须来自本文件或 import 闭包 ----
{
  const good = mkSchema({
    'schema.yaml': '- imports: [render.yaml]\n',
    'math.yaml': VEC,
    'render.yaml': `- imports: [math.yaml]
- namespace: render
  version: 1
  class_list:
    - struct: Sprite
      fields: [{ name: pos, type: math.Vec3 }]
`,
  })
  check('覆盖到的结构依赖通过', run(good).errors.length === 0, run(good).errors.join(' | '))

  const bad = mkSchema({
    'schema.yaml': '- imports: [render.yaml, math.yaml]\n',
    'math.yaml': VEC,
    'render.yaml': `- namespace: render
  version: 1
  class_list:
    - struct: Sprite
      fields: [{ name: pos, type: math.Vec3 }]
`,
  })
  const r = run(bad)
  check('未 import 的结构依赖 → 报错', r.errors.some((e) => e.includes('不在 import 闭包内')), r.errors.join(' | '))
  check('报错含可复制的 import 建议', r.errors.some((e) => e.includes('imports 中加入 "./math.yaml"')), r.errors.join(' | '))

  const transitive = mkSchema({
    'schema.yaml': '- imports: [render.yaml]\n',
    'math.yaml': VEC,
    'mid.yaml': '- imports: [math.yaml]\n',
    'render.yaml': `- imports: [mid.yaml]
- namespace: render
  version: 1
  class_list:
    - struct: Sprite
      fields: [{ name: pos, type: math.Vec3 }]
`,
  })
  check('传递闭包算覆盖', run(transitive).errors.length === 0, run(transitive).errors.join(' | '))
}

// ---- 6. 未使用 import → 告警 ----
{
  const dir = mkSchema({
    'schema.yaml': '- imports: [render.yaml]\n',
    'math.yaml': VEC,
    'render.yaml': `- imports: [math.yaml]
- namespace: render
  version: 1
  class_list:
    - struct: Sprite
      fields: [{ name: hp, type: uint32_t }]
`,
  })
  const { errors, warnings } = run(dir)
  check('未使用 import 无硬错误', errors.length === 0, errors.join(' | '))
  check('未使用 import → 告警', warnings.some((w) => w.includes('未被使用')), warnings.join(' | '))
}

// ---- 7. api_list 类型与脚本 ABI 上限：fail closed，不允许生成器静默漏绑 ----
{
  const bytes = parseSchemaDocument([{
    namespace: 'codec', version: 1,
    api_list: [{ fn: 'compress', args: [{ name: 'data', type: 'bytes' }, { name: 'level', type: 'int32_t' }], returns: 'bytes' }],
  }])
  check('bytes API 可解析', bytes[0].apis[0].returns === 'bytes' && bytes[0].apis[0].args[0].type === 'bytes')
  expectThrow('未知 API 参数类型 → 报错', () => parseSchemaDocument([{
    namespace: 'bad', api_list: [{ fn: 'f', args: [{ name: 'x', type: 'ArrayBuffer' }], returns: 'void' }],
  }]), 'unsupported api arg type')
  expectThrow('重复 API 参数名 → 报错', () => parseSchemaDocument([{
    namespace: 'bad', api_list: [{ fn: 'f', args: [{ name: 'x', type: 'float' }, { name: 'x', type: 'float' }] }],
  }]), 'duplicate api arg')
  expectThrow('API 参数超过 16 个 → 报错', () => parseSchemaDocument([{
    namespace: 'bad', api_list: [{ fn: 'f', args: Array.from({ length: 17 }, (_, i) => ({ name: `x${i}`, type: 'float' })) }],
  }]), 'script ABI limit is 16')
}

if (fails.length > 0) {
  for (const f of fails) console.error(`FAIL: ${f}`)
  console.error(`\n${fails.length} failed, ${pass} passed`)
  process.exit(1)
}
console.log(`ok: ${pass} check(s) passed`)
