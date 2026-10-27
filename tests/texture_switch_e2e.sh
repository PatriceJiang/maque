#!/usr/bin/env bash
# 场景切换纹理缓存回归（khronos gallery 的 "BoomBoxWithAxes 贴图错"）：
# 两个场景的组件资源路径**逐字相同**（textures/t.png），但各自目录下是不同图片。
# 切场景时 VFS 挂载点变了；若 tex_cache（键=相对路径）不清空，B 会复用 A 的纹理。
#
# 判定链：
#   ① A 场景正常渲染（红色纹理）→ 切到 B（蓝色纹理，同相对路径）；
#   ② 切场景后画面中心像素应为 B 的蓝色（不是 A 的红色）；
#   ③ 日志出现 B 的 title（确实完成了切换）。
set -u

PLAYER="$1"
DATA_DIR="$2"
WORK="$3"

# shellcheck source=maque_python.sh
. "$(dirname "$0")/maque_python.sh"

rm -rf "$WORK"
mkdir -p "$WORK"
export MAQUE_HEADLESS=1

fail() { echo "texture_switch_e2e: FAIL $*" >&2; exit 1; }

cp -R "$DATA_DIR/a" "$DATA_DIR/b" "$WORK/"
mkdir -p "$WORK/a/textures" "$WORK/b/textures"

# 生成两张纯色 PNG（只依赖 Python 标准库 zlib/struct，无需 Pillow）。
"$PY" - "$WORK" <<'PY' || fail "生成测试纹理失败"
import struct, sys, zlib, os
root = sys.argv[1]
def png(path, rgb):
    w = h = 8
    raw = b''.join(b'\x00' + bytes(rgb) * w for _ in range(h))
    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)
    data = (b'\x89PNG\r\n\x1a\n'
            + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw, 9))
            + chunk(b'IEND', b''))
    open(path, 'wb').write(data)
png(os.path.join(root, 'a', 'textures', 't.png'), (255, 0, 0, 255))
png(os.path.join(root, 'b', 'textures', 't.png'), (0, 0, 255, 255))
print('textures ok')
PY

# 保证 A 的纹理先进缓存，再切到 B。
timeout 60 "$PLAYER" --shot "$WORK/out.ppm" --shot-frame 5 "$WORK/a/scene.scn" > "$WORK/run.log" 2>&1
[ "$?" -eq 0 ] || { cat "$WORK/run.log"; fail "player 运行失败"; }
grep -q "title='texswitch-b'" "$WORK/run.log" || { cat "$WORK/run.log"; fail "没有切换到 B 场景"; }

"$PY" - "$WORK/out.ppm" <<'PY' || fail "切场景后纹理不对（复用了旧场景缓存）"
import sys
d = open(sys.argv[1], 'rb').read()
assert d[:2] == b'P6', d[:8]
parts = d.split(b'\n', 3)
w, h = map(int, parts[1].split())
assert w == 64 and h == 64, (w, h)
px = parts[3]
i = ((h // 2) * w + w // 2) * 3
r, g, b = px[i], px[i + 1], px[i + 2]
assert b > 200 and r < 60 and g < 60, f'期望蓝色（B 场景纹理），实际 rgb=({r},{g},{b})'
print(f'texture_switch_e2e: center rgb=({r},{g},{b})')
PY

echo "texture_switch_e2e: ok"
exit 0
