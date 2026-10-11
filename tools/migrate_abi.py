#!/usr/bin/env python3
"""把模块头迁移到 DSL：转换 → 合并进 schema.yaml（迁移辅助，非构建依赖）。

用法：python3 tools/migrate_abi.py <header> <module> [<namespace>] [--apply]
不带 --apply 只打印将要写入的 module_abi 段（干跑）。
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCHEMA = ROOT / 'modules/core/data/schema.yaml'


def convert(header: str, module: str) -> str:
    r = subprocess.run([sys.executable, str(ROOT / 'tools/hdr2abi.py'), header, module],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(r.stderr)
    return r.stdout.rstrip('\n')


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
    schema = SCHEMA.read_text()
    SCHEMA.write_text(merge(schema, ns, block))
    print(f'merged module_abi for {module} into namespace {ns}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
