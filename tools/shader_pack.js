#!/usr/bin/env node
'use strict'

/**
 * 内建 shader 打包器（构建流水线，由 cmake/shader_pack.cmake 调用）。
 *
 * 资产布局（唯一真相，随仓库提交）：
 *   assets/builtin/shaders/<name>/*.wgsl      一个 shader 一个目录；目录名 = shader 名；
 *                                             目录内多个 .wgsl 按文件名排序拼接
 * 生成物（构建目录，不入库）：
 *   <out>/<name>.cpp            该 shader 的 zstd 压缩 Blob + 取源码的函数（每个 shader 一个源文件）
 *   <out>/builtin_shaders.h     注册表类型 / 表 / count / mq_builtin_shader_text()
 *   <out>/builtin_shaders.cpp   注册表（只引用**启用**的 shader → 剔除后不链接、不占体积）
 *
 * 用法：
 *   node tools/shader_pack.js --assets <dir> --out <dir> [--shaders a,b,c] [--no-compress]
 *   --shaders 省略 = 打包 --assets 下全部 shader 目录；给空串 = 一个都不编（合法，表为空）。
 *
 * 压缩：优先 Node 内置 zlib.zstdCompressSync（Node ≥ 22.15），其次外部 `zstd` CLI；
 * 都没有则报错并提示用 --no-compress（CMake 会据 libzstd 可用性自动选择）。
 */

const fs = require('fs')
const path = require('path')
const zlib = require('zlib')
const { execFileSync } = require('child_process')

const ZSTD_LEVEL = 19        // 与 CLI 的 `-19` 一致；shader 只有几 KB，取高档

function parseArgs(argv) {
  const args = { assets: null, out: null, shaders: null, compress: true }
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i]
    if (a === '--assets') args.assets = argv[++i]
    else if (a === '--out') args.out = argv[++i]
    else if (a === '--shaders') args.shaders = argv[++i]
    else if (a === '--no-compress') args.compress = false
    else { console.error(`shader_pack: 未知参数 "${a}"`); process.exit(2) }
  }
  if (!args.assets || !args.out) {
    console.error('usage: node tools/shader_pack.js --assets <dir> --out <dir> [--shaders a,b] [--no-compress]')
    process.exit(2)
  }
  return args
}

/* 发现 shader 名（一级子目录，且目录内至少有一个 .wgsl） */
function discover(assetsDir) {
  if (!fs.existsSync(assetsDir)) return []
  return fs.readdirSync(assetsDir, { withFileTypes: true })
    .filter((e) => e.isDirectory())
    .map((e) => e.name)
    .filter((name) => fs.readdirSync(path.join(assetsDir, name)).some((f) => f.endsWith('.wgsl')))
    .sort()
}

/* 读一个 shader：目录内 *.wgsl 按文件名排序拼接（后续要拆段时天然支持） */
function readShader(assetsDir, name) {
  const dir = path.join(assetsDir, name)
  if (!fs.existsSync(dir) || !fs.statSync(dir).isDirectory()) {
    throw new Error(`shader "${name}" 不存在（${path.relative(process.cwd(), dir)}）`)
  }
  const files = fs.readdirSync(dir).filter((f) => f.endsWith('.wgsl')).sort()
  if (files.length === 0) throw new Error(`shader "${name}" 目录里没有 .wgsl 文件: ${dir}`)
  const parts = files.map((f) => fs.readFileSync(path.join(dir, f), 'utf8'))
  const text = parts.join('')
  if (!text.trim()) throw new Error(`shader "${name}" 内容为空`)
  return { text, files, source: files.map((f) => `${name}/${f}`).join('+') }
}

