#!/usr/bin/env bash
# 场景雾 + 色调映射端到端（plans/27-threejs-feature-migration.md M1-2/M1-3）：
#   scenes/pbr/atmosphere{,_off,_reinhard,_none}.scn 同一几何/相机，只改 fog/tone。
#   ① 雾：近球（距离=near）与无雾一致；远球（距离=far）明显混入雾色（偏蓝、变暗）
#   ② tone mapping：ACES vs Reinhard 中球平均亮度下降；None 比 ACES 更亮（高光裁剪）
#   ③ 同一帧两次渲染逐字节一致；④ shader 无 validation 错误
set -u
. "$(dirname "$0")/maque_python.sh" 2>/dev/null || true
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"
PLAYER="$1"; SCN="$2"; WORK="$3"

_mq_is_windows() { case "$(uname -s 2>/dev/null || echo unknown)" in MINGW*|MSYS*|CYGWIN*) return 0 ;; *) return 1 ;; esac; }
if ! _mq_is_windows && [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    echo "atmosphere_e2e: skip（无显示环境）"; exit 77
fi
[ -f "$SCN" ] || { echo "atmosphere_e2e: skip（缺 $SCN）"; exit 77; }
DIR="$(dirname "$SCN")"
fail() { echo "atmosphere_e2e: FAIL $*" >&2; exit 1; }

rm -rf "$WORK"; mkdir -p "$WORK"
unset MAQUE_HEADLESS MAQUE_PRESENT MAQUE_CAPTURE MAQUE_CAPTURE_WGPU MAQUE_CAPTURE_FRAMES \
      MAQUE_MAX_FRAMES MAQUE_IMGUI MAQUE_SHOT MAQUE_SHOT_SEQ MAQUE_ENV_IBL MAQUE_RENDER3D_INFO 2>/dev/null || true
export MAQUE_WINDOW=0x0

shot() {  # shot <name> <scene>
    timeout 90 "$PLAYER" --shot "$WORK/$1.ppm" "$2" >"$WORK/$1.txt" 2>&1 \
        || { cat "$WORK/$1.txt"; fail "$1 渲染失败"; }
    grep -q "runtime: shot" "$WORK/$1.txt" || { cat "$WORK/$1.txt"; fail "$1 截图未写入"; }
    if grep -qiE 'validation|WGSL|pipeline.*error|shader.*error' "$WORK/$1.txt"; then
        cat "$WORK/$1.txt"; fail "$1 shader/管线错误"
    fi
}

shot fog        "$SCN"
shot fog_off    "$DIR/atmosphere_off.scn"
shot reinhard   "$DIR/atmosphere_reinhard.scn"
shot none       "$DIR/atmosphere_none.scn"
shot fog_b      "$SCN"

cmp -s "$WORK/fog.ppm" "$WORK/fog_b.ppm" || fail "两次渲染不一致（非确定性）"

"$PY" - "$WORK" <<'PY' || exit 1
import sys
from pathlib import Path

work = Path(sys.argv[1])


def load(name):
    d = (work / f"{name}.ppm").read_bytes()
    magic, dims, _max, px = d.split(b'\n', 3)
    assert magic == b'P6', magic
    w, h = map(int, dims.split())
    return w, h, px


def region(px, w, cx, cy, r, bg=90):
    vals = []
    for y in range(cy - r, cy + r):
        for x in range(cx - r, cx + r):
            i = (y * w + x) * 3
            c = (px[i], px[i + 1], px[i + 2])
            if sum(c) > bg:
                vals.append(c)
    n = len(vals)
    mean = tuple(sum(v[k] for v in vals) / n for k in range(3)) if n else (0.0, 0.0, 0.0)
    lum = sum(0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2] for c in vals) / n if n else 0.0
    return n, mean, lum

# 相机 z=5、fov35：近球 x=-2.5 → ~259；中球 x=0/z=-5 → 640；远球 x=4/z=-10 → 843
r_near = (259, 270, 55)
r_mid = (640, 255, 28)
r_far = (843, 250, 20)

w, h, fog = load('fog')
_, _, fog_off = load('fog_off')
_, _, rein = load('reinhard')
_, _, none = load('none')

fn, fm, fl = region(fog, w, *r_near)
on_, om, ol = region(fog_off, w, *r_near)
mid_n, mid_m, mid_l = region(fog, w, *r_mid)
mid_on, mid_om, mid_ol = region(fog_off, w, *r_mid)
far_n, far_m, far_l = region(fog, w, *r_far)
far_on, far_om, far_ol = region(fog_off, w, *r_far)
rn, rm, rl = region(rein, w, *r_mid)
nn, nm, nl = region(none, w, *r_mid)

near_delta = max(abs(fm[k] - om[k]) for k in range(3))
far_delta = max(abs(far_m[k] - far_om[k]) for k in range(3))
print(f'atmosphere_e2e: near={fm} off={om} sum_delta={near_delta:.1f}')
print(f'atmosphere_e2e: far={far_m} off={far_om} delta={far_delta:.1f} n={far_n}/{far_on}')
print(f'atmosphere_e2e: ACES mid lum={mid_l:.1f} Reinhard={rl:.1f} None={nl:.1f}')

ok = True
ok &= fn > 500 and on_ > 500
ok &= near_delta < 3.0                 # 距离=near：雾因子 0，颜色不变
ok &= far_delta > 40.0                 # 距离=far：明显混入雾色
ok &= far_m[2] > far_m[0] + 10.0       # 雾色偏蓝（b > r）
ok &= far_om[0] > far_om[2] + 10.0     # 无雾时球是暖色（r > b）
ok &= mid_l > rl + 15.0                # ACES 比 Reinhard 亮
ok &= nl > mid_l + 8.0                 # None（无压缩）比 ACES 亮
sys.exit(0 if ok else 1)
PY
echo "atmosphere_e2e: PASS（雾近端不变/远端混色 + ACES/Reinhard/None 亮度差 + 确定性）"
