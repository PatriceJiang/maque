'use strict'

/**
 * maque schema codegen (SCHEMA.md v1)
 *
 * Reads the schema directory (schema/, entry schema/schema.yaml) and produces, from the same
 * canonical IR:
 *   1. generated/maque_types.h   -- C++ types (enum class / struct / defaults / static_assert)
 *   2. generated/maque_desc.h+c  -- C ABI descriptors + registry (mq_schema_types[])
 *   3. generated/maque_schema.json -- normalized, sorted, machine readable schema
 *   (+ maque_api.h / maque_bindings.cpp / maque_control.* / maque_events.* / abi/*.h / API.md / TYPES.md)
 *
 * CLI: node parser.js [--schema schema/] [--out generated] [--check]
 *   --schema 接受目录（内含 schema.yaml 入口）或单个 YAML 文件（旧式单文件 schema）。
 *
 * Output is deterministic: types are emitted in a stable (name + dependency) order,
 * and type_id is FNV-1a 32bit of the fully qualified name (never a sequence number).
 */

const fs = require('fs')
const path = require('path')
const YAML = require('yaml')

// ---------------------------------------------------------------------------
// constants
// ---------------------------------------------------------------------------

const FNV_OFFSET = 0x811c9dc5
const FNV_PRIME = 0x01000193

// scalar -> byte size
const SCALARS = {
  bool: 1,
  char: 1,
  int8_t: 1,
  uint8_t: 1,
  int16_t: 2,
  uint16_t: 2,
  int32_t: 4,
  uint32_t: 4,
  int64_t: 8,
  uint64_t: 8,
  float: 4,
  double: 8,
}

// scalar -> mq_type_kind suffix
const SCALAR_KIND = {
  bool: 'BOOL',
  char: 'I8',
  int8_t: 'I8',
  uint8_t: 'U8',
  int16_t: 'I16',
  uint16_t: 'U16',
  int32_t: 'I32',
  uint32_t: 'U32',
  int64_t: 'I64',
  uint64_t: 'U64',
  float: 'F32',
  double: 'F64',
}

const INTEGER_SCALARS = new Set([
  'char', 'int8_t', 'uint8_t', 'int16_t', 'uint16_t',
  'int32_t', 'uint32_t', 'int64_t', 'uint64_t',
])

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

function fnv1a32(str) {
  let hash = FNV_OFFSET
  for (let i = 0; i < str.length; i++) {
    hash ^= str.charCodeAt(i)
    // Math.imul keeps 32bit unsigned semantics
    hash = Math.imul(hash, FNV_PRIME) >>> 0
  }
  return hash >>> 0
}

function alignUp(value, align) {
  if (align <= 1) return value
  return Math.ceil(value / align) * align
}

function mangle(name) {
  return String(name).replace(/[^A-Za-z0-9_]/g, '_')
}

class SchemaError extends Error {}

// The schema grammar allows bare type expressions such as `type: float[9]` or
// `type: math.Vec3[]`. Inside YAML flow mappings (`{ name: m, type: float[9] }`)
// the `[` / `<` / `,` make those values invalid YAML, so authors are forced to
// quote them. As a convenience we transparently re-quote such values when the
// strict parse fails; this never touches already-quoted values.
function repairUnquotedTypeValues(text) {
  const re = /(^|[\s,{])type:[ \t]*/gm
  const edits = []
  let m
  while ((m = re.exec(text))) {
    const start = m.index + m[0].length
    const first = text[start]
    if (first === undefined || first === '"' || first === "'" || first === '\n') continue
    let i = start
    let angle = 0
    let bracket = 0
    while (i < text.length) {
      const c = text[i]
      if (c === '\n') break
      if (c === '<') angle++
      else if (c === '>') angle--
      else if (c === '[') bracket++
      else if (c === ']') bracket--
      else if ((c === ',' || c === '}') && angle <= 0 && bracket <= 0) break
      i++
    }
    const value = text.slice(start, i).replace(/\s+$/, '')
    if (value) edits.push([start, i, `"${value}"`])
  }
  for (let k = edits.length - 1; k >= 0; k--) {
    const [s, e, v] = edits[k]
    text = text.slice(0, s) + v + text.slice(e)
  }
  return text
}

function parseYamlDocument(text) {
  try {
    return YAML.parse(text)
  } catch (strictError) {
    const repaired = repairUnquotedTypeValues(text)
    if (repaired !== text) {
      try {
        return YAML.parse(repaired)
      } catch (_) {
        /* fall through to the original, more accurate error */
      }
    }
    throw strictError
  }
}


// ---------------------------------------------------------------------------
// type expression parser
//
// grammar:
//   type   := base array*
//   array  := '[' number ']' | '[' ']'
//   base   := scalar | 'string' | 'entity_ref'
//           | 'asset_ref' ('<' type '>')?
//           | 'map' '<' type ',' type '>'
//           | 'optional' '<' type '>'
//           | named
//   named  := identifier ('.' identifier)?
// ---------------------------------------------------------------------------

function tokenizeType(input) {
  const text = String(input)
  const tokens = []
  let i = 0
  while (i < text.length) {
    const ch = text[i]
    if (/\s/.test(ch)) { i++; continue }
    if (ch === '<' || ch === '>' || ch === '[' || ch === ']' || ch === ',') {
      tokens.push(ch)
      i++
      continue
    }
    let m = /^[0-9]+/.exec(text.slice(i))
    if (m) {
      tokens.push({ number: Number(m[0]) })
      i += m[0].length
      continue
    }
    m = /^[A-Za-z_][A-Za-z0-9_.]*/.exec(text.slice(i))
    if (m) {
      tokens.push({ ident: m[0] })
      i += m[0].length
      continue
    }
    throw new SchemaError(`cannot tokenize type expression near "${text.slice(i)}"`)
  }
  return tokens
}

function parseTypeExpr(input) {
  const tokens = tokenizeType(input)
  let pos = 0

  const peek = () => tokens[pos]
  const next = () => tokens[pos++]
  const expect = (sym) => {
    const t = tokens[pos]
    if (t !== sym) throw new SchemaError(`expected "${sym}" in type expression "${input}"`)
    pos++
  }

  function parseBase() {
    const t = next()
    if (t === undefined) throw new SchemaError(`unexpected end of type expression "${input}"`)
    if (typeof t === 'object' && t.number !== undefined) {
      throw new SchemaError(`unexpected number in type expression "${input}"`)
    }
    if (typeof t === 'object' && t.ident !== undefined) {
      const name = t.ident
      if (SCALARS[name] !== undefined) return { cat: 'scalar', name }
      switch (name) {
        case 'string': return { cat: 'string' }
        case 'entity_ref': return { cat: 'entity_ref' }
        case 'asset_ref': {
          if (peek() === '<') {
            expect('<')
            const inner = parseType()
            expect('>')
            return { cat: 'asset_ref', inner }
          }
          return { cat: 'asset_ref' }
        }
        case 'map': {
          expect('<')
          const key = parseType()
          expect(',')
          const value = parseType()
          expect('>')
          return { cat: 'map', key, value }
        }
        case 'optional': {
          expect('<')
          const inner = parseType()
          expect('>')
          return { cat: 'optional', inner }
        }
        default:
          if (!/^[A-Za-z_][A-Za-z0-9_]*(\.[A-Za-z_][A-Za-z0-9_]*)*$/.test(name)) {
            throw new SchemaError(`invalid named type "${name}"`)
          }
          return { cat: 'named', name }
      }
    }
    throw new SchemaError(`unexpected token in type expression "${input}"`)
  }

  function parseType() {
    let node = parseBase()
    while (peek() === '[') {
      expect('[')
      const t = peek()
      if (t === ']') {
        expect(']')
        node = { cat: 'array_dyn', elem: node }
      } else if (typeof t === 'object' && t.number !== undefined) {
        const count = next().number
        expect(']')
        if (count <= 0) throw new SchemaError(`array length must be > 0 in "${input}"`)
        node = { cat: 'array_fixed', elem: node, count }
      } else {
        throw new SchemaError(`expected array length or "]" in "${input}"`)
      }
    }
    return node
  }

  const result = parseType()
  if (pos !== tokens.length) {
    throw new SchemaError(`trailing tokens in type expression "${input}"`)
  }
  return result
}

// ---------------------------------------------------------------------------
// schema loading
// ---------------------------------------------------------------------------

function parseAttrs(raw) {
  if (!raw || typeof raw !== 'object') return {}
  const attrs = {}
  for (const key of Object.keys(raw)) attrs[key] = raw[key]
  return attrs
}

function attrsFlags(attrs) {
  let flags = 0
  if (attrs.transient) flags |= 1
  if (attrs.readonly) flags |= 2
  if (attrs.deprecated) flags |= 4
  return flags
}

function parseSchemaDocument(root) {
  if (!Array.isArray(root)) {
    throw new SchemaError('schema root must be an array of namespaces')
  }

  const namespaces = []
  const seenNamespaces = new Set()

  for (const nsEntry of root) {
    if (!nsEntry || typeof nsEntry !== 'object') {
      throw new SchemaError('each namespace entry needs a "namespace" key')
    }
    if (nsEntry.interface !== undefined) continue   // 接口条目另由 parseInterfaces 处理
    if (nsEntry.namespace === undefined) {
      throw new SchemaError('each namespace entry needs a "namespace" key')
    }
    const nsName = String(nsEntry.namespace)
    if (seenNamespaces.has(nsName)) {
      throw new SchemaError(`duplicate namespace "${nsName}"`)
    }
    seenNamespaces.add(nsName)

    const list = nsEntry.class_list || nsEntry.type_list || []
    if (!Array.isArray(list)) {
      throw new SchemaError(`namespace "${nsName}" type list must be an array`)
    }

    const types = []
    for (const def of list) {
      types.push(parseTypeDef(nsName, def))
    }
    const apis = []
    if (Array.isArray(nsEntry.api_list)) {
      for (const a of nsEntry.api_list) apis.push(parseApiDef(nsName, a))
    }
    // 控制平面方法（AI/工具公开面，docs/11 §3、docs/21 §6.3）
    const controls = []
    const mutatingSet = new Set(Array.isArray(nsEntry.control_mutating) ? nsEntry.control_mutating.map(String) : [])
    if (Array.isArray(nsEntry.control_list)) {
      for (const c of nsEntry.control_list) {
        if (!c || c.method === undefined) {
          throw new SchemaError(`invalid control definition in namespace "${nsName}"`)
        }
        controls.push({
          ns: nsName,
          method: String(c.method),
          args: Array.isArray(c.args)
            ? c.args.map((a) => ({ name: String(a.name), type: String(a.type) }))
            : [],
          summary: c.summary === undefined ? null : String(c.summary),
          mutating: c.mutating === true || mutatingSet.has(String(c.method)),   // 权限（D2 默认只读）
        })
      }
    }
    // 模块 C ABI（docs/02/21）：跨模块公开接口也只声明一次，头文件由生成器产出
    let moduleAbi = null
    if (nsEntry.module_abi) {
      const ma = nsEntry.module_abi
      if (!ma.module) throw new SchemaError(`module_abi in "${nsName}" needs a module name`)
      moduleAbi = {
        module: String(ma.module),
        version: ma.version === undefined ? 1 : Number(ma.version),
        // language: c | cxx；cxx 时声明包在 namespace 内（无 extern "C"）
        language: ma.language === undefined ? 'c' : String(ma.language),
        nameSpace: ma.namespace === undefined ? null : String(ma.namespace),
        abiName: ma.abi_name === undefined ? `maque.${ma.module}` : String(ma.abi_name),
        summary: ma.summary === undefined ? null : String(ma.summary),
        includes: Array.isArray(ma.includes) ? ma.includes.map(String) : [],
        opaque: Array.isArray(ma.opaque) ? ma.opaque.map(String) : [],
        enums: Array.isArray(ma.enums) ? ma.enums.map((e) => ({
          name: e.name === undefined ? null : String(e.name),
          doc: e.doc === undefined ? null : String(e.doc),
          values: (e.values || []).map((v) => ({
            name: String(v.name), value: Number(v.value),
            doc: v.doc === undefined ? null : String(v.doc),
          })),
        })) : [],
        structs: Array.isArray(ma.structs) ? ma.structs.map((st) => ({
          name: String(st.name),
          doc: st.doc === undefined ? null : String(st.doc),
          fields: (st.fields || []).map((f) => ({
            name: String(f.name), type: String(f.type),
            doc: f.doc === undefined ? null : String(f.doc),
          })),
        })) : [],
        // code：原样输出的 C/C++ 代码段（如 static inline 辅助函数）
        code: ma.code === undefined ? null : String(ma.code),
        consts: Array.isArray(ma.consts) ? ma.consts.map((c) => ({
          name: String(c.name), type: String(c.type), value: String(c.value),
          doc: c.doc === undefined ? null : String(c.doc),
        })) : [],
        typedefs: Array.isArray(ma.typedefs) ? ma.typedefs.map((t) => ({
          name: String(t.name), type: String(t.type),
        })) : [],
        macros: Array.isArray(ma.macros) ? ma.macros.map((m) => ({
          name: String(m.name), value: String(m.value),
          // value_cxx：C++ 与 C 取值不同的宏（如复合字面量 vs 列表初始化）
          valueCxx: m.value_cxx === undefined ? null : String(m.value_cxx),
          // raw：整段原样输出（含 #if defined(_WIN32) 等平台条件）
          raw: m.raw === undefined ? null : String(m.raw),
          doc: m.doc === undefined ? null : String(m.doc),
        })) : [],
        fns: (ma.fns || []).map((f) => ({
          name: String(f.fn),
          returns: f.returns === undefined ? 'void' : String(f.returns),
          doc: f.doc === undefined ? null : String(f.doc),
          args: (f.args || []).map((a) => ({
            name: String(a.name), type: String(a.type),
            doc: a.doc === undefined ? null : String(a.doc),
          })),
        })),
      }
      if (!moduleAbi.fns.length) {
        throw new SchemaError(`module_abi "${moduleAbi.module}" has no fns`)
      }
    }

    // 脚本事件回调（docs/14 §4、docs/19 §8）
    const callbacks = []
    if (Array.isArray(nsEntry.callbacks)) {
      for (const cb of nsEntry.callbacks) {
        if (!cb || cb.name === undefined) {
          throw new SchemaError(`invalid callback definition in namespace "${nsName}"`)
        }
        callbacks.push({
          ns: nsName,
          name: String(cb.name),
          // on：该回调响应的输入事件（mq_input_event_type 名，如 pointer_down）；无则不由输入触发
          on: cb.on === undefined ? null : String(cb.on),
          args: Array.isArray(cb.args)
            ? cb.args.map((a) => ({ name: String(a.name), type: String(a.type) }))
            : [],
        })
      }
    }
    namespaces.push({
      name: nsName,
      version: nsEntry.version === undefined ? 1 : Number(nsEntry.version),
      types,
      apis,
      controls,
      callbacks,
      moduleAbi,
      global: !!nsEntry.global,     // api 绑定到脚本全局（无命名空间前缀）
    })
  }
  return namespaces
}

// 函数接口声明：{ fn, args:[{name,type}], returns }（类型为标量/string）。
function parseApiDef(nsName, def) {
  if (!def || typeof def !== 'object' || def.fn === undefined) {
    throw new SchemaError(`invalid api definition in namespace "${nsName}"`)
  }
  const args = Array.isArray(def.args)
    ? def.args.map((a) => ({ name: String(a.name), type: String(a.type) }))
    : []
  return {
    ns: nsName,
    name: String(def.fn),
    args,
    returns: def.returns === undefined ? 'void' : String(def.returns),
    // MANUAL：实现的头文件（生成器只 include，不重复声明；docs/21）
    manual: def.manual === undefined || def.manual === null ? null : String(def.manual),
    // ctx：函数首参为实例上下文 void* ctx（宿主 API；绑定注册时注入）
    ctx: !!def.ctx,
  }
}

function parseTypeDef(nsName, def) {
  if (!def || typeof def !== 'object') {
    throw new SchemaError(`invalid type definition in namespace "${nsName}"`)
  }

  if (def.struct !== undefined || def.class !== undefined) {
    const name = String(def.struct !== undefined ? def.struct : def.class)
    const fields = Array.isArray(def.fields) ? def.fields : []
    return {
      kind: 'struct',
      ns: nsName,
      name,
      fqName: `${nsName}.${name}`,
      size: def.size === undefined ? null : Number(def.size),
      align: def.align === undefined ? null : Number(def.align),
      fields: fields.map((f) => parseField(nsName, name, f)),
      roles: Array.isArray(def.roles) ? def.roles.map(String) : [],
      values: null,
      underlying: null,
      aliasNode: null,
      aliasText: null,
    }
  }

  if (def.enum !== undefined) {
    const name = String(def.enum)
    const values = Array.isArray(def.values) ? def.values : []
    return {
      kind: 'enum',
      ns: nsName,
      name,
      fqName: `${nsName}.${name}`,
      size: null,
      align: null,
      fields: [],
      values: values.map((v) => ({
        name: String(v.name),
        rawValue: v.value === undefined ? null : Number(v.value),
      })),
      underlying: def.underlying === undefined ? 'int32_t' : String(def.underlying),
      aliasNode: null,
      aliasText: null,
    }
  }

  if (def.alias !== undefined) {
    let name
    let targetText
    if (typeof def.alias === 'string') {
      const m = /^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+?)\s*$/.exec(def.alias)
      if (!m) throw new SchemaError(`invalid alias "${def.alias}" in namespace "${nsName}"`)
      name = m[1]
      targetText = m[2]
    } else if (def.alias && typeof def.alias === 'object') {
      name = String(def.alias.name)
      targetText = String(def.alias.target)
    } else {
      throw new SchemaError(`invalid alias definition in namespace "${nsName}"`)
    }
    return {
      kind: 'alias',
      ns: nsName,
      name,
      fqName: `${nsName}.${name}`,
      size: null,
      align: null,
      fields: [],
      values: null,
      underlying: null,
      aliasNode: parseTypeExpr(targetText),
      aliasText: targetText,
    }
  }

  throw new SchemaError(`type definition in namespace "${nsName}" has no struct/class/enum/alias key`)
}