function compress(text) {
  const raw = Buffer.from(text, 'utf8')
  if (typeof zlib.zstdCompressSync === 'function') {
    return zlib.zstdCompressSync(raw, { params: { [zlib.constants.ZSTD_c_compressionLevel]: ZSTD_LEVEL } })
  }
  // 退回外部 CLI（同版本 zstd 输出确定）
  try {
    return execFileSync('zstd', [`-${ZSTD_LEVEL}`, '-q', '-c', '-'], { input: raw, maxBuffer: 1 << 26 })
  } catch (err) {
    throw new Error(`本机 Node(${process.version}) 无 zlib.zstdCompressSync，且调用 zstd CLI 失败：` +
                    `${err.message}\n  → 升级 Node（≥ 22.15）或用 --no-compress（CMake: -DMAQUE_BUILTIN_SHADER_COMPRESS=OFF）`)
  }
}

const isIdent = (s) => /^[A-Za-z_][A-Za-z0-9_]*$/.test(s)

function cArray(name, buf) {
  const lines = []
  for (let i = 0; i < buf.length; i += 16) {
    const chunk = [...buf.subarray(i, i + 16)].map((b) => `0x${b.toString(16).padStart(2, '0')}`)
    lines.push('    ' + chunk.join(', ') + ',')
  }
  return `static const unsigned char ${name}[] = {\n${lines.join('\n')}\n};`
}

const HEADER = `/* 生成物：DO NOT EDIT —— 由 tools/shader_pack.js 从 assets/builtin/shaders/ 打包（构建流水线）。
 * 资产是唯一真相；本文件与 <shader>.cpp 是"按 shader 剔除"的开关点（未启用的不会被生成/链接）。 */
#ifndef MAQUE_BUILTIN_SHADERS_H
#define MAQUE_BUILTIN_SHADERS_H

#ifdef __cplusplus
extern "C" {
#endif

/* 一个内建 shader（注册表条目）。text() 首次调用解压并缓存，重复调用零开销。 */
typedef struct mq_builtin_shader {
    const char* name;                 /* shader 名 = 资产目录名（gfx 按名字取） */
    const char* source;               /* 资产相对路径（诊断/自省） */
    unsigned int raw_size;            /* 原始 WGSL 字节数（不含结尾 '\\0'） */
    const char* (*text)(void);        /* 取源码（解压 + 缓存）；失败返回 NULL */
} mq_builtin_shader;

extern const mq_builtin_shader mq_builtin_shaders[];
extern const unsigned int      mq_builtin_shader_count;

/* 按名字取内建 shader 源码（未编入 / 不存在 → NULL）。 */
const char* mq_builtin_shader_text(const char* name);

#ifdef __cplusplus
}
#endif
#endif /* MAQUE_BUILTIN_SHADERS_H */
`

function genShaderSource(name, text, blob, compressed) {
  const id = name.replace(/[^A-Za-z0-9_]/g, '_')
  if (!isIdent(id)) throw new Error(`shader 名 "${name}" 不能转成 C 标识符`)
  const raw = Buffer.from(text, 'utf8')
  const ratio = compressed ? `${blob.length} / ${raw.length} 字节（${Math.round(blob.length * 100 / raw.length)}%）` : '未压缩'
  return `/* 生成物：DO NOT EDIT —— tools/shader_pack.js
 * 源：assets/builtin/shaders/${name}/（${raw.length} 字节 → ${ratio}） */
#include "builtin_shaders.h"
#include "shader_decompress.h"

${cArray(`kBlob_${id}`, blob)}
#define BLOB_${id.toUpperCase()}_SIZE ${blob.length}u
#define RAW_${id.toUpperCase()}_SIZE ${raw.length}u
#define COMPRESSED_${id.toUpperCase()} ${compressed ? 1 : 0}

const char* mq_shader_${id}(void) {
    return mq_shader_unpack("${name}", kBlob_${id}, BLOB_${id.toUpperCase()}_SIZE,
                            RAW_${id.toUpperCase()}_SIZE, COMPRESSED_${id.toUpperCase()});
}
`
}

