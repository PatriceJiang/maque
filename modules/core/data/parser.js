'use strict'

/**
 * maque schema codegen (SCHEMA.md v1)
 *
 * Reads schema.yaml and produces three artifacts from the same canonical IR:
 *   1. generated/maque_types.h   -- C++ types (enum class / struct / defaults / static_assert)
 *   2. generated/maque_desc.h+c  -- C ABI descriptors + registry (mq_schema_types[])
 *   3. generated/maque_schema.json -- normalized, sorted, machine readable schema
 *
 * CLI: node parser.js [--schema schema.yaml] [--out generated] [--check]
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
    if (!nsEntry || typeof nsEntry !== 'object' || nsEntry.namespace === undefined) {
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
    namespaces.push({
      name: nsName,
      version: nsEntry.version === undefined ? 1 : Number(nsEntry.version),
      types,
      apis,
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
  lines.push('// Source: schema.yaml (SCHEMA.md v1)')
  lines.push('')
  lines.push('#pragma once')
  lines.push('')
  lines.push('#include <cstdint>')
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

typedef struct mq_field_desc {
    const char*  name;
    uint32_t     offset;       /* offsetof(Type, field) */
    mq_type_kind kind;
    uint32_t     type_id;      /* named/element type id; 0 if not named */
    uint32_t     count;        /* fixed array length; 0 otherwise */
    uint32_t     attrs_flags;  /* bit0 transient, bit1 readonly, bit2 deprecated */
    /* 动态数组（T[]）的容器算子（其余字段为 0）——使反射可类型化地增删/取元素 */
    uint32_t     elem_type_id;                    /* 具名元素类型 id；0 否则 */
    void       (*container_resize)(void* field, uint32_t n);
    void*      (*container_elem)(void* field, uint32_t i);
} mq_field_desc;

typedef struct mq_type_desc {
    const char*          name;   /* fully qualified ns.Name */
    uint32_t             type_id;
    uint32_t             size;
    uint32_t             align;
    uint32_t             field_count;
    const mq_field_desc* fields;
    uint32_t             flags;  /* bit0 is_enum, bit1 is_pod, bit2 has_dynamic */
} mq_type_desc;

/* registry (lookup by type_id / name) */
extern const mq_type_desc* const mq_schema_types[];
extern const uint32_t            mq_schema_type_count;