function parseField(nsName, typeName, f) {
  if (!f || typeof f !== 'object' || f.name === undefined || f.type === undefined) {
    throw new SchemaError(`invalid field in ${nsName}.${typeName} (needs name + type)`)
  }
  const typeNode = parseTypeExpr(f.type)
  return {
    name: String(f.name),
    typeText: String(f.type),
    typeNode,
    default: f.default === undefined ? null : f.default,
    attrs: parseAttrs(f.attrs),
  }
}

// ---------------------------------------------------------------------------
// resolution + derived properties (pod / dynamic / layout)
// ---------------------------------------------------------------------------

function buildRegistry(namespaces, errors) {
  const byFq = new Map()
  const bySimple = new Map()
  for (const ns of namespaces) {
    for (const t of ns.types) {
      if (byFq.has(t.fqName)) {
        if (errors) {
          errors.push(`duplicate type name "${t.fqName}"`)
          continue
        }
        throw new SchemaError(`duplicate type name "${t.fqName}"`)
      }
      byFq.set(t.fqName, t)
      if (!bySimple.has(t.name)) bySimple.set(t.name, [])
      bySimple.get(t.name).push(t)
    }
  }
  return { byFq, bySimple }
}

function resolveNamedNode(node, currentNs, registry, errors) {
  if (!node || typeof node !== 'object') return
  switch (node.cat) {
    case 'named': {
      const ref = node.name
      let target = null
      if (ref.includes('.')) {
        target = registry.byFq.get(ref) || null
        if (!target) {
          errors.push(`unknown type reference "${ref}" (fully qualified)`)
          return
        }
      } else {
        const local = registry.byFq.get(`${currentNs}.${ref}`)
        if (local) {
          target = local
        } else {
          const candidates = registry.bySimple.get(ref) || []
          if (candidates.length === 1) {
            target = candidates[0]
          } else if (candidates.length === 0) {
            errors.push(`unknown type reference "${ref}"`)
            return
          } else {
            errors.push(
              `ambiguous type reference "${ref}" (candidates: ${candidates.map((c) => c.fqName).join(', ')})`
            )
            return
          }
        }
      }
      node.resolved = target
      return
    }
    case 'asset_ref':
      if (node.inner) resolveNamedNode(node.inner, currentNs, registry, errors)
      return
    case 'optional':
      resolveNamedNode(node.inner, currentNs, registry, errors)
      return
    case 'map':
      resolveNamedNode(node.key, currentNs, registry, errors)
      resolveNamedNode(node.value, currentNs, registry, errors)
      return
    case 'array_fixed':
    case 'array_dyn':
      resolveNamedNode(node.elem, currentNs, registry, errors)
      return
    default:
      return
  }
}

function resolveTypes(namespaces, registry, errors) {
  for (const ns of namespaces) {
    for (const t of ns.types) {
      if (t.kind === 'struct') {
        for (const f of t.fields) resolveNamedNode(f.typeNode, ns.name, registry, errors)
      } else if (t.kind === 'alias') {
        resolveNamedNode(t.aliasNode, ns.name, registry, errors)
      }
    }
  }
}

// pod / dynamic determination, memoized per type with cycle guard
function typePod(t, guard = new Set()) {
  if (t._pod !== undefined) return t._pod
  if (guard.has(t)) return true // recursive by-value cycle: treat as trivially pod to break the loop
  guard.add(t)
  let result
  if (t.kind === 'enum') {
    result = true
  } else if (t.kind === 'alias') {
    result = nodePod(t.aliasNode, guard)
  } else {
    result = t.fields.every((f) => nodePod(f.typeNode, guard))
  }
  guard.delete(t)
  t._pod = result
  return result
}

function nodePod(node, guard) {
  if (!node) return true
  switch (node.cat) {
    case 'scalar':
    case 'entity_ref':
    case 'asset_ref':
      return true
    case 'named':
      return node.resolved ? typePod(node.resolved, guard) : true
    case 'array_fixed':
      return nodePod(node.elem, guard)
    case 'string':
    case 'map':
    case 'optional':
    case 'array_dyn':
      return false
    default:
      return true
  }
}

function typeHasDynamic(t, guard = new Set()) {
  if (t._dyn !== undefined) return t._dyn
  if (guard.has(t)) return false
  guard.add(t)
  let result
  if (t.kind === 'enum') {
    result = false
  } else if (t.kind === 'alias') {
    result = nodeHasDynamic(t.aliasNode, guard)
  } else {
    result = t.fields.some((f) => nodeHasDynamic(f.typeNode, guard))
  }
  guard.delete(t)
  t._dyn = result
  return result
}

function nodeHasDynamic(node, guard) {
  if (!node) return false
  switch (node.cat) {
    case 'string':
    case 'map':
    case 'optional':
    case 'array_dyn':
      return true
    case 'named':
      return node.resolved ? typeHasDynamic(node.resolved, guard) : false
    case 'array_fixed':
      return nodeHasDynamic(node.elem, guard)
    default:
      return false
  }
}

// size / align model -- returns null when it cannot be computed in JS

function nodeSize(node, guard = new Set()) {
  if (!node) return null
  switch (node.cat) {
    case 'scalar':
      return SCALARS[node.name]
    case 'named':
      return node.resolved ? typeSize(node.resolved, guard) : null
    case 'array_fixed': {
      const s = nodeSize(node.elem, guard)
      return s === null ? null : s * node.count
    }
    default:
      return null // dynamic containers and refs (ref size unknown here)
  }
}

function nodeAlign(node, guard = new Set()) {
  if (!node) return 1
  switch (node.cat) {
    case 'scalar':
      return SCALARS[node.name] || 1
    case 'named':
      return node.resolved ? typeAlign(node.resolved, guard) : 1
    case 'array_fixed':
      return nodeAlign(node.elem, guard)
    case 'entity_ref':
    case 'asset_ref':
      return 4 // assumed handle alignment
    default:
      return 8 // dynamic containers / unknown
  }
}

function typeSize(t, guard = new Set()) {
  if (t.kind === 'enum') return SCALARS[t.underlying] === undefined ? null : SCALARS[t.underlying]
  if (t.kind === 'alias') return nodeSize(t.aliasNode, guard)
  if (t.size !== null) return t.size
  const layout = structLayout(t, guard)
  return layout ? layout.size : null
}

function typeAlign(t, guard = new Set()) {
  if (t.kind === 'enum') return SCALARS[t.underlying] === undefined ? 1 : SCALARS[t.underlying]
  if (t.kind === 'alias') return nodeAlign(t.aliasNode, guard)
  if (t.align !== null) return t.align
  const layout = structLayout(t, guard)
  return layout ? layout.align : 1
}

function structLayout(t, guard = new Set()) {
  if (t._layout !== undefined) return t._layout
  if (guard.has(t)) return null
  guard.add(t)
  let offset = 0
  let maxAlign = 1
  let ok = true
  for (const f of t.fields) {
    const s = nodeSize(f.typeNode, guard)
    const a = nodeAlign(f.typeNode, guard)
    if (s === null || a === null || a <= 0) { ok = false; break }
    offset = alignUp(offset, a)
    offset += s
    if (a > maxAlign) maxAlign = a
  }
  guard.delete(t)
  const result = ok ? { size: alignUp(offset, maxAlign), align: maxAlign } : null
  if (ok) t._layout = result
  return result
}

// ---------------------------------------------------------------------------
// validation (SCHEMA.md v1 §9)
// ---------------------------------------------------------------------------