function genRegistry(shaders, empty) {
  const decls = shaders.map((s) => `extern const char* mq_shader_${s.id}(void);`).join('\n')
  const rows = shaders.map((s) =>
    `    { "${s.name}", "${s.source}", ${s.rawSize}u, mq_shader_${s.id} },`).join('\n')
  const countExpr = empty ? '0u'
    : '(unsigned int)(sizeof mq_builtin_shaders / sizeof mq_builtin_shaders[0])'
  return `/* 生成物：DO NOT EDIT —— tools/shader_pack.js
 * 注册表：只列出**本次编入**的 shader（剔除的 shader 不在这里 → 不链接、不进二进制）。 */
#include "builtin_shaders.h"

#include <string.h>

${decls ? decls + '\n\n' : ''}const mq_builtin_shader mq_builtin_shaders[] = {
${rows || '    { 0, 0, 0u, 0 },   /* 占位：全部 shader 都被剔除（C++ 不允许 0 长数组） */\n'}};
const unsigned int mq_builtin_shader_count = ${countExpr};

const char* mq_builtin_shader_text(const char* name) {
    if (!name) return NULL;
    for (unsigned int i = 0; i < mq_builtin_shader_count; ++i) {
        if (strcmp(mq_builtin_shaders[i].name, name) == 0) return mq_builtin_shaders[i].text();
    }
    return NULL;
}
`
}

function main() {
  const args = parseArgs(process.argv.slice(2))
  const all = discover(args.assets)
  let names = args.shaders === null ? all : args.shaders.split(',').map((s) => s.trim()).filter(Boolean)
  for (const n of names) {
    if (!all.includes(n)) {
      throw new Error(`--shaders 里的 "${n}" 不是资产目录下的 shader（可用：${all.join(', ') || '无'}）`)
    }
  }
  names = [...new Set(names)].sort()

  fs.mkdirSync(args.out, { recursive: true })
  // 清理上次生成、这次未编入的 <name>.cpp（注册表已不含它们，避免构建目录里留旧文件）
  for (const f of fs.readdirSync(args.out)) {
    if (!f.endsWith('.cpp') || f === 'builtin_shaders.cpp') continue
    if (!names.includes(f.slice(0, -4))) fs.rmSync(path.join(args.out, f))
  }
  const packed = []
  for (const name of names) {
    const { text, source } = readShader(args.assets, name)
    if (!text.includes('@vertex')) console.warn(`shader_pack: 警告：shader "${name}" 里没有 @vertex`)
    if (!text.includes('@fragment')) console.warn(`shader_pack: 警告：shader "${name}" 里没有 @fragment`)
    let blob = Buffer.from(text, 'utf8')
    let compressed = 0
    if (args.compress) {
      blob = compress(text)
      compressed = 1
    }
    fs.writeFileSync(path.join(args.out, `${name}.cpp`), genShaderSource(name, text, blob, compressed))
    packed.push({ name, source, rawSize: Buffer.byteLength(text, 'utf8'), id: name.replace(/[^A-Za-z0-9_]/g, '_'),
                  compressed, blobSize: blob.length })
  }

  fs.writeFileSync(path.join(args.out, 'builtin_shaders.h'), HEADER)
  fs.writeFileSync(path.join(args.out, 'builtin_shaders.cpp'), genRegistry(packed, packed.length === 0))

  const kind = args.compress ? 'zstd' : '原文'
  console.log(`shader_pack: ${packed.length} 个 shader（${kind}）→ ${path.relative(process.cwd(), args.out) || args.out}`)
  for (const p of packed) {
    console.log(`  ${p.name.padEnd(10)} ${p.rawSize} → ${p.blobSize} 字节${p.compressed ? `（${Math.round(p.blobSize * 100 / p.rawSize)}%）` : ''}`)
  }
  if (packed.length === 0) console.warn('shader_pack: 警告：没有任何内建 shader 编入（全部被剔除？见 MAQUE_SHADER_*）')
}

try {
  main()
} catch (err) {
  console.error(`shader_pack: 失败：${err.message}`)
  process.exit(1)
}