#ifdef __cplusplus
}
#endif
`
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

  // 动态数组（T[]，具名 struct 元素）的容器算子
  const dynElem = (node) =>
    (node && node.cat === 'array_dyn' && node.elem && node.elem.cat === 'named' && node.elem.resolved)
      ? node.elem.resolved : null

  for (const t of types) {
    if (t.kind !== 'struct' || t.fields.length === 0) continue
    const sym = `mq_fields_${mangle(t.fqName)}`
    for (const f of t.fields) {
      const de = dynElem(f.typeNode)
      if (!de) continue
      const base = `mq_dyn_${mangle(t.fqName)}_${mangle(f.name)}`
      const elemCpp = de.fqName.replace(/\./g, '::')
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
        extra = `${de.typeId}u, ${base}_resize, ${base}_elem`
      } else {
        extra = `0u, 0, 0`
      }
      lines.push(
        `    { "${f.name}", (uint32_t)offsetof(${t.fqName.replace(/\./g, '::')}, ${f.name}), ` +
        `MQ_TK_${kind}, ${typeId}u, ${count}u, ${attrs}u, ${extra} },`
      )
    }
    lines.push('};')
    lines.push('')
  }

  for (const t of types) {
    const cpp = t.fqName.replace(/\./g, '::')
    const fieldsSym = t.kind === 'struct' && t.fields.length > 0
      ? `mq_fields_${mangle(t.fqName)}`
      : '0'
    lines.push(`extern const mq_type_desc mq_type_${mangle(t.fqName)} = {`)
    lines.push(`    "${t.fqName}",`)
    lines.push(`    ${t.typeId}u,`)
    lines.push(`    (uint32_t)sizeof(${cpp}),`)
    lines.push(`    (uint32_t)alignof(${cpp}),`)
    lines.push(`    ${t.kind === 'struct' ? t.fields.length : 0}u,`)
    lines.push(`    ${fieldsSym},`)
    lines.push(`    ${typeFlags(t)}u,`)
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

function genJson(namespaces) {
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

  const doc = {
    schema_version: 1,
    generated_by: 'parser.js',
    type_count: types.length,
    types,
  }
  return JSON.stringify(doc, null, 2) + '\n'
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
  console.log('usage: node parser.js [--schema schema.yaml] [--out generated] [--check]')
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
  lines.push('// Engine API declarations (from schema.yaml api_list)')
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
      const rt = apiCppType(fn.returns)
      return rt !== null && fn.args.every((a) => apiCppType(a.type) !== null)
    })
    if (declared.length === 0) continue
    lines.push(`namespace ${ns.name} {`)
    for (const fn of declared) {
      const params = fn.args.map((a) => `${apiCppType(a.type)} ${a.name}`).join(', ')
      lines.push(`  ${apiCppType(fn.returns)} ${fn.name}(${params});`)
    }
    lines.push('}')
    lines.push('')
  }
  return lines.join('\n')
}

function genBindingsSource(namespaces) {
  const lines = []
  lines.push('// Generated by parser.js -- DO NOT EDIT')
  lines.push('// Script bindings for engine APIs declared in schema.yaml api_list.')
  lines.push('#include "maque_api.h"')
  lines.push('#include "core/script/mq_script.h"')
  lines.push('#include <cstdint>')
  lines.push('')
  lines.push('namespace {')
  for (const ns of namespaces) {
    for (const fn of ns.apis || []) {
      const numeric = apiNumeric(fn.returns) && fn.args.every((a) => API_SCALARS.has(a.type))
      if (!numeric) continue
      const sym = `b_${ns.name}_${fn.name}`
      const callArgs = fn.args.map((a, i) => apiCast(a.type, `a[${i}]`)).join(', ')
      lines.push(`int ${sym}(void*, int, const double* a, double* ret) {`)
      if (fn.returns === 'void') lines.push(`  ${ns.name}::${fn.name}(${callArgs}); return 0;`)
      else lines.push(`  *ret = (double)${ns.name}::${fn.name}(${callArgs}); return 0;`)
      lines.push('}')
    }
  }
  lines.push('} // namespace')
  lines.push('')
  lines.push('extern "C" void mq_bind_generated(mq_script_vm* vm) {')
  for (const ns of namespaces) {
    for (const fn of ns.apis || []) {
      const numeric = apiNumeric(fn.returns) && fn.args.every((a) => API_SCALARS.has(a.type))
      if (!numeric) continue
      lines.push(`  mq_script_register_num(vm, "${ns.name}.${fn.name}", b_${ns.name}_${fn.name}, nullptr);`)
    }
  }
  lines.push('}')
  lines.push('')
  return lines.join('\n')
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

  const schemaPath = path.resolve(args.schema || path.join(__dirname, 'schema.yaml'))
  if (!fs.existsSync(schemaPath)) {
    console.error(`error: schema not found: ${schemaPath}`)
    return 2
  }

  let root
  try {
    root = parseYamlDocument(fs.readFileSync(schemaPath, 'utf8'))
  } catch (err) {
    console.error(`error: failed to parse ${schemaPath}: ${err.message}`)
    return 2
  }

  let namespaces
  try {
    namespaces = parseSchemaDocument(root)
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
    console.log(`ok: ${registry.byFq.size} type(s) across ${namespaces.length} namespace(s) valid`)
    return 0
  }

  const outDir = path.resolve(args.out || path.join(path.dirname(schemaPath), 'generated'))
  fs.mkdirSync(outDir, { recursive: true })

  const types = sortedTypes(namespaces)
  const files = [
    ['maque_types.h', genTypesHeader(namespaces)],
    ['maque_desc.h', genDescHeader()],
    ['maque_desc.cpp', genDescSource(types)],
    ['maque_schema.json', genJson(namespaces)],
    ['maque_api.h', genApiHeader(namespaces)],
    ['maque_bindings.cpp', genBindingsSource(namespaces)],
  ]
  for (const [name, content] of files) {
    fs.writeFileSync(path.join(outDir, name), content)
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
}