function validate(namespaces, registry) {
  const errors = []
  const warnings = []

  // 1. unique type names (already enforced in buildRegistry), unique ns (parse),
  //    unique field names within a type, duplicate enum values
  for (const ns of namespaces) {
    for (const t of ns.types) {
      if (t.kind === 'struct') {
        const seen = new Set()
        for (const f of t.fields) {
          if (seen.has(f.name)) errors.push(`duplicate field "${f.name}" in ${t.fqName}`)
          seen.add(f.name)
        }
      } else if (t.kind === 'enum') {
        const seenNames = new Set()
        const seenValues = new Set()
        let auto = -1
        for (const v of t.values) {
          if (seenNames.has(v.name)) errors.push(`duplicate enum member "${v.name}" in ${t.fqName}`)
          seenNames.add(v.name)
          const value = v.rawValue === null ? auto + 1 : v.rawValue
          v.value = value
          auto = value
          if (seenValues.has(value)) errors.push(`duplicate enum value ${value} in ${t.fqName}`)
          seenValues.add(value)
        }
        if (SCALARS[t.underlying] === undefined || !INTEGER_SCALARS.has(t.underlying)) {
          errors.push(`enum ${t.fqName} has non-integer underlying type "${t.underlying}"`)
        }
      } else if (t.kind === 'alias') {
        const target = t.aliasNode
        if (target && target.cat === 'scalar' && SCALARS[target.name] === undefined) {
          errors.push(`alias ${t.fqName} targets unknown scalar`)
        }
      }
    }
  }

  // resolve references (unknown/ambiguous)
  resolveTypes(namespaces, registry, errors)

  // 3. size/align: only POD may declare size/align, and a declared POD size must
  //    equal the computed layout exactly (not merely be "large enough").
  const sizeMismatches = []
  for (const ns of namespaces) {
    for (const t of ns.types) {
      if (t.kind !== 'struct') continue
      const pod = typePod(t)
      if ((t.size !== null || t.align !== null) && !pod) {
        errors.push(
          `${t.fqName} declares size/align but is not POD (contains dynamic container/string/map/optional)`
        )
        continue
      }
      const layout = structLayout(t)
      if (t.size !== null) {
        if (layout) {
          if (t.size !== layout.size) {
            errors.push(
              `POD size mismatch: ${t.fqName} | declared ${t.size} | computed ${layout.size}`
            )
            sizeMismatches.push({ name: t.fqName, declared: t.size, computed: layout.size })
          }
        } else {
          warnings.push(
            `${t.fqName} declares size ${t.size} but its layout cannot be computed in JS (contains handle/reference types); relying on compiler static_assert`
          )
        }
      }
      if (t.align !== null && layout && t.align !== layout.align) {
        warnings.push(
          `${t.fqName} declared align ${t.align} differs from computed alignment ${layout.align}`
        )
      }
    }
  }

  // 4. default value sanity (best effort, warnings only)
  for (const ns of namespaces) {
    for (const t of ns.types) {
      if (t.kind !== 'struct') continue
      for (const f of t.fields) {
        if (f.default === null) continue
        const node = f.typeNode
        if (node.cat === 'scalar' && node.name !== 'bool' && node.name !== 'char') {
          const text = String(f.default).trim()
          const numeric =
            /^[{(\[]/.test(text) ||
            /^[-+]?0[xX][0-9a-fA-F]+[uUlL]*$/.test(text) ||
            /^[-+]?(\d+\.?\d*|\.\d+)([eE][-+]?\d+)?[fFuUlL]*$/.test(text)
          if (!numeric) {
            warnings.push(`${t.fqName}.${f.name} default "${text}" does not look numeric`)
          }
        }
        if (node.cat === 'named' && node.resolved && node.resolved.kind === 'enum') {
          const text = String(f.default)
          const target = node.resolved
          const memberMatch = /([A-Za-z_][A-Za-z0-9_]*)\s*$/.exec(text)
          if (memberMatch && target.values) {
            const memberName = memberMatch[1]
            if (!target.values.some((v) => v.name === memberName)) {
              warnings.push(
                `${t.fqName}.${f.name} default "${text}" is not a member of enum ${target.fqName}`
              )
            }
          }
        }
      }
    }
  }

  return { errors, warnings, sizeMismatches }
}

// ---------------------------------------------------------------------------
// C++ rendering
// ---------------------------------------------------------------------------

function cppTypeName(node) {
  switch (node.cat) {
    case 'scalar':
      return node.name
    case 'named':
      return node.resolved ? node.resolved.fqName.replace(/\./g, '::') : node.name.replace(/\./g, '::')
    case 'string':
      return 'std::string'
    case 'entity_ref':
      return 'maque::EntityRef'
    case 'asset_ref':
      return 'maque::AssetRef'
    case 'map':
      return `std::map<${cppTypeName(node.key)},${cppTypeName(node.value)}>`
    case 'optional':
      return `std::optional<${cppTypeName(node.inner)}>`
    case 'array_dyn':
      return `std::vector<${cppTypeName(node.elem)}>`
    case 'array_fixed':
      return cppTypeName(node.elem)
    default:
      throw new SchemaError(`cannot render C++ type for node ${node.cat}`)
  }
}

function cppFieldDecl(node, name) {
  if (node.cat === 'array_fixed') {
    return `${cppTypeName(node.elem)} ${name}[${node.count}]`
  }
  return `${cppTypeName(node)} ${name}`
}

// Normalize a schema `default:` expression for C++ emission:
//   - `null` / `nullptr` -> `{}` (works for handle/aggregate defaults)
//   - `ns.Name...` -> `ns::Name...` for any *defined* namespace, so authors may
//     write `anim.InterpMode::Linear`. Numeric literals such as `1.0F` are left
//     untouched because the token before `.` is a digit, not a namespace name.
// String defaults (e.g. "0xFFFFFFFFFFFFFFFFULL") are emitted verbatim.
function normalizeDefaultValue(value, nsSet) {
  if (value === null || value === undefined) return null
  let text = String(value)
  if (/^\s*(null|nullptr)\s*$/.test(text)) return '{}'
  text = text.replace(/([A-Za-z_][A-Za-z0-9_]*)\.(?=[A-Za-z_])/g, (match, name) =>
    nsSet.has(name) ? `${name}::` : match
  )
  return text
}

// deterministic ordering: dependency aware, name tiebreak
function topoSort(items, depsOf) {
  const remaining = new Map()
  items.forEach((it) => remaining.set(it, new Set()))
  for (const it of items) {
    for (const dep of depsOf(it)) {
      if (dep !== it && remaining.has(dep)) remaining.get(it).add(dep)
    }
  }
  const ordered = []
  const byName = (a, b) => keyOf(a).localeCompare(keyOf(b))
  function keyOf(x) { return x.fqName || x.name }
  while (remaining.size > 0) {
    const ready = [...remaining.entries()]
      .filter(([, deps]) => deps.size === 0)
      .map(([it]) => it)
      .sort(byName)
    if (ready.length === 0) {
      // cycle: fall back to name order for the rest (deterministic)
      const rest = [...remaining.keys()].sort(byName)
      ordered.push(...rest)
      break
    }
    for (const it of ready) {
      ordered.push(it)
      remaining.delete(it)
    }
    for (const deps of remaining.values()) {
      for (const it of ready) deps.delete(it)
    }
  }
  return ordered
}

function namedRefsOf(node, out = new Set()) {
  if (!node) return out
  switch (node.cat) {
    case 'named':
      if (node.resolved) out.add(node.resolved)
      break
    case 'asset_ref':
      // emits maque::AssetRef regardless of T -> no C++ declaration dependency
      break
    case 'optional':
      namedRefsOf(node.inner, out)
      break
    case 'map':
      namedRefsOf(node.key, out); namedRefsOf(node.value, out)
      break
    case 'array_fixed':
    case 'array_dyn':
      namedRefsOf(node.elem, out)
      break
    default:
      break
  }
  return out
}

function typeDeps(t) {
  const out = new Set()
  if (t.kind === 'struct') {
    for (const f of t.fields) namedRefsOf(f.typeNode, out)
  } else if (t.kind === 'alias') {
    namedRefsOf(t.aliasNode, out)
  }
  return out
}

function orderNamespaces(namespaces) {
  const depsOf = (ns) => {
    const out = new Set()
    for (const t of ns.types) {
      for (const dep of typeDeps(t)) {
        if (dep.ns !== ns.name) out.add(namespaces.find((n) => n.name === dep.ns))
      }
    }
    out.delete(undefined)
    out.delete(ns)
    return out
  }
  return topoSort(namespaces, depsOf)
}

function genTypesHeader(namespaces) {
  const namespacesByName = new Set(namespaces.map((n) => n.name))
  const lines = []
  lines.push('// Generated by parser.js -- DO NOT EDIT')
  lines.push('// Source: schema/ (SCHEMA.md v1)')
  lines.push('')
  lines.push('#pragma once')
  lines.push('')
  lines.push('#include <cstdint>')
  lines.push('#include <cstdio>')
  lines.push('#include <string>')
  lines.push('#include <vector>')
  lines.push('#include <map>')
  lines.push('#include <optional>')
  lines.push('')
  lines.push('#ifndef MAQUE_REFLECT_REFS_DEFINED')
  lines.push('#define MAQUE_REFLECT_REFS_DEFINED')
  lines.push('namespace maque {')
  lines.push('struct EntityRef { uint32_t id = 0; uint32_t generation = 0; };')
  lines.push('struct AssetRef { uint64_t id = 0; };')
  lines.push('} // namespace maque')
  lines.push('#endif // MAQUE_REFLECT_REFS_DEFINED')

  for (const ns of orderNamespaces(namespaces)) {
    lines.push('')
    lines.push(`namespace ${ns.name} {`)
    lines.push('')
    const ordered = topoSort(ns.types, typeDeps)
    for (const t of ordered) {
      if (t.kind === 'enum') {
        lines.push(`enum class ${t.name} : ${t.underlying} {`)
        const width = t.values.reduce((w, v) => Math.max(w, v.name.length), 0)
        for (const v of t.values) {
          lines.push(`    ${v.name.padEnd(width)} = ${v.value},`)
        }
        lines.push('};')
        lines.push('')
      } else if (t.kind === 'alias') {
        lines.push(`using ${t.name} = ${cppTypeName(t.aliasNode)};`)
        lines.push('')
      } else {
        lines.push(`struct ${t.name} {`)
        for (const f of t.fields) {
          const decl = cppFieldDecl(f.typeNode, f.name)
          const isString = cppTypeName(f.typeNode) === 'std::string'
          if (f.default !== null && f.default !== undefined) {
            const dv = isString
              ? JSON.stringify(String(f.default))          // 正确的 C++ 字符串字面量
              : normalizeDefaultValue(f.default, namespacesByName)
            lines.push(`    ${decl} = ${dv};`)
          } else {
            lines.push(`    ${decl};`)                     // std::string 默认即空串
          }
        }
        lines.push('};')
        if (t.size !== null && typePod(t)) {
          lines.push(
            `static_assert(sizeof(${t.name}) == ${t.size}, "${t.name} must be ${t.size} bytes");`
          )
        }
        if (t.align !== null && typePod(t)) {
          lines.push(
            `static_assert(alignof(${t.name}) == ${t.align}, "${t.name} must be ${t.align}-byte aligned");`
          )
        }
        lines.push('')
      }
    }
    lines.push(`} // namespace ${ns.name}`)
  }

  return lines.join('\n') + '\n'
}

// ---------------------------------------------------------------------------
// descriptor rendering
// ---------------------------------------------------------------------------

function fieldKind(node) {
  switch (node.cat) {
    case 'scalar':
      return SCALAR_KIND[node.name]
    case 'named':
      return node.resolved && node.resolved.kind === 'enum' ? 'ENUM' : 'STRUCT'
    case 'string':
      return 'STRING'
    case 'map':
      return 'MAP'
    case 'entity_ref':
      return 'ENTITY_REF'
    case 'asset_ref':
      return 'ASSET_REF'
    case 'array_fixed':
      return 'ARRAY_FIXED'
    case 'array_dyn':
      return 'ARRAY_DYN'
    case 'optional':
      return 'OPTIONAL'
    default:
      return 'STRUCT'
  }
}

function fieldTypeId(node) {
  switch (node.cat) {
    case 'named':
      return node.resolved ? node.resolved.typeId : 0
    case 'array_fixed':
    case 'array_dyn':
      return fieldTypeId(node.elem)
    case 'optional':
      return fieldTypeId(node.inner)
    default:
      return 0
  }
}

function fieldCount(node) {
  return node.cat === 'array_fixed' ? node.count : 0
}

function typeFlags(t) {
  let flags = 0
  if (t.kind === 'enum') flags |= 1
  if (typePod(t)) flags |= 2
  if (typeHasDynamic(t)) flags |= 4
  return flags
}

function sortedTypes(namespaces) {
  const all = []
  for (const ns of namespaces) for (const t of ns.types) all.push(t)
  return all.sort((a, b) => a.fqName.localeCompare(b.fqName))
}

function genDescHeader() {
  return `// Generated by parser.js -- DO NOT EDIT
// C ABI reflection descriptors (SCHEMA.md v1 §6)
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mq_type_kind {
    MQ_TK_BOOL, MQ_TK_I8, MQ_TK_U8, MQ_TK_I16, MQ_TK_U16, MQ_TK_I32, MQ_TK_U32,
    MQ_TK_I64, MQ_TK_U64, MQ_TK_F32, MQ_TK_F64, MQ_TK_ENUM, MQ_TK_STRUCT,
    MQ_TK_ARRAY_FIXED, MQ_TK_ARRAY_DYN, MQ_TK_STRING, MQ_TK_MAP,
    MQ_TK_ENTITY_REF, MQ_TK_ASSET_REF, MQ_TK_OPTIONAL
} mq_type_kind;

/* 动态数组的元素类型哨兵：元素是字符串（无具名类型可引用）。
 * 反射/persist 据此按 std::string 处理（此前 string[] 因 elem_type_id=0 被整体跳过）。 */
#define MQ_ELEM_TYPE_STRING 0xFFFFFFFFu

typedef struct mq_field_desc {
    const char*  name;
    uint32_t     offset;       /* offsetof(Type, field) */
    mq_type_kind kind;
    uint32_t     type_id;      /* named/element type id; 0 if not named */
    uint32_t     count;        /* fixed array length; 0 otherwise */
    uint32_t     attrs_flags;  /* bit0 transient, bit1 readonly, bit2 deprecated */
    /* 动态数组（T[]）的容器算子（其余字段为 0）——使反射可类型化地增删/取元素 */
    uint32_t     elem_type_id;                    /* 具名元素类型 id；0 否则 */
    void       (*container_clear)(void* field);      /* 置空（不释放容量）：构造/析构用 */
    uint32_t   (*container_size)(const void* field);
    void       (*container_resize)(void* field, uint32_t n);
    void*      (*container_elem)(void* field, uint32_t i);
} mq_field_desc;

/* 组件语义角色（由 schema 的 roles 声明；引擎据此通用处理，不硬编码类型）。 */
#define MQ_ROLE_TRANSFORM2D (1u << 0)
#define MQ_ROLE_SPRITE      (1u << 1)
#define MQ_ROLE_UI_BUTTON   (1u << 2)
#define MQ_ROLE_UI_LABEL    (1u << 3)
#define MQ_ROLE_CAMERA      (1u << 4)
#define MQ_ROLE_ANIM_PLAYER (1u << 5)
#define MQ_ROLE_PARTICLE    (1u << 6)
#define MQ_ROLE_NAV_AGENT   (1u << 7)
#define MQ_ROLE_UI_CONTROL  (1u << 8)    /* 2D 盒模型/节点属性（visible/opacity/scale/angle/z_index） */
#define MQ_ROLE_UI_LAYOUT   (1u << 9)    /* 自动布局（H/V/GRID，Layout 等价） */
#define MQ_ROLE_UI_PROGRESS (1u << 10)   /* 进度条（填充子节点宽度/缩放） */
#define MQ_ROLE_UI_SLIDER   (1u << 11)   /* 滑块（拖拽改值 + changed 事件） */
#define MQ_ROLE_UI_CHECK    (1u << 12)   /* 复选框（点击切换 + toggled 事件） */
#define MQ_ROLE_UI_OPACITY  (1u << 13)   /* 子树透明度 */
#define MQ_ROLE_UI_SAFEAREA (1u << 14)   /* 安全区（内缩 + 目标尺寸） */
#define MQ_ROLE_UI_SCROLL   (1u << 15)   /* 滚动视图（内容偏移 + 裁剪范围） */
#define MQ_ROLE_PHYSICS_BODY (1u << 16)  /* 2D 刚体：装配期入物理世界，帧内回写变换 */
#define MQ_ROLE_PHYSICS_COLLIDER (1u << 17) /* 2D 碰撞体：形状来自 Collider 组件 */
#define MQ_ROLE_UI_TEXT     (1u << 18)   /* 文本：Label/RichText（排版 + 字形渲染） */
#define MQ_ROLE_UI_CANVAS   (1u << 19)   /* 画布：UI 根 + 分辨率策略（设计分辨率 → 设备像素） */

/* 枚举字面量（供 JSON 文本按名字读写、AI 自省；docs/06） */
typedef struct mq_enum_value {
    const char* name;    /* 短名（如 "Vertical"；写入也接受 "Type::Vertical"） */
    int64_t     value;
} mq_enum_value;

typedef struct mq_type_desc {
    const char*          name;   /* fully qualified ns.Name */
    uint32_t             type_id;
    uint32_t             size;
    uint32_t             align;
    uint32_t             field_count;
    const mq_field_desc* fields;
    uint32_t             flags;  /* bit0 is_enum, bit1 is_pod, bit2 has_dynamic */
    uint32_t             role;   /* MQ_ROLE_* 位标志 */
    const mq_enum_value* values;      /* 枚举值表（仅 is_enum） */
    uint32_t             value_count; /* 枚举值个数 */
    /* 默认构造：placement-new 生成的 C++ 类型（应用 schema 默认值 + 正确构造 string/vector）。
     * 组件/资源实例必须先调用它，否则字段是零字节而非 schema 默认值（docs/06）。 */
    void               (*init_defaults)(void* obj);
} mq_type_desc;

/* registry (lookup by type_id / name) */
extern const mq_type_desc* const mq_schema_types[];
extern const uint32_t            mq_schema_type_count;

#ifdef __cplusplus
}
#endif
`
}

const ROLE_BIT = {
  transform2d: 'MQ_ROLE_TRANSFORM2D',
  sprite: 'MQ_ROLE_SPRITE',
  ui_button: 'MQ_ROLE_UI_BUTTON',
  ui_label: 'MQ_ROLE_UI_LABEL',
  camera: 'MQ_ROLE_CAMERA',
  anim_player: 'MQ_ROLE_ANIM_PLAYER',
  particle_emitter: 'MQ_ROLE_PARTICLE',
  nav_agent: 'MQ_ROLE_NAV_AGENT',
  ui_control: 'MQ_ROLE_UI_CONTROL',
  ui_layout: 'MQ_ROLE_UI_LAYOUT',
  ui_progress: 'MQ_ROLE_UI_PROGRESS',
  ui_slider: 'MQ_ROLE_UI_SLIDER',
  ui_check: 'MQ_ROLE_UI_CHECK',
  ui_opacity: 'MQ_ROLE_UI_OPACITY',
  ui_safe_area: 'MQ_ROLE_UI_SAFEAREA',
  ui_scroll: 'MQ_ROLE_UI_SCROLL',
  physics_body: 'MQ_ROLE_PHYSICS_BODY',
  physics_collider: 'MQ_ROLE_PHYSICS_COLLIDER',
  ui_text: 'MQ_ROLE_UI_TEXT',
  ui_canvas: 'MQ_ROLE_UI_CANVAS',
}
function roleBits(t) {
  const parts = (t.roles || []).map((r) => ROLE_BIT[r]).filter(Boolean)
  return parts.length ? parts.join(' | ') : '0u'
}

function genDescSource(types) {
  const lines = []
  lines.push('// Generated by parser.js -- DO NOT EDIT')
  lines.push('// C ABI reflection descriptors + registry (SCHEMA.md v1 §6)')
  lines.push('//')
  lines.push('// Compiled as C++ (it includes maque_types.h for offsetof/sizeof), but every')
  lines.push('// exported symbol keeps C linkage (extern "C") so the descriptor table is a')
  lines.push('// stable C ABI consumable across .so / processes / languages.')
  lines.push('')
  lines.push('#include "maque_desc.h"')
  lines.push('#include "maque_types.h"')
  lines.push('#include <cstddef>')
  lines.push('#include <new>')
  lines.push('')
  // offsetof on non-standard-layout (container-bearing) types is intentional:
  // those fields are flagged has_dynamic and only ever walked by the visitor.
  lines.push('#if defined(__GNUC__) || defined(__clang__)')
  lines.push('#pragma GCC diagnostic ignored "-Winvalid-offsetof"')
  lines.push('#endif')
  lines.push('')
  lines.push('#ifdef __cplusplus')
  lines.push('extern "C" {')
  lines.push('#endif')
  lines.push('')

  // 动态数组（T[]）的容器算子元素：具名 struct，或字符串（元素类型由哨兵表示）
  const dynElem = (node) => {
    if (!node || node.cat !== 'array_dyn' || !node.elem) return null
    if (node.elem.cat === 'string') return { sentinel: true, fqName: 'std::string', typeId: 0 }
    if (node.elem.cat === 'named' && node.elem.resolved) return node.elem.resolved
    return null
  }

  for (const t of types) {
    if (t.kind !== 'struct' || t.fields.length === 0) continue
    const sym = `mq_fields_${mangle(t.fqName)}`
    for (const f of t.fields) {
      const de = dynElem(f.typeNode)
      if (!de) continue
      const base = `mq_dyn_${mangle(t.fqName)}_${mangle(f.name)}`
      const elemCpp = de.sentinel ? 'std::string' : de.fqName.replace(/\./g, '::')
      lines.push(`static void ${base}_clear(void* f) { static_cast<std::vector<${elemCpp}>*>(f)->clear(); }`)
      lines.push(`static uint32_t ${base}_size(const void* f) { return (uint32_t)static_cast<const std::vector<${elemCpp}>*>(f)->size(); }`)
      lines.push(`static void ${base}_resize(void* f, uint32_t n) { static_cast<std::vector<${elemCpp}>*>(f)->resize(n); }`)
      lines.push(`static void* ${base}_elem(void* f, uint32_t i) { return &static_cast<std::vector<${elemCpp}>*>(f)->at(i); }`)
    }
    lines.push(`static const mq_field_desc ${sym}[] = {`)
    for (const f of t.fields) {
      const kind = fieldKind(f.typeNode)
      const typeId = fieldTypeId(f.typeNode)
      const count = fieldCount(f.typeNode)
      const attrs = attrsFlags(f.attrs)
      const de = dynElem(f.typeNode)
      let extra
      if (de) {
        const base = `mq_dyn_${mangle(t.fqName)}_${mangle(f.name)}`
        const elemId = de.sentinel ? 'MQ_ELEM_TYPE_STRING' : `${de.typeId}u`
        extra = `${elemId}, ${base}_clear, ${base}_size, ${base}_resize, ${base}_elem`
      } else {
        extra = `0u, 0, 0, 0, 0`
      }
      lines.push(
        `    { "${f.name}", (uint32_t)offsetof(${t.fqName.replace(/\./g, '::')}, ${f.name}), ` +
        `MQ_TK_${kind}, ${typeId}u, ${count}u, ${attrs}u, ${extra} },`
      )
    }
    lines.push('};')
    lines.push('')
  }

  /* 枚举值表：JSON 文本可按名字读写枚举（"Vertical" 或 "ContainerMode::Vertical"） */
  for (const t of types) {
    if (t.kind !== 'enum') continue
    lines.push(`static const mq_enum_value mq_values_${mangle(t.fqName)}[] = {`)
    for (const v of t.values) lines.push(`    { "${v.name}", (int64_t)${v.value} },`)
    lines.push('};')
    lines.push('')
  }

  /* 默认构造器：直接调用生成类型的 C++ 默认构造（字段默认值 + 动态成员构造一次到位） */
  for (const t of types) {
    const cpp = t.fqName.replace(/\./g, '::')
    lines.push(`static void mq_defaults_${mangle(t.fqName)}(void* p) { new (p) ${cpp}{}; }`)
  }
  lines.push('')

  for (const t of types) {
    const cpp = t.fqName.replace(/\./g, '::')
    const fieldsSym = t.kind === 'struct' && t.fields.length > 0
      ? `mq_fields_${mangle(t.fqName)}`
      : '0'
    const valuesSym = t.kind === 'enum' ? `mq_values_${mangle(t.fqName)}` : '0'
    const valueCount = t.kind === 'enum' ? t.values.length : 0
    lines.push(`extern const mq_type_desc mq_type_${mangle(t.fqName)} = {`)
    lines.push(`    "${t.fqName}",`)
    lines.push(`    ${t.typeId}u,`)
    lines.push(`    (uint32_t)sizeof(${cpp}),`)
    lines.push(`    (uint32_t)alignof(${cpp}),`)
    lines.push(`    ${t.kind === 'struct' ? t.fields.length : 0}u,`)
    lines.push(`    ${fieldsSym},`)
    lines.push(`    ${typeFlags(t)}u,`)
    lines.push(`    ${roleBits(t)},`)
    lines.push(`    ${valuesSym},`)
    lines.push(`    ${valueCount}u,`)
    lines.push(`    mq_defaults_${mangle(t.fqName)},`)
    lines.push('};')
    lines.push('')
  }

  lines.push('const mq_type_desc* const mq_schema_types[] = {')
  for (const t of types) {
    lines.push(`    &mq_type_${mangle(t.fqName)},`)
  }
  lines.push('};')
  lines.push('')
  lines.push(`const uint32_t mq_schema_type_count = ${types.length}u;`)
  lines.push('')
  lines.push('#ifdef __cplusplus')
  lines.push('} // extern "C"')
  lines.push('#endif')
  lines.push('')

  return lines.join('\n')
}

// ---------------------------------------------------------------------------
// JSON rendering
// ---------------------------------------------------------------------------

function jsonField(f) {
  const node = f.typeNode
  const attrs = {}
  for (const key of Object.keys(f.attrs).sort()) attrs[key] = f.attrs[key]
  return {
    name: f.name,
    type: f.typeText,
    kind: fieldKind(node).toLowerCase(),
    type_id: fieldTypeId(node),
    count: fieldCount(node),
    offset: f._jsonOffset === undefined ? null : f._jsonOffset,
    attrs,
  }
}

function computeJsonOffsets(t) {
  for (const f of t.fields) f._jsonOffset = null
  if (!typePod(t)) return
  let offset = 0
  for (const f of t.fields) {
    const s = nodeSize(f.typeNode)
    const a = nodeAlign(f.typeNode)
    if (s === null || a === null || a <= 0) return
    offset = alignUp(offset, a)
    f._jsonOffset = offset
    offset += s
  }
}

function genJson(namespaces, tree) {
  const types = sortedTypes(namespaces).map((t) => {
    computeJsonOffsets(t)
    const entry = {
      name: t.fqName,
      namespace: t.ns,
      kind: t.kind,
      type_id: t.typeId,
      size: t.size !== null ? t.size : typeSize(t),
      align: t.align !== null ? t.align : (typePod(t) ? typeAlign(t) : null),
      pod: typePod(t),
      has_dynamic: typeHasDynamic(t),
    }
    if (t.source) entry.source = t.source
    if (t.roles && t.roles.length) entry.roles = t.roles
    if (t.kind === 'enum') {
      entry.underlying = t.underlying
      entry.values = t.values.map((v) => ({ name: v.name, value: v.value }))
      entry.fields = []
    } else if (t.kind === 'alias') {
      entry.alias_of = t.aliasText
      entry.fields = []
    } else {
      entry.fields = t.fields.map(jsonField)
    }
    return entry
  })

  /* 函数/控制面/回调/模块 ABI 也进 JSON：AI 消费同一份声明（docs/11/21 §6.3）。 */
  const apis = []
  const controls = []
  const callbacks = []
  const modules = []
  for (const ns of namespaces) {
    for (const a of ns.apis || []) {
      apis.push({
        name: `${ns.name}.${a.name}`, namespace: ns.name, fn: a.name,
        args: a.args, returns: a.returns, ctx: a.ctx,
        manual: a.manual ? String(a.manual) : null,
      })
    }
    for (const c of ns.controls || []) {
      controls.push({ method: c.method, namespace: ns.name, args: c.args })
    }
    for (const cb of ns.callbacks || []) {
      callbacks.push({ name: cb.name, args: cb.args, on: cb.on })
    }
    const ma = ns.moduleAbi
    if (ma) {
      modules.push({
        module: ma.module, abi_name: ma.abiName, version: ma.version,
        language: ma.language, header: `abi/${ma.module}.h`,
        fns: ma.fns.map((f) => ({ name: f.name, returns: f.returns, args: f.args })),
        structs: ma.structs.map((st) => ({ name: st.name, fields: st.fields })),
        enums: ma.enums.map((e) => ({ name: e.name, values: e.values })),
      })
    }
  }
  const doc = {
    schema_version: 1,
    generated_by: 'parser.js',
    type_count: types.length,
    api_count: apis.length,
    control_count: controls.length,
    callback_count: callbacks.length,
    module_count: modules.length,
    types,
    apis,
    controls,
    callbacks,
    modules,
  }
  /* schema 目录布局：哪个文件声明了哪个 namespace / 依赖谁（AI/工具据此定位「改哪个文件」）。 */
  if (tree) {
    doc.files = tree.files.map((f) => ({
      path: f.rel,
      namespaces: namespaces.filter((ns) => ns.source === f.rel).map((ns) => ns.name),
      imports: f.imports.map((d) => d.rel),
    }))
  }
  return JSON.stringify(doc, null, 2) + '\n'
}

// ---------------------------------------------------------------------------
// schema 目录与 import（SCHEMA.md §1）
//
// 入口 = 一个 YAML 文件（`--schema <目录>` → 目录内 schema.yaml；也接受单个文件）。
// 文件的顶层条目是 namespace / interface 声明，或 import 指令：
//     - import: <路径>            # 单个
//     - imports: [<路径>, …]      # 多个
//
// 语义：
//   · import 路径相对**当前文件**，必须落在 schema 根目录内且存在；
//   · 合并顺序 = 从入口出发的**后序遍历**（被 import 的文件先于本文件、按声明顺序、去重）
//     → 全局顺序完全由 import 图决定，确定且可复现（docs/06 §4）；
//   · import 图必须**无环**；目录内未被入口（间接）import 的文件 = 错误（防死文件）；
//   · 同一 namespace 可由多个文件共同声明（版本必须一致；module_abi / global 只能声明一次）；
//   · 结构依赖强校验：字段类型引用的类型，其声明文件必须在引用文件的 import 闭包内。
// ---------------------------------------------------------------------------

const SCHEMA_ENTRY_NAME = 'schema.yaml'
const SCHEMA_FILE_RE = /\.ya?ml$/i

function isEntryObject(v) {
  return v !== null && typeof v === 'object' && !Array.isArray(v)
}

function isImportEntry(e) {
  return isEntryObject(e) && (e.import !== undefined || e.imports !== undefined)
}

function importPathsOf(entry, rel) {
  const out = []
  const push = (v) => {
    if (typeof v !== 'string' || !v.trim()) {
      throw new SchemaError(`${rel}: import 路径必须是非空字符串`)
    }
    out.push(v.trim())
  }
  if (entry.import !== undefined) push(entry.import)
  if (entry.imports !== undefined) {
    if (!Array.isArray(entry.imports)) throw new SchemaError(`${rel}: imports 必须是路径数组`)
    for (const p of entry.imports) push(p)
  }
  return out
}

/* 读取 schema 树：入口 + import 闭包。files 按合并顺序（依赖在前）排列。 */
function loadSchema(schemaArg) {
  const abs = path.resolve(schemaArg)
  let stat = null
  try { stat = fs.statSync(abs) } catch (_) { stat = null }
  if (!stat) throw new SchemaError(`schema 不存在: ${abs}`)
  const dirMode = stat.isDirectory()
  const entryPath = dirMode ? path.join(abs, SCHEMA_ENTRY_NAME) : abs
  if (dirMode && !fs.existsSync(entryPath)) {
    throw new SchemaError(`schema 目录缺少入口文件 ${SCHEMA_ENTRY_NAME}: ${abs}`)
  }
  const rootDir = path.dirname(entryPath)
  const relOf = (p) => path.relative(rootDir, p).split(path.sep).join('/')

  const files = []
  const byPath = new Map()
  const stack = []

  const visit = (file) => {
    const canonical = path.resolve(file)
    const cycleAt = stack.indexOf(canonical)
    if (cycleAt >= 0) {
      throw new SchemaError(`import 成环: ${stack.slice(cycleAt).concat(canonical).map(relOf).join(' → ')}`)
    }
    if (byPath.has(canonical)) return byPath.get(canonical)
    if (!fs.existsSync(canonical)) {
      throw new SchemaError(`import 的文件不存在: ${relOf(canonical)}`)
    }
    if (fs.statSync(canonical).isDirectory()) {
      throw new SchemaError(`import 目标是目录（应为文件）: ${relOf(canonical)}`)
    }

    const raw = fs.readFileSync(canonical, 'utf8')
    let doc
    try {
      doc = parseYamlDocument(raw)
    } catch (err) {
      throw new SchemaError(`${relOf(canonical)}: YAML 解析失败: ${err.message}`)
    }
    if (doc === null || doc === undefined) doc = []
    if (!Array.isArray(doc)) {
      throw new SchemaError(`${relOf(canonical)}: 顶层必须是条目数组（namespace / interface / import）`)
    }

    const rec = { path: canonical, rel: relOf(canonical), raw, imports: [], entries: [] }
    byPath.set(canonical, rec)
    stack.push(canonical)
    // import 一律先于本文件条目合并（与它在文件中的书写位置无关）
    for (const e of doc) {
      if (!isImportEntry(e)) continue
      for (const p of importPathsOf(e, rec.rel)) {
        const target = path.resolve(path.dirname(canonical), p)
        const outside = path.relative(rootDir, target)
        if (outside.startsWith('..') || path.isAbsolute(outside)) {
          throw new SchemaError(`${rec.rel}: import "${p}" 越出 schema 根目录`)
        }
        rec.imports.push(visit(target))
      }
    }
    for (const e of doc) {
      if (isImportEntry(e)) continue
      if (!isEntryObject(e)) {
        throw new SchemaError(`${rec.rel}: 顶层条目必须是 namespace / interface / import 映射`)
      }
      rec.entries.push(e)
    }
    stack.pop()
    files.push(rec)   // 后序：依赖在前
    return rec
  }

  visit(entryPath)
  return { entryPath, rootDir, dirMode, files, entries: files.flatMap((f) => f.entries) }
}

/* 目录内全部 schema 文件（供「未被入口覆盖」检查；排序保证报错顺序确定）。 */
function scanSchemaFiles(rootDir) {
  const skipDirs = new Set(['generated', 'node_modules'])
  const out = []
  const walk = (dir) => {
    let entries = []
    try { entries = fs.readdirSync(dir, { withFileTypes: true }) } catch (_) { return }
    for (const e of entries) {
      if (e.name.startsWith('.')) continue
      const p = path.join(dir, e.name)
      if (e.isDirectory()) {
        if (!skipDirs.has(e.name)) walk(p)
      } else if (SCHEMA_FILE_RE.test(e.name)) {
        out.push(path.resolve(p))
      }
    }
  }
  walk(rootDir)
  return out.sort()
}

/* 解析合并条目 → namespace 列表（按首次出现定序），并标注每个类型/命名空间的来源文件。 */
function parseSchemaTree(tree) {
  const namespaces = []
  const byName = new Map()
  for (const file of tree.files) {
    const seen = new Set()
    for (const entry of file.entries) {
      if (entry.namespace !== undefined) {
        const name = String(entry.namespace)
        if (seen.has(name)) {
          throw new SchemaError(`${file.rel}: namespace "${name}" 在同一文件内重复声明`)
        }
        seen.add(name)
      }
      for (const ns of parseSchemaDocument([entry])) {
        if (ns.source === undefined) ns.source = file.rel
        for (const t of ns.types) if (t.source === undefined) t.source = file.rel
        const prev = byName.get(ns.name)
        if (!prev) {
          byName.set(ns.name, ns)
          namespaces.push(ns)
          continue
        }
        // 跨文件扩展同一 namespace
        if (prev.version !== ns.version) {
          throw new SchemaError(
            `namespace "${ns.name}" 版本不一致: ${prev.source} v${prev.version} vs ${file.rel} v${ns.version}`)
        }
        if (prev.global !== ns.global) {
          throw new SchemaError(`namespace "${ns.name}": global 只能声明一次 (${prev.source} / ${file.rel})`)
        }
        if (prev.moduleAbi && ns.moduleAbi) {
          throw new SchemaError(`namespace "${ns.name}": module_abi 只能声明一次 (${prev.source} / ${file.rel})`)
        }
        if (ns.moduleAbi !== null) prev.moduleAbi = ns.moduleAbi
        prev.types.push(...ns.types)
        prev.apis.push(...ns.apis)
        prev.controls.push(...ns.controls)
        prev.callbacks.push(...ns.callbacks)
      }
    }
  }
  return namespaces
}

/* import 校验：孤儿文件 / 结构依赖覆盖 / 未使用 import。须在 resolveTypes 之后调用。 */
function checkSchemaImports(tree, namespaces) {
  const errors = []
  const warnings = []
  const byRel = new Map(tree.files.map((f) => [f.rel, f]))

  const reachable = new Map()
  const closureOf = (file) => {
    let set = reachable.get(file.rel)
    if (set) return set
    set = new Set([file.rel])
    reachable.set(file.rel, set)   // 先登记：import 图已保证无环
    for (const dep of file.imports) for (const r of closureOf(dep)) set.add(r)
    return set
  }
  for (const f of tree.files) closureOf(f)

  // 1. 目录内未被入口（经 import）覆盖的文件 = 死文件
  if (tree.dirMode) {
    const seen = new Set(tree.files.map((f) => f.rel))
    for (const p of scanSchemaFiles(tree.rootDir)) {
      const rel = path.relative(tree.rootDir, p).split(path.sep).join('/')
      if (!seen.has(rel)) {
        errors.push(`schema 文件未被 import 覆盖: ${rel}（在 ${SCHEMA_ENTRY_NAME} 中登记，或由某个文件 import）`)
      }
    }
  }

  // 2. 结构依赖：字段类型引用的类型必须来自本文件或 import 闭包
  for (const ns of namespaces) {
    for (const t of ns.types) {
      const closure = t.source ? closureOf(byRel.get(t.source)) : null
      for (const dep of typeDeps(t)) {
        if (!dep || dep.ns === ns.name || !dep.source) continue
        if (dep.source === t.source || !closure) continue
        if (closure.has(dep.source)) continue
        const hint = path.relative(path.dirname(t.source), dep.source).split(path.sep).join('/')
        const importPath = hint.startsWith('.') ? hint : `./${hint}`
        errors.push(
          `${t.source}: ${t.fqName} 引用 ${dep.fqName}（${dep.source}），但该文件不在 import 闭包内` +
          ` —— 在 ${t.source} 的 imports 中加入 "${importPath}"`)
      }
    }
  }

  // 3. 未使用的 import（保守启发：闭包内的类型名一个都没在本文件出现）
  const namesByFile = new Map()
  for (const ns of namespaces) {
    for (const t of ns.types) {
      if (!namesByFile.has(t.source)) namesByFile.set(t.source, new Set())
      namesByFile.get(t.source).add(t.name)
    }
  }
  for (const file of tree.files) {
    if (path.resolve(file.path) === path.resolve(tree.entryPath)) continue   // 入口 = 清单，import 即其内容
    for (const dep of file.imports) {
      const names = new Set()
      for (const rel of closureOf(dep)) for (const n of (namesByFile.get(rel) || [])) names.add(n)
      const used = [...names].some((n) => new RegExp(`\\b${n}\\b`).test(file.raw))
      if (!used) {
        warnings.push(`${file.rel}: import "${dep.rel}" 未被使用（其闭包内没有任何类型名出现在本文件）`)
      }
    }
  }
  return { errors, warnings }
}

/* schema 文件表（--check 输出：文件 → namespace + 依赖）。 */
function schemaFileSummary(tree, namespaces) {
  const nsOfFile = new Map()
  for (const ns of namespaces) {
    if (!nsOfFile.has(ns.source)) nsOfFile.set(ns.source, [])
    nsOfFile.get(ns.source).push(ns.name)
  }
  const width = Math.max(0, ...tree.files.map((f) => f.rel.length))
  return tree.files.map((f) => {
    const nss = (nsOfFile.get(f.rel) || []).join(', ') || '—'
    const deps = f.imports.length ? `  ← ${f.imports.map((d) => d.rel).join(', ')}` : ''
    return `  ${f.rel.padEnd(width)}  ${nss}${deps}`
  })
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

function parseArgs(argv) {
  const args = { schema: null, out: null, check: false }
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i]
    if (a === '--check') {
      args.check = true
    } else if (a === '--schema') {
      args.schema = argv[++i]
    } else if (a.startsWith('--schema=')) {
      args.schema = a.slice('--schema='.length)
    } else if (a === '--out') {
      args.out = argv[++i]
    } else if (a.startsWith('--out=')) {
      args.out = a.slice('--out='.length)
    } else if (a === '-h' || a === '--help') {
      args.help = true
    } else {
      throw new SchemaError(`unknown argument "${a}"`)
    }
  }
  return args
}

function printHelp() {
  console.log('usage: node parser.js [--schema schema/ | <file>.yaml] [--out generated] [--check]')
  console.log('  --schema <dir|file>  schema 目录（内含 schema.yaml 入口，默认 schema/）或单文件')
  console.log('  --out <dir>          输出目录（默认 schema 同级的 generated/）')
  console.log('  --check              只校验（import / 引用 / 布局 / 尺寸），不写文件')
}

// ---- 接口 DSL（插件 ABI）：从声明生成 C vtable（docs/02, docs/21）----
function parseInterfaces(root) {
  const out = []
  const C_SCALARS = new Set(['float', 'double', 'bool', 'int8_t', 'uint8_t',
    'int16_t', 'uint16_t', 'int32_t', 'uint32_t', 'int64_t', 'uint64_t'])
  for (const e of root) {
    if (!e || e.interface === undefined) continue
    const records = (e.records || []).map((r) => ({
      name: String(r.record),
      fields: (r.fields || []).map((f) => ({ name: String(f.name), type: String(f.type) })),
    }))
    const methods = (e.methods || []).map((m) => ({
      fn: String(m.fn),
      args: (m.args || []).map((a) => ({ name: String(a.name), type: String(a.type) })),
      returns: m.returns === undefined ? 'void' : String(m.returns),
    }))
    out.push({ name: String(e.interface), version: Number(e.version || 1), records, methods, scalars: C_SCALARS })
  }
  return out
}
function ifaceCType(t, iface, isSelf) {
  if (t === 'self') return 'void*'
  if (iface.scalars.has(t)) return t
  return t   // 透传 C 类型（如 const uint8_t*）
}
function genInterfacesHeader(interfaces) {
  const lines = []
  lines.push('// Generated by parser.js -- DO NOT EDIT')
  lines.push('// Plugin interface ABI (C vtables) from schema interfaces.')
  lines.push('#pragma once')
  lines.push('#include <stdint.h>')
  lines.push('#include <stddef.h>')
  lines.push('#include "core/module/mq_module.h"   // mq_api_header')
  lines.push('')
  for (const it of interfaces) {
    const V = it.version
    for (const r of it.records) {
      lines.push(`typedef struct mq_${it.name}_${r.name} {`)
      for (const f of r.fields) lines.push(`    ${ifaceCType(f.type, it)} ${f.name};`)
      lines.push(`} mq_${it.name}_${r.name};`)
      lines.push('')
    }
    lines.push(`#define MQ_${it.name.toUpperCase()}_API_NAME "maque.${it.name}"`)
    lines.push(`typedef struct mq_${it.name}_v${V} {`)
    lines.push('    mq_api_header header;')
    for (const m of it.methods) {
      const ret = m.returns === 'void' ? 'void' : ifaceCType(m.returns, it)
      const ps = m.args.map((a) => `${ifaceCType(a.type, it)} ${a.name}`).join(', ')
      lines.push(`    ${ret} (*${m.fn})(${ps});`)
    }
    lines.push(`} mq_${it.name}_v${V};`)
    lines.push('')
  }
  return lines.join('\n')
}

// 函数接口：标量类型映射到 C++ / 数值编组。
const API_SCALARS = new Set(['float', 'double', 'bool', 'int8_t', 'uint8_t',
  'int16_t', 'uint16_t', 'int32_t', 'uint32_t', 'int64_t', 'uint64_t'])
function apiCppType(t) {
  if (API_SCALARS.has(t)) return t
  if (t === 'string') return 'const char*'
  return null
}
function apiNumeric(t) { return t === 'void' || API_SCALARS.has(t) }
function apiCast(t, expr) {
  if (t === 'bool') return `(${expr} != 0.0)`
  if (t === 'float') return `(float)${expr}`
  if (t === 'double') return expr
  return `(${t})${expr}`
}

function genApiHeader(namespaces) {
  const lines = []
  lines.push('// Generated by parser.js -- DO NOT EDIT')
  lines.push('// Engine API declarations (from schema/ api_list)')
  lines.push('')
  lines.push('#pragma once')
  lines.push('')
  // MANUAL：收集需要在头文件中内联实现的外部头
  const manualIncludes = new Set()
  for (const ns of namespaces)
    for (const fn of ns.apis || []) if (fn.manual) manualIncludes.add(fn.manual)
  for (const inc of manualIncludes) lines.push(`#include "${inc}"`)
  if (manualIncludes.size) lines.push('')
  for (const ns of namespaces) {
    if (!ns.apis || ns.apis.length === 0) continue
    const declared = ns.apis.filter((fn) => {
      if (fn.manual) return false                     // 由 manual 头提供声明+实现
      const rt = fn.returns === 'void' ? 'void' : apiCppType(fn.returns)
      return rt !== null && fn.args.every((a) => apiCppType(a.type) !== null)
    })
    if (declared.length === 0) continue
    lines.push(`namespace ${ns.name} {`)
    for (const fn of declared) {
      const ps = []
      if (fn.ctx) ps.push('void* ctx')
      for (const a of fn.args) ps.push(`${apiCppType(a.type)} ${a.name}`)
      const ret = fn.returns === 'void' ? 'void' : apiCppType(fn.returns)
      lines.push(`  ${ret} ${fn.name}(${ps.join(', ')});`)
    }
    lines.push('}')
    lines.push('')
  }
  return lines.join('\n')
}

function genBindingsSource(namespaces) {
  const lines = []
  lines.push('// Generated by parser.js -- DO NOT EDIT')
  lines.push('// Script bindings for engine APIs declared in schema/ api_list.')
  lines.push('#include "maque_api.h"')
  lines.push('#include "core/script/mq_script.h"')
  lines.push('#include <cstdint>')
  lines.push('#include <cstdio>')
  lines.push('')
  lines.push('namespace {')

  // 分类：str 绑定（单个 string 参数）；num 绑定（全标量）
  const fns = []
  for (const ns of namespaces) {
    for (const fn of ns.apis || []) {
      const args = fn.args || []
      const strBinding = args.length === 1 && args[0].type === 'string'
      // 数值 + 单个尾部字符串（如 set_label(id, text)）也支持：字符串作为 s 参数传入
      const strArg = args.filter((a) => a.type === 'string')
      const mixedBinding = !strBinding && strArg.length === 1 &&
                           args[args.length - 1].type === 'string' &&
                           args.every((a) => a.type === 'string' || API_SCALARS.has(a.type))
      const numBinding = args.every((a) => API_SCALARS.has(a.type))
      const retStr = fn.returns === 'string'
      /* multi：任意数量/位置的 string（如 node_set_number(id, comp, field, value)）。
       * 生成 'n'/'s' 类型串 → mq_script_register_multi 通用编组（docs/21 §6.1）。 */
      const general = args.every((a) => a.type === 'string' || API_SCALARS.has(a.type))
      /* 其余任意组合（含"单个 string 但不在末尾"，如 tween_start(id, field, to, dur)）
       * 一律走通用编组：不再静默跳过（docs/21 §6.1）。 */
      const multiBinding = general && !numBinding && !strBinding && !mixedBinding
      /* 字符串返回：数值参数走 num/ret_str；含字符串参数时走 multi_ret_str（此前被静默丢弃） */
      if (retStr && !numBinding && !multiBinding) continue
      if (!strBinding && !numBinding && !mixedBinding && !multiBinding) continue
      fns.push({ ns: ns.name, global: !!ns.global, fn, strBinding, mixedBinding, multiBinding, retStr })
    }
  }
  for (const { ns, fn, strBinding, mixedBinding, multiBinding } of fns) {
    const sym = `b_${ns}_${fn.name}`
    if (strBinding) {
      lines.push(`int ${sym}(void* user, const char* s, double* ret) {`)
      const args = fn.ctx ? 'user, s' : 's'
      const call = `${ns}::${fn.name}(${args})`
      if (fn.returns === 'void') lines.push(`  ${call}; return 0;`)
      else lines.push(`  *ret = (double)${call}; return 0;`)
      lines.push('}')
    } else if (mixedBinding) {
      const parts = []
      if (fn.ctx) parts.push('user')
      let ai = 0
      for (const a of fn.args) parts.push(a.type === 'string' ? 's' : apiCast(a.type, `a[${ai++}]`))
      lines.push(`int ${sym}(void* user, int, const double* a, const char* s, double* ret) {`)
      const call = `${ns}::${fn.name}(${parts.join(', ')})`
      if (fn.returns === 'void') lines.push(`  ${call}; return 0;`)
      else lines.push(`  *ret = (double)${call}; return 0;`)
      lines.push('}')
    } else if (multiBinding) {
      const kinds = fn.args.map((a) => (a.type === 'string' ? 's' : 'n')).join('')
      const parts = []
      if (fn.ctx) parts.push('user')
      let ni = 0, si = 0
      for (const a of fn.args) {
        parts.push(a.type === 'string' ? `s[${si++}]` : apiCast(a.type, `a[${ni++}]`))
      }
      const call = `${ns}::${fn.name}(${parts.join(', ')})`
      const retStrMulti = fn.returns === 'string'
      if (retStrMulti) {
        lines.push(`int ${sym}(void* user, int, const double* a, int, const char** s, char* ret, int cap) {`)
        lines.push(`  std::snprintf(ret, (size_t)cap, "%s", ${call}); return 0;`)
      } else {
        lines.push(`int ${sym}(void* user, int, const double* a, int, const char** s, double* ret) {`)
        if (fn.returns === 'void') lines.push(`  ${call}; return 0;`)
        else lines.push(`  *ret = (double)${call}; return 0;`)
      }
      lines.push(`}  /* kinds=${kinds} */`)
    } else if (fn.returns === 'string') {
      const parts = []
      if (fn.ctx) parts.push('user')
      fn.args.forEach((a, i) => parts.push(apiCast(a.type, `a[${i}]`)))
      lines.push(`int ${sym}(void* user, int, const double* a, char* ret, int cap) {`)
      lines.push(`  std::snprintf(ret, (size_t)cap, "%s", ${ns}::${fn.name}(${parts.join(', ')})); return 0;`)
      lines.push('}')
    } else {
      const parts = []
      if (fn.ctx) parts.push('user')
      fn.args.forEach((a, i) => parts.push(apiCast(a.type, `a[${i}]`)))
      lines.push(`int ${sym}(void* user, int, const double* a, double* ret) {`)
      const call = `${ns}::${fn.name}(${parts.join(', ')})`
      if (fn.returns === 'void') lines.push(`  ${call}; return 0;`)
      else lines.push(`  *ret = (double)${call}; return 0;`)
      lines.push('}')
    }
  }
  lines.push('} // namespace')
  lines.push('')
  lines.push('extern "C" void mq_bind_generated(mq_script_vm* vm, void* ctx) {')
  for (const { ns, global, fn, strBinding, mixedBinding, multiBinding } of fns) {
    const sym = `b_${ns}_${fn.name}`
    const user = fn.ctx ? 'ctx' : 'nullptr'
    const kinds = multiBinding ? fn.args.map((a) => (a.type === 'string' ? 's' : 'n')).join('') : null
    const reg = strBinding ? 'mq_script_register_str'
              : mixedBinding ? 'mq_script_register_mixed'
              : multiBinding ? (fn.returns === 'string' ? 'mq_script_register_multi_ret_str'
                                                        : 'mq_script_register_multi')
              : (fn.returns === 'string' ? 'mq_script_register_ret_str' : 'mq_script_register_num')
    const jsName = global ? fn.name : `${ns}.${fn.name}`
    lines.push(kinds ? `  ${reg}(vm, "${jsName}", "${kinds}", ${sym}, ${user});`
                     : `  ${reg}(vm, "${jsName}", ${sym}, ${user});`)
  }
  lines.push('}')
  lines.push('')
  return lines.join('\n')
}

/* 控制平面方法：声明头（引擎实现语义）+ 注册表（替代手写 mq_control_register）
 * + 类型化 MCP inputSchema（docs/11 §3、docs/21 §6.3）。 */
function controlSym(method) {
  return `mq_control_${String(method).replace(/[^A-Za-z0-9]/g, '_')}`
}
function jsonTypeFor(t) {
  switch (t) {
    case 'float': case 'double': case 'int8_t': case 'uint8_t': case 'int16_t': case 'uint16_t':
    case 'int32_t': case 'uint32_t': case 'int64_t': case 'uint64_t':
      return 'number'
    case 'bool': return 'boolean'
    case 'string': return 'string'
    case 'json': return 'object'
    default: return 'string'
  }
}
/* 绑定入口契约（docs/21 §3）：mq_bind_generated 的声明由生成器拥有，
 * 避免手写头与生成绑定签名漂移。 */
function genBindHeader() {
  return [
    '// Generated by parser.js -- DO NOT EDIT',
    '// Script binding entry point (from schema/ api_list)',
    '#pragma once',
    '',
    '#include "core/script/mq_script.h"',
    '',
    '#ifdef __cplusplus',
    'extern "C" {',
    '#endif',
    '',
    '/* 把 api_list 声明的全部 <ns>.<fn> 注册进脚本 VM；ctx 为宿主实例上下文。 */',
    'void mq_bind_generated(mq_script_vm* vm, void* ctx);',
    '',
    '#ifdef __cplusplus',
    '}',
    '#endif',
    '',
  ].join('\n')
}

function genControlHeader(namespaces) {
  const lines = []
  lines.push('// Generated by parser.js -- DO NOT EDIT')
  lines.push('// Control-plane method handlers (from schema/ control_list)')
  lines.push('//')
  lines.push('// 契约（docs/11 §3、docs/21）：方法名/参数/返回形态只在此声明一次；')
  lines.push('// 引擎只实现 mq_control_<method> 的语义，注册与 MCP schema 由生成代码完成。')
  lines.push('#pragma once')
  lines.push('')
  lines.push('#include <stddef.h>')
  lines.push('#include "core/control/mq_control.h"')
  lines.push('')
  lines.push('#ifdef __cplusplus')
  lines.push('extern "C" {')
  lines.push('#endif')
  lines.push('')
  for (const ns of namespaces) {
    for (const c of ns.controls || []) {
      lines.push(`/* ${c.method}(${c.args.map((a) => a.name + ': ' + a.type).join(', ')}) */`)
      lines.push(`int ${controlSym(c.method)}(void* user, const mq_json* params, char* out, size_t out_cap);`)
    }
  }
  lines.push('')
  lines.push('/* 注册全部控制面方法（runtime 调用；不再手写 mq_control_register）。 */')
  lines.push('void mq_control_register_generated(mq_control* c, void* user);')
  lines.push('/* 生成的方法名列表（JSON 数组，含类型化 inputSchema），供 MCP tools/list 使用。 */')
  lines.push('const char* mq_control_tools_json(void);')
  lines.push('')
  lines.push('#ifdef __cplusplus')
  lines.push('}')
  lines.push('#endif')
  lines.push('')
  return lines.join('\n')
}
function genControlSource(namespaces) {
  const lines = []
  lines.push('// Generated by parser.js -- DO NOT EDIT')
  lines.push('// Control-plane registration + typed MCP tool schema (from control_list)')
  lines.push('#include "maque_control.h"')
  lines.push('')
  lines.push('#include <cstdio>')
  lines.push('#include <cstring>')
  lines.push('')
  lines.push('namespace {')
  lines.push('struct Entry { const char* method; mq_control_handler fn; };')
  for (const ns of namespaces) {
    for (const c of ns.controls || []) {
      lines.push(`int tramp_${controlSym(c.method)}(void* user, const char*, const mq_json* params, char* out, size_t cap) {`)
      lines.push(`  return ${controlSym(c.method)}(user, params, out, cap);`)
      lines.push('}')
    }
  }
  lines.push('} // namespace')
  lines.push('')
  lines.push('static const Entry kEntries[] = {')
  for (const ns of namespaces) {
    for (const c of ns.controls || []) {
      lines.push(`  { "${c.method}", tramp_${controlSym(c.method)} },`)
    }
  }
  lines.push('};')
  lines.push('')
  lines.push('// 权限判定单一真相（D2）：变更类方法由 control_list 的 mutating 标记决定')
  lines.push('static const char* kMutating[] = {')
  for (const ns of namespaces) {
    for (const c of ns.controls || []) {
      if (c.mutating) lines.push(`  "${c.method}",`)
    }
  }
  lines.push('};')
  lines.push('')
  lines.push('extern "C" int mq_control_is_mutating(const char* method) {')
  lines.push('  if (!method) return 0;')
  lines.push('  for (const char* m : kMutating) if (std::strcmp(m, method) == 0) return 1;')
  lines.push('  return 0;')
  lines.push('}')
  lines.push('')
  lines.push('extern "C" void mq_control_register_generated(mq_control* c, void* user) {')
  lines.push('  if (!c) return;')
  lines.push('  for (const Entry& e : kEntries) mq_control_register(c, e.method, e.fn, user);')
  lines.push('}')
  lines.push('')
  lines.push('extern "C" const char* mq_control_tools_json(void) {')
  /* MCP 工具表：JSON 含大量引号 → 以 C++ 原始字符串字面量输出（避免转义地狱）。 */
  const tools = []
  for (const ns of namespaces) {
    for (const c of ns.controls || []) {
      const props = c.args.map((a) => `"${a.name}":{"type":"${jsonTypeFor(a.type)}"}`).join(',')
      tools.push(`{"name":"${c.method}","description":"maque ${c.ns} domain method",` +
                 `"inputSchema":{"type":"object","properties":{${props}},"additionalProperties":false}}`)
    }
  }
  /* 必须单行：控制面 stdio 传输按行分隔 JSON-RPC（docs/11 §2）。 */
  lines.push(`  static const char* kJson = R\"MQTOOLS([${tools.join(',')}])MQTOOLS\";`)
  lines.push('  return kJson;')
  lines.push('}')
  lines.push('')
  return lines.join('\n')
}
/* 脚本事件回调：名字 + 参数个数 + 输入事件映射（docs/14 §4、docs/19 §8）。 */
function genEventsHeader(namespaces) {
  const cbs = []
  for (const ns of namespaces) for (const cb of ns.callbacks || []) cbs.push(cb)
  const lines = []
  lines.push('// Generated by parser.js -- DO NOT EDIT')
  lines.push('// Script event callbacks (from schema/ callbacks)')
  lines.push('#pragma once')
  lines.push('')
  lines.push('#include <stdint.h>')
  lines.push('#include "core/script/mq_script.h"')
  lines.push('#include "core/input/mq_input.h"   // 输入事件枚举（回调 on: 映射用）')
  lines.push('')
  lines.push('#ifdef __cplusplus')
  lines.push('extern "C" {')
  lines.push('#endif')
  lines.push('')
  lines.push('typedef enum mq_event_kind {')
  cbs.forEach((cb, i) => lines.push(`  MQ_EVENT_${cb.name.toUpperCase()} = ${i},`))
  lines.push(`  MQ_EVENT_COUNT = ${cbs.length}`)
  lines.push('} mq_event_kind;')
  lines.push('')
  lines.push('typedef struct mq_event_desc { const char* name; uint32_t argc; } mq_event_desc;')
  lines.push('const mq_event_desc* mq_event_table(uint32_t* out_count);')
  lines.push('/* 输入事件类型（mq_input_event_type 值）→ 回调 kind；无对应回调返回 -1。 */')
  lines.push('int mq_event_for_input(uint32_t input_event_type);')
  lines.push('/* 按声明派发：数值参数依序取 a/b/c，字符串参数取 text；未定义的回调被跳过。成功 0。 */')
  lines.push('int mq_event_dispatch(mq_script_vm* vm, int kind, double a, double b, double c, const char* text);')
  lines.push('')
  lines.push('#ifdef __cplusplus')
  lines.push('}')
  lines.push('#endif')
  lines.push('')
  return lines.join('\n')
}
function genEventsSource(namespaces) {
  const cbs = []
  for (const ns of namespaces) for (const cb of ns.callbacks || []) cbs.push(cb)
  const lines = []
  lines.push('// Generated by parser.js -- DO NOT EDIT')
  lines.push('// Script event routing (from callbacks)')
  lines.push('#include "maque_events.h"')
  lines.push('')
  lines.push('#include <cstring>')
  lines.push('')
  lines.push('static const mq_event_desc kEvents[] = {')
  for (const cb of cbs) lines.push(`  { "${cb.name}", ${cb.args.length}u },`)
  lines.push('};')
  lines.push('')
  lines.push('extern "C" const mq_event_desc* mq_event_table(uint32_t* out_count) {')
  lines.push(`  if (out_count) *out_count = ${cbs.length}u;`)
  lines.push('  return kEvents;')
  lines.push('}')
  lines.push('')
  lines.push('extern "C" int mq_event_for_input(uint32_t t) {')
  lines.push('  switch (t) {')
  const INPUT_TYPES = {
    pointer_down: 'MQ_INPUT_POINTER_DOWN', pointer_up: 'MQ_INPUT_POINTER_UP',
    pointer_move: 'MQ_INPUT_POINTER_MOVE', pointer_wheel: 'MQ_INPUT_POINTER_WHEEL',
    click: 'MQ_INPUT_CLICK', key_down: 'MQ_INPUT_KEY_DOWN', key_up: 'MQ_INPUT_KEY_UP',
    text: 'MQ_INPUT_TEXT', pointer_cancel: 'MQ_INPUT_POINTER_CANCEL',
  }
  for (const cb of cbs) {
    if (!cb.on) continue
    const macro = INPUT_TYPES[cb.on]
    if (!macro) throw new SchemaError(`callback "${cb.name}" has unknown on: ${cb.on}`)
    lines.push(`    case ${macro}: return MQ_EVENT_${cb.name.toUpperCase()};`)
  }
  lines.push('    default: return -1;')
  lines.push('  }')
  lines.push('}')
  lines.push('')
  /* 每个回调按声明的参数类型生成精确调用（DSL 是唯一真相，不再按 argc 猜测）。 */
  lines.push('extern "C" int mq_event_dispatch(mq_script_vm* vm, int kind, double a, double b, double c, const char* text) {')
  lines.push('  if (!vm || kind < 0 || kind >= (int)MQ_EVENT_COUNT) return 1;')
  lines.push('  switch (kind) {')
  cbs.forEach((cb, i) => {
    const nums = cb.args.filter((x) => x.type !== 'string')
    const hasStr = cb.args.some((x) => x.type === 'string')
    const undef = `MQ_EVENT_${cb.name.toUpperCase()}`
    lines.push(`    case ${undef}: {`)
    lines.push(`      if (!mq_script_has(vm, "${cb.name}")) return 0;   // 脚本未定义 → 跳过`)
    const argv = ['a', 'b', 'c'].slice(0, nums.length)
    if (hasStr) {
      lines.push(`      return mq_script_call${nums.length}s(vm, "${cb.name}", ${argv.join(', ') || '0.0'}, text ? text : "", nullptr);`)
    } else {
      lines.push(`      return mq_script_call${nums.length}(vm, "${cb.name}"${argv.length ? ', ' + argv.join(', ') : ''}, nullptr);`)
    }
    lines.push('    }')
  })
  lines.push('    default: return 1;')
  lines.push('  }')
  lines.push('}')

  return lines.join('\n')
}

/* 模块 C ABI 头（docs/02「跨模块只暴露 C ABI」、docs/21「声明一次」）：
 * 生成的 <module>.h 与原手写头等价，原头退化为 #include shim（路径不变，消费者无需改动）。 */
/* C 声明符：把「类型 + 名字」渲染成合法 C 声明。
 * 支持 指针数组 `char(*)[256]` → `char (*name)[256]`、函数指针 `ret (*)(args)` → `ret (*name)(args)`、
 * 数组 `T[N]` → `T name[N]`、其余 `T name`。 */
function cDecl(type, name) {
  const ptrArr = type.match(/^(.+?)\(\*\)(\[[^\]]*\])$/)
  if (ptrArr) return `${ptrArr[1]} (*${name})${ptrArr[2]}`
  if (type.includes('(*)')) return type.replace('(*)', `(*${name})`)
  const arr = type.match(/^(.*)\[([^\]]*)\]$/)
  if (arr) return `${arr[1]} ${name}[${arr[2]}]`
  return `${type} ${name}`
}

