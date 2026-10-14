#!/usr/bin/env bash
# codegen 漂移校验：重新生成到临时目录，与**入库的** generated/ 逐字节比对。
# 目的：schema/（唯一真相）改动后忘记提交生成物时，CI/本地立即发现（生成物已入库，docs/21）。
set -u
: "${MAQUE_SCHEMA:?}" "${MAQUE_PARSER:?}" "${MAQUE_TRACKED:?}"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

cd "$(dirname "$MAQUE_PARSER")"
node "$(basename "$MAQUE_PARSER")" --schema "$MAQUE_SCHEMA" --out "$TMP" >/dev/null || {
  echo "codegen_drift: codegen 失败" >&2; exit 1; }

if diff -r -q "$TMP" "$MAQUE_TRACKED" > /tmp/codegen_drift.diff 2>&1; then
  echo "codegen_drift: ok（生成物与 schema/ 一致）"
  exit 0
fi
echo "codegen_drift: FAIL —— generated/ 与 schema/ 不一致（请提交重新生成的结果）" >&2
sed -n '1,20p' /tmp/codegen_drift.diff >&2
exit 1
