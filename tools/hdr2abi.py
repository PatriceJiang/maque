#!/usr/bin/env python3
"""从头文件反向生成 module_abi DSL（迁移辅助工具，非构建依赖）。

用法：python3 tools/hdr2abi.py <header> <module> [<namespace>]
输出：可直接粘贴进 modules/core/data/schema/ 的 module_abi 段。
"""
import re
import subprocess
import sys
from pathlib import Path


def load(path: str) -> str:
    p = Path(path)
    if p.exists():
        cur = p.read_text(errors='ignore')
        # shim（已改为 #include "abi/..."）或生成物 → 回落到 git 历史里的原始头
        if 'abi/' not in cur and 'DO NOT EDIT' not in cur[:200]:
            return cur
    # 已被换成 shim（或不存在）→ 从 git 历史取原始头
    rel = str(p)
    for rev in ('HEAD', 'HEAD~1', 'HEAD~2', 'HEAD~3'):
        r = subprocess.run(['git', 'show', f'{rev}:{rel}'], capture_output=True, text=True)
        if r.returncode == 0 and 'module_abi' not in r.stdout:
            return r.stdout
    return p.read_text()


def strip(t: str) -> str:
    t = re.sub(r'/\*.*?\*/', ' ', t, flags=re.S)
    t = re.sub(r'//[^\n]*', ' ', t)
    t = re.sub(r'#ifdef __cplusplus.*?#endif', ' ', t, flags=re.S)
    return t


def parse(txt: str):
    body = strip(txt)
    opaque = []
    for m in re.finditer(r'typedef struct (\w+) \1;', body):
        if m.group(1) not in opaque:
            opaque.append(m.group(1))
    structs = []
    for m in re.finditer(r'typedef struct (\w+)\s*\{(.*?)\}\s*(\w+)\s*;', body, re.S):
        name, fields_txt = m.group(1), m.group(2)
        fields = []
        for seg in fields_txt.split(';'):
            seg = re.sub(r'\s+', ' ', seg).strip()
            if not seg:
                continue
            # 函数指针字段：ret (*name)(args)
            fp = re.match(r'^(.+?)\(\*\s*(\w+)\)\s*\((.*)\)$', seg)
            if fp:
                fields.append((fp.group(2), f'{fp.group(1).strip()} (*)({fp.group(3)})'))
                continue
            arr = re.match(r'^(.+?)\s+(\w+)(\[[^\]]*\])$', seg)
            if arr:
                fields.append((arr.group(2), f'{arr.group(1).strip()}{arr.group(3)}'))
                continue
            # 合并声明：float x, y
            parts = [p.strip() for p in seg.split(',')]
            head = re.match(r'^(.+?)\s+(\w+)$', parts[0])
            if not head:
                continue
            typ = head.group(1).strip()
            for p in parts:
                mm = re.match(r'^(.+?)\s+(\w+)$', p)
                if mm:
                    fields.append((mm.group(2), mm.group(1).strip()))
                elif re.match(r'^\w+$', p):
                    fields.append((p, typ))
        structs.append((name, fields))
    enums = []
    for m in re.finditer(r'enum\s*(\w*)\s*\{(.*?)\}\s*(\w*)\s*;', body, re.S):
        vals, nxt = [], 0
        for item in m.group(2).split(','):
            item = item.strip()
            if not item:
                continue
            if '=' in item:
                n, v = [x.strip() for x in item.split('=', 1)]
                vals.append((n, v))
                try:
                    nxt = int(v, 0) + 1
                except ValueError:
                    nxt = None
            else:
                vals.append((item, str(nxt) if nxt is not None else '0'))
                nxt = nxt + 1 if nxt is not None else None
        enums.append((m.group(1) or None, vals, m.group(3) or None))
    macros = []
    for m in re.finditer(r'#define\s+(\w+)\s+([^\n]+)', body):
        name, val = m.group(1), m.group(2).strip()
        if name.endswith('_H') or name in ('MQ_PHYSICS_ABI_VERSION', 'MQ_ASSET_ABI_VERSION'):
            continue
        macros.append((name, val))
    typedefs = []
    for m in re.finditer(r'typedef\s+([A-Za-z_][\w \*]*?)\(\*\s*(\w+)\)\s*\(([^;]*)\)\s*;', body):
        typedefs.append((m.group(2), f'{m.group(1).strip()} (*)({m.group(3).strip()})'))
    fns = []
    for m in re.finditer(r'\n([A-Za-z_][\w \*]*?)\s+(mq_\w+)\s*\(([^;{]*?)\)\s*;', body, re.S):
        ret, name, args = m.group(1).strip(), m.group(2), m.group(3).strip()
        parsed = []
        if args and args != 'void':
            for a in re.split(r',(?![^()]*\))', args):
                a = re.sub(r'\s+', ' ', a.strip())
                if not a:
                    continue
                fp = re.match(r'^(.+?)\(\*\s*(\w+)\)\s*\((.*)\)$', a)
                if fp:
                    parsed.append((fp.group(2), f'{fp.group(1).strip()} (*)({fp.group(3)})'))
                    continue
                arr = re.match(r'^(.+?)\s+(\w+)(\[[^\]]*\])$', a)
                if arr:
                    parsed.append((arr.group(2), f'{arr.group(1).strip()}{arr.group(3)}'))
                    continue
                mm = re.match(r'^(.+?)\s*(\w+)$', a)
                if mm:
                    parsed.append((mm.group(2), mm.group(1).strip()))
        fns.append((name, ret, parsed))
    return opaque, structs, enums, macros, typedefs, fns