function genModuleAbi(mod) {
  const up = mod.module.toUpperCase()
  const lines = []
  lines.push('// Generated by parser.js -- DO NOT EDIT')
  lines.push(`// ${mod.abiName} C ABI v${mod.version}（跨模块仅 C ABI + 版本化，docs/02）`)
  if (mod.summary) lines.push(`// ${mod.summary}`)
  lines.push('#pragma once')
  lines.push('')
  for (const inc of mod.includes) lines.push(`#include ${inc}`)
  lines.push('')
  lines.push(`#define MQ_${up}_ABI_VERSION ${mod.version}u`)
  lines.push(`#define MQ_${up}_IFACE_NAME "${mod.abiName}.v${mod.version}"`)
  lines.push(`#define MQ_${up}_ABI_NAME "${mod.abiName}"`)
  lines.push('')
  const cxx = mod.language === 'cxx'
  if (cxx) {
    if (mod.nameSpace) lines.push(`namespace ${mod.nameSpace} {`)
  } else {
    lines.push('#ifdef __cplusplus')
    lines.push('extern "C" {')
    lines.push('#endif')
  }
  lines.push('')
  /* ① 前向声明：不透明句柄 + 全部结构体（避免 typedef/字段的前向引用顺序问题） */
  const fwd = [...mod.opaque, ...mod.structs.map((st) => st.name)]
  for (const o of fwd) lines.push(`typedef struct ${o} ${o};`)
  if (fwd.length) lines.push('')
  for (const e of mod.enums) {
    if (e.doc) lines.push(`/* ${e.doc} */`)
    lines.push(e.name ? `typedef enum ${e.name} {` : 'enum {')
    e.values.forEach((v, i) => {
      const comma = i + 1 < e.values.length ? ',' : ''
      lines.push(`    ${v.name} = ${v.value}${comma}${v.doc ? '   /* ' + v.doc + ' */' : ''}`)
    })
    lines.push(e.name ? `} ${e.name};` : '};')
    lines.push('')
  }
  for (const st of mod.structs) {
    if (st.doc) lines.push(`/* ${st.doc} */`)
    lines.push(`typedef struct ${st.name} {`)
    for (const f of st.fields) {
      /* 三种 C 声明形态：函数指针 `ret (*)(args)` → `ret (*name)(args)`；
       * 数组 `T[N]` → `T name[N]`；其余 `T name`。 */
      lines.push(`    ${cDecl(f.type, f.name)};${f.doc ? '   /* ' + f.doc + ' */' : ''}`)
    }
    lines.push(`} ${st.name};`)
    lines.push('')
  }
  for (const k of mod.consts) {
    if (k.doc) lines.push(`/* ${k.doc} */`)
    lines.push(`constexpr ${k.type} ${k.name} = ${k.value};`)
  }
  if (mod.consts.length) lines.push('')
  /* ④ 函数指针等类型别名（结构体已定义完毕） */
  for (const td of mod.typedefs) {
    lines.push(`typedef ${cDecl(td.type, td.name)};`)
  }
  if (mod.typedefs.length) lines.push('')
  /* ⑤ 宏 */
  if (mod.code) {
    for (const ln of mod.code.split('\n')) lines.push(ln)
    lines.push('')
  }
  for (const mc of mod.macros) {
    if (mc.doc) lines.push(`/* ${mc.doc} */`)
    if (mc.raw) {
      for (const ln of mc.raw.split('\n')) lines.push(ln)
    } else if (mc.valueCxx) {
      lines.push('#ifdef __cplusplus')
      lines.push(`#define ${mc.name} ${mc.valueCxx}`)
      lines.push('#else')
      lines.push(`#define ${mc.name} ${mc.value}`)
      lines.push('#endif')
    } else {
      lines.push(`#define ${mc.name} ${mc.value}`)
    }
  }
  if (mod.macros.length) lines.push('')
  for (const f of mod.fns) {
    if (f.doc) lines.push(`/* ${f.doc} */`)
    const args = f.args.length ? f.args.map((a) => cDecl(a.type, a.name)).join(', ') : 'void'
    lines.push(`${f.returns} ${f.name}(${args});`)
  }
  lines.push('')
  if (cxx) {
    if (mod.nameSpace) lines.push(`} // namespace ${mod.nameSpace}`)
  } else {
    lines.push('#ifdef __cplusplus')
    lines.push('}')
    lines.push('#endif')
  }
  lines.push('')
  return lines.join('\n')
}


