#!/usr/bin/env python3
"""把模块头迁移到 DSL：转换 → 合并进 schema/ 目录（迁移辅助，非构建依赖）。

用法：python3 tools/migrate_abi.py <header> <module> [<namespace>] [--apply]
不带 --apply 只打印将要写入的 module_abi 段（干跑）。
--apply 时按命名空间找到 schema/ 下声明它的那个文件（一个文件一个 namespace），
在其中补 module_abi 段；找不到就报错（先建文件并在 schema/schema.yaml 登记）。
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCHEMA_DIR = ROOT / 'modules/core/data/schema'


def schema_files() -> list[Path]:
    """schema 目录内全部 schema 文件（入口 schema.yaml 除外）。"""
    return sorted(p for p in SCHEMA_DIR.rglob('*.yaml') if p.name != 'schema.yaml')


def find_namespace_file(ns: str) -> Path:
    """定位声明 `- namespace: <ns>` 的文件（同一 namespace 只应有一个声明文件）。"""
    hits = [p for p in schema_files()
            if re.search(rf'^- namespace: {re.escape(ns)}\s*$', p.read_text(), re.M)]
    if not hits:
        raise SystemExit(f'namespace {ns} 不存在于 {SCHEMA_DIR}/（先建文件并在 schema.yaml 登记）')
    if len(hits) > 1:
        raise SystemExit(f'namespace {ns} 声明在多个文件: {", ".join(str(p) for p in hits)}')
    return hits[0]


def merge(schema: str, ns: str, block: str) -> str:
    body = '  module_abi:\n' + block.split('  module_abi:\n', 1)[1]
    m = re.search(rf'^- namespace: {ns}\n(?:.*\n)*?(?=\n- (?:namespace|interface): |\Z)', schema, re.M)
    if m:
        seg = m.group(0)
        if 'module_abi:' in seg:
            raise SystemExit(f'namespace {ns} already has module_abi')
        return schema.replace(seg, seg.rstrip('\n') + '\n' + body.rstrip('\n') + '\n', 1)
    return schema.rstrip('\n') + '\n\n' + f'- namespace: {ns}\n  version: 1\n' + body.rstrip('\n') + '\n'


def main() -> int:
    header, module = sys.argv[1], sys.argv[2]
    ns = module
    args = [a for a in sys.argv[3:] if not a.startswith('--')]
    if args:
        ns = args[0]
    apply = '--apply' in sys.argv
    block = convert(header, module)
    if not apply:
        print(block)
        return 0
    target = find_namespace_file(ns)
    target.write_text(merge(target.read_text(), ns, block))
    print(f'merged module_abi for {module} into namespace {ns} ({target.relative_to(ROOT)})')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