def q(s: str) -> str:
    return '"' + s.replace('"', '\\"') + '"'


def main() -> int:
    header, module = sys.argv[1], sys.argv[2]
    ns = sys.argv[3] if len(sys.argv) > 3 else module
    txt = load(header)
    opaque, structs, enums, macros, typedefs, fns = parse(txt)
    out = [f'- namespace: {ns}', '  version: 1', '  module_abi:',
           f'    module: {module}', f'    abi_name: "maque.{module}"']
    incs = sorted(set(re.findall(r'#include\s+(<[^>]+>|"[^"]+")', txt)))
    if incs:
        out.append('    includes: [ ' + ', '.join(q(i) for i in incs) + ' ]')
    if opaque:
        out.append('    opaque: [ ' + ', '.join(opaque) + ' ]')
    if typedefs:
        out.append('    typedefs:')
        for n, t in typedefs:
            out.append(f'      - {{ name: {n}, type: {q(t)} }}')
    if macros:
        out.append('    macros:')
        for n, v in macros:
            out.append(f'      - {{ name: {n}, value: {q(v)} }}')
    if enums:
        out.append('    enums:')
        for name, vals, _ in enums:
            out.append(f'      - {"name: " + name if name else "values:"}' if name else '      - values:')
            if name:
                out.append('        values:')
            for n, v in vals:
                out.append(f'          - {{ name: {n}, value: {v} }}')
    if structs:
        out.append('    structs:')
        for name, fields in structs:
            out.append(f'      - name: {name}')
            out.append('        fields:')
            for fn, ft in fields:
                out.append(f'          - {{ name: {fn}, type: {q(ft)} }}')
    out.append('    fns:')
    for name, ret, args in fns:
        a = ', '.join(f'{{ name: {n}, type: {q(t)} }}' for n, t in args)
        out.append(f'      - {{ fn: {name}, returns: {q(ret)}, args: [ {a} ] }}')
    print('\n'.join(out))
    print(f'# 统计: opaque={len(opaque)} structs={len(structs)} enums={len(enums)} macros={len(macros)} typedefs={len(typedefs)} fns={len(fns)}', file=sys.stderr)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