/* ---------------------------------------------------------------------------
 * 文档生成（docs/21 §6.5：从声明生成参考文档）
 *   API.md    —— 脚本 API / 控制面方法 / 事件回调 / 插件接口 / 模块 C ABI
 *   TYPES.md  —— 数据类型（字段、默认值、attrs、role）
 * 生成物随 codegen 一起产出；`tools/gen_api_docs.sh` 会同步一份到 docs/api/ 供阅读。
 * ------------------------------------------------------------------------- */
function mdEsc(v) {
  return String(v === undefined || v === null ? '' : v).replace(/\|/g, '\\|')
}
function roleNames(t) {
  return (t.roles || []).join(', ')
}
function attrsStr(attrs) {
  const keys = Object.keys(attrs || {})
  if (!keys.length) return ''
  return keys.map((k) => {
    const v = attrs[k]
    return v === true ? k : `${k}: ${v}`
  }).join(', ')
}

function genApiDoc(namespaces, interfaces) {
  const L = []
  let apiCount = 0, ctrlCount = 0, cbCount = 0, modCount = 0
  for (const ns of namespaces) {
    apiCount += (ns.apis || []).length
    ctrlCount += (ns.controls || []).length
    cbCount += (ns.callbacks || []).length
    if (ns.moduleAbi) modCount++
  }
  L.push('# maque API 参考（自动生成）')
  L.push('')
  L.push('> ⚠️ 本文件由 `modules/core/data/parser.js` 从 `schema/`（**唯一真相**）生成，**请勿手工编辑**。')
  L.push('> 重新生成：`cmake --build build --target maque_codegen`，再运行 `tools/gen_api_docs.sh` 同步到 `docs/api/`。')
  L.push('')
  L.push(`统计：脚本 API **${apiCount}** 个 · 控制面方法 **${ctrlCount}** 个 · 事件回调 **${cbCount}** 个 · 模块 C ABI **${modCount}** 个 · 插件接口 **${interfaces.length}** 个。`)
  L.push('')
  L.push('## 目录')
  L.push('')
  L.push('1. [脚本 API（api_list）](#1-脚本-apiapilist)')
  L.push('2. [控制面方法（control_list，AI/MCP）](#2-控制面方法control_listaimcp)')
  L.push('3. [脚本事件回调（callbacks）](#3-脚本事件回调callbacks)')
  L.push('4. [插件接口（interface → C vtable）](#4-插件接口interface--c-vtable)')
  L.push('5. [模块 C ABI（module_abi）](#5-模块-c-abimodule_abi)')
  L.push('6. [数据类型与组件](TYPES.md)')
  L.push('')

  /* 1. 脚本 API */
  L.push('## 1. 脚本 API（api_list）')
  L.push('')
  L.push('脚本里直接调用（`host` 命名空间为全局函数，无前缀；其余为 `<ns>.<fn>`）。')
  L.push('`ctx` = 首参注入引擎实例（脚本不可见）。')
  L.push('')
  for (const ns of namespaces) {
    if (!(ns.apis || []).length) continue
    const src = ns.source ? ` · \`${ns.source}\`` : ''
    L.push(`### ${ns.name}${ns.global ? '（全局，无前缀）' : ''}${src}`)
    L.push('')
    L.push('| 函数 | 参数 | 返回 | 说明 |')
    L.push('|------|------|------|------|')
    for (const a of ns.apis) {
      const name = ns.global ? a.name : `${ns.name}.${a.name}`
      const args = a.args.map((x) => `${x.name}: ${x.type}`).join(', ')
      const note = a.manual ? `MANUAL → \`${a.manual}\`` : (a.ctx ? 'ctx' : '')
      L.push(`| \`${name}\` | ${mdEsc(args) || '—'} | ${mdEsc(a.returns)} | ${note} |`)
    }
    L.push('')
  }

  /* 2. 控制面方法 */
  L.push('## 2. 控制面方法（control_list，AI/MCP）')
  L.push('')
  L.push('JSON-RPC（`{"jsonrpc":"2.0","id":N,"method":"<方法>","params":{…}}`）；')
  L.push('同一份声明还生成 MCP `tools/list`（含类型化 `inputSchema`）。')
  L.push('')
  L.push('| 方法 | 参数 | MCP 类型 |')
  L.push('|------|------|----------|')
  for (const ns of namespaces) {
    for (const c of ns.controls || []) {
      const args = c.args.map((x) => `${x.name}: ${x.type}`).join(', ')
      const types = c.args.map((x) => jsonTypeFor(x.type)).join(', ')
      L.push(`| \`${c.method}\` | ${mdEsc(args) || '—'} | ${types || '—'} |`)
    }
  }
  L.push('')

  /* 3. 回调 */
  L.push('## 3. 脚本事件回调（callbacks）')
  L.push('')
  L.push('引擎按名字调用脚本里的全局函数；`触发` 列为对应输入事件（无则非输入触发）。')
  L.push('')
  L.push('| 回调 | 参数 | 触发 |')
  L.push('|------|------|------|')
  for (const ns of namespaces) {
    for (const cb of ns.callbacks || []) {
      const args = cb.args.map((x) => `${x.name}: ${x.type}`).join(', ')
      L.push(`| \`${cb.name}\` | ${mdEsc(args) || '—'} | ${mdEsc(cb.on) || '—'} |`)
    }
  }
  L.push('')

  /* 4. 插件接口 */
  L.push('## 4. 插件接口（interface → C vtable）')
  L.push('')
  L.push('跨 `.so` 的唯一函数表；宿主经 `maque::module`(dlopen) 加载并校验版本/结构大小。')
  L.push('')
  for (const it of interfaces) {
    L.push(`### ${it.name} v${it.version} — \`${it.apiName}\``)
    L.push('')
    if ((it.records || []).length) {
      L.push('记录（POD）：')
      L.push('')
      for (const rec of it.records) {
        L.push(`- \`${rec.name}\`：${rec.fields.map((f) => `${f.name}: ${f.type}`).join(', ')}`)
      }
      L.push('')
    }
    L.push('| 方法 | 参数 | 返回 |')
    L.push('|------|------|------|')
    for (const m of it.methods) {
      const args = m.args.map((a) => `${a.name}: ${a.type}`).join(', ')
      L.push(`| \`${m.fn}\` | ${mdEsc(args) || '—'} | ${mdEsc(m.returns)} |`)
    }
    L.push('')
  }

  /* 5. 模块 C ABI */
  L.push('## 5. 模块 C ABI（module_abi）')
  L.push('')
  L.push('每模块的公开 C 头由 DSL 生成（原手写头是 `#include "abi/<mod>.h"` shim）；')
  L.push('`language: cxx` 的模块生成命名空间声明。')
  L.push('')
  for (const ns of namespaces) {
    const m = ns.moduleAbi
    if (!m) continue
    L.push(`### ${m.module} — \`${m.abiName}\` v${m.version}${m.language === 'cxx' ? '（C++）' : ''}`)
    L.push('')
    L.push(`头文件：\`build/modules/core/data/generated/abi/${m.module}.h\` · 宏：\`MQ_${m.module.toUpperCase()}_ABI_VERSION\` / \`_IFACE_NAME\``)
    L.push('')
    if (m.summary) { L.push(`${m.summary}`); L.push('') }
    if ((m.enums || []).length) {
      L.push('枚举：')
      L.push('')
      for (const e of m.enums) {
        L.push(`- ${e.name ? `\`${e.name}\`` : '(匿名)'}：${e.values.map((v) => `${v.name}=${v.value}`).join(', ')}`)
      }
      L.push('')
    }
    if ((m.structs || []).length) {
      L.push('结构体：')
      L.push('')
      for (const st of m.structs) {
        L.push(`- \`${st.name}\`：${st.fields.map((f) => `${f.name}: ${f.type}`).join(', ')}`)
      }
      L.push('')
    }
    L.push('| 函数 | 返回 | 参数 |')
    L.push('|------|------|------|')
    for (const f of m.fns) {
      const args = f.args.map((a) => `${a.name}: ${a.type}`).join(', ')
      L.push(`| \`${f.name}\` | ${mdEsc(f.returns)} | ${mdEsc(args) || '—'} |`)
    }
    L.push('')
  }
  return L.join('\n')
}

function genTypeDoc(namespaces) {
  const L = []
  const types = sortedTypes(namespaces)
  L.push('# maque 数据类型参考（自动生成）')
  L.push('')
  L.push('> ⚠️ 由 `modules/core/data/parser.js` 从 `schema/` 生成，**请勿手工编辑**。')
  L.push('> 这些类型同时生成 C++ 头（`maque_types.h`）与 C ABI 描述符（`maque_desc.*`）；`role` 表示引擎会通用处理该组件的语义。')
  L.push('')
  L.push(`共 **${types.length}** 个类型。字段格式：\`名称: 类型 = 默认值 (attrs)\`。`)
  L.push('')
  const byNs = new Map()
  for (const t of types) {
    if (!byNs.has(t.ns)) byNs.set(t.ns, [])
    byNs.get(t.ns).push(t)
  }
  for (const [ns, list] of byNs) {
    const src = list[0].source ? ` · \`${list[0].source}\`` : ''
    L.push(`## ${ns}（${list.length}）${src}`)
    L.push('')
    for (const t of list) {
      const sz = t.size !== null && t.size !== undefined ? t.size : typeSize(t)
      const szTxt = sz === null || sz === undefined ? '变长（含动态成员）' : `${sz}B`
      const head = t.kind === 'enum'
        ? `### ${t.fqName} — enum`
        : t.kind === 'alias' ? `### ${t.fqName} — alias \`${t.aliasText}\``
        : `### ${t.fqName} — struct (${szTxt})`
      L.push(head)
      L.push('')
      if (t.roles && t.roles.length) { L.push(`role: \`${roleNames(t)}\``); L.push('') }
      if (t.kind === 'enum') {
        L.push('| 值 | 数值 |')
        L.push('|----|------|')
        for (const v of t.values) L.push(`| \`${v.name}\` | ${v.value} |`)
      } else if (t.kind === 'struct') {
        if (!t.fields.length) { L.push('（无字段）') }
        else {
          L.push('| 字段 | 类型 | 默认值 | attrs |')
          L.push('|------|------|--------|-------|')
          for (const f of t.fields) {
            const def = f.default === null || f.default === undefined ? '—'
                      : String(Array.isArray(f.default) ? '{' + f.default.join(', ') + '}' : f.default)
            L.push(`| \`${f.name}\` | ${mdEsc(f.typeText)} | ${mdEsc(def)} | ${mdEsc(attrsStr(f.attrs))} |`)
          }
        }
      }
      L.push('')
    }
  }
  return L.join('\n')
}

function main() {
  let args
  try {
    args = parseArgs(process.argv.slice(2))
  } catch (err) {
    console.error(`error: ${err.message}`)
    return 2
  }
  if (args.help) {
    printHelp()
    return 0
  }

  const schemaArg = path.resolve(args.schema || path.join(__dirname, 'schema'))

  let tree
  let namespaces
  try {
    tree = loadSchema(schemaArg)
    namespaces = parseSchemaTree(tree)
  } catch (err) {
    if (err instanceof SchemaError) {
      console.error(`error: ${err.message}`)
      return 1
    }
    throw err
  }

  const duplicateErrors = []
  const registry = buildRegistry(namespaces, duplicateErrors)

  // assign stable type ids
  for (const ns of namespaces) {
    for (const t of ns.types) t.typeId = fnv1a32(t.fqName)
  }

  const { errors, warnings, sizeMismatches } = validate(namespaces, registry)
  errors.unshift(...duplicateErrors)
  // import 校验（结构依赖需要 validate 里已解析的引用）
  const importCheck = checkSchemaImports(tree, namespaces)
  errors.push(...importCheck.errors)
  warnings.push(...importCheck.warnings)
  for (const w of warnings) console.warn(`warning: ${w}`)
  if (sizeMismatches.length > 0) {
    console.error('POD size mismatches (name | declared | computed):')
    for (const m of sizeMismatches) {
      console.error(`  ${m.name} | ${m.declared} | ${m.computed}`)
    }
  }
  if (errors.length > 0) {
    for (const e of errors) console.error(`error: ${e}`)
    console.error(`\n${errors.length} error(s); generation aborted`)
    return 1
  }

  if (args.check) {
    for (const line of schemaFileSummary(tree, namespaces)) console.log(line)
    console.log(
      `ok: ${registry.byFq.size} type(s) across ${namespaces.length} namespace(s), ${tree.files.length} file(s) valid`)
    return 0
  }

  const interfaces = parseInterfaces(tree.entries)
  // 接口校验（最小）：方法名/参数名唯一
  for (const it of interfaces) {
    const seen = new Set()
    for (const m of it.methods) {
      if (seen.has(m.fn)) errors.push(`interface ${it.name}: duplicate method ${m.fn}`)
      seen.add(m.fn)
    }
  }

  const defaultOutDir = tree.dirMode
    ? path.join(tree.rootDir, '..', 'generated')
    : path.join(tree.rootDir, 'generated')
  const outDir = path.resolve(args.out || defaultOutDir)
  fs.mkdirSync(outDir, { recursive: true })

  const types = sortedTypes(namespaces)
  const files = [
    ['maque_types.h', genTypesHeader(namespaces)],
    ['maque_desc.h', genDescHeader()],
    ['maque_desc.cpp', genDescSource(types)],
    ['maque_schema.json', genJson(namespaces, tree)],
    ['maque_api.h', genApiHeader(namespaces)],
    ['maque_bindings.cpp', genBindingsSource(namespaces)],
    ['maque_ifaces.h', genInterfacesHeader(interfaces)],
    ['maque_bind.h', genBindHeader()],
    ['API.md', genApiDoc(namespaces, interfaces)],
    ['TYPES.md', genTypeDoc(namespaces)],
    ['maque_control.h', genControlHeader(namespaces)],
    ['maque_control_gen.cpp', genControlSource(namespaces)],
    ['maque_events.h', genEventsHeader(namespaces)],
    ['maque_events_gen.cpp', genEventsSource(namespaces)],
    ...namespaces.filter((ns) => ns.moduleAbi)
                     .map((ns) => [`abi/${ns.moduleAbi.module}.h`, genModuleAbi(ns.moduleAbi)]),
  ]
  for (const [name, content] of files) {
    const target = path.join(outDir, name)
    fs.mkdirSync(path.dirname(target), { recursive: true })
    fs.writeFileSync(target, content)
  }

  console.log(`generated ${types.length} type(s) -> ${outDir}`)
  for (const [name] of files) console.log(`  ${name}`)
  return 0
}

if (require.main === module) {
  process.exit(main())
}

module.exports = {
  parseSchemaDocument,
  parseTypeExpr,
  fnv1a32,
  validate,
  buildRegistry,
  genTypesHeader,
  genDescHeader,
  genDescSource,
  genJson,
  // schema 目录 / import（SCHEMA.md §1）
  loadSchema,
  parseSchemaTree,
  checkSchemaImports,
  scanSchemaFiles,
  schemaFileSummary,
}
