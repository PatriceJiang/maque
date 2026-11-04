"""生成 samples/tank 的 FC《坦克大战》风格像素资源（确定性、可重跑）。

设计：
  * 场景 = 640x480；战场 = 26x26 个 16px 单元（= NES 的 26x26 个 8px 单元放大 2 倍）；
    地图按 13x13 大格编写，每个大格铺成 2x2 单元 —— 与 1990 FC 版的数据结构一致。
  * 坦克 32x32；每个朝向单独出一张图（不在运行时做 90° 纹理旋转），像素边界不糊。
  * 砖块/钢块/水/树/冰都是 16x16 单元；树画在坦克之上（运行时用 node z-index）。
  * 文字用自制 8x8 位图字体：HUD 1x，标题/结算 2x；不依赖系统 TTF 的形状。
  * 只写 samples/tank/art 下的生成物；脚本本身是唯一真相，重跑逐字节确定。

用法：python tools/make_tank_art.py
"""
from __future__ import annotations

import math
import os
import random

from PIL import Image, ImageDraw

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
OUT = os.path.join(ROOT, "samples", "tank", "art")


def save(img: Image.Image, name: str) -> None:
    path = os.path.join(OUT, name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    img.save(path)
    print(f"  {name:28s} {img.size[0]}x{img.size[1]}  {os.path.getsize(path)} bytes")


# ---------------------------------------------------------------- 5x7 位图字体
FONT = {
    "A": ("01110", "10001", "10001", "11111", "10001", "10001", "10001"),
    "B": ("11110", "10001", "10001", "11110", "10001", "10001", "11110"),
    "C": ("01110", "10001", "10000", "10000", "10000", "10001", "01110"),
    "D": ("11110", "10001", "10001", "10001", "10001", "10001", "11110"),
    "E": ("11111", "10000", "10000", "11110", "10000", "10000", "11111"),
    "F": ("11111", "10000", "10000", "11110", "10000", "10000", "10000"),
    "G": ("01110", "10001", "10000", "10111", "10001", "10001", "01110"),
    "H": ("10001", "10001", "10001", "11111", "10001", "10001", "10001"),
    "I": ("11111", "00100", "00100", "00100", "00100", "00100", "11111"),
    "J": ("00111", "00010", "00010", "00010", "00010", "10010", "01100"),
    "K": ("10001", "10010", "10100", "11000", "10100", "10010", "10001"),
    "L": ("10000", "10000", "10000", "10000", "10000", "10000", "11111"),
    "M": ("10001", "11011", "10101", "10101", "10001", "10001", "10001"),
    "N": ("10001", "11001", "10101", "10011", "10001", "10001", "10001"),
    "O": ("01110", "10001", "10001", "10001", "10001", "10001", "01110"),
    "P": ("11110", "10001", "10001", "11110", "10000", "10000", "10000"),
    "Q": ("01110", "10001", "10001", "10001", "10101", "10010", "01101"),
    "R": ("11110", "10001", "10001", "11110", "10100", "10010", "10001"),
    "S": ("01111", "10000", "10000", "01110", "00001", "00001", "11110"),
    "T": ("11111", "00100", "00100", "00100", "00100", "00100", "00100"),
    "U": ("10001", "10001", "10001", "10001", "10001", "10001", "01110"),
    "V": ("10001", "10001", "10001", "10001", "10001", "01010", "00100"),
    "W": ("10001", "10001", "10001", "10101", "10101", "11011", "10001"),
    "X": ("10001", "10001", "01010", "00100", "01010", "10001", "10001"),
    "Y": ("10001", "10001", "01010", "00100", "00100", "00100", "00100"),
    "Z": ("11111", "00001", "00010", "00100", "01000", "10000", "11111"),
    "0": ("01110", "10001", "10011", "10101", "11001", "10001", "01110"),
    "1": ("00100", "01100", "00100", "00100", "00100", "00100", "01110"),
    "2": ("01110", "10001", "00001", "00010", "00100", "01000", "11111"),
    "3": ("11110", "00001", "00001", "01110", "00001", "00001", "11110"),
    "4": ("00010", "00110", "01010", "10010", "11111", "00010", "00010"),
    "5": ("11111", "10000", "10000", "11110", "00001", "00001", "11110"),
    "6": ("01110", "10000", "10000", "11110", "10001", "10001", "01110"),
    "7": ("11111", "00001", "00010", "00100", "01000", "01000", "01000"),
    "8": ("01110", "10001", "10001", "01110", "10001", "10001", "01110"),
    "9": ("01110", "10001", "10001", "01111", "00001", "00001", "01110"),
    " ": ("00000",) * 7,
    ".": ("00000", "00000", "00000", "00000", "00000", "00110", "00110"),
    ",": ("00000", "00000", "00000", "00000", "00110", "00110", "00100"),
    ":": ("00000", "00110", "00110", "00000", "00110", "00110", "00000"),
    "-": ("00000", "00000", "00000", "11111", "00000", "00000", "00000"),
    "/": ("00001", "00010", "00010", "00100", "01000", "01000", "10000"),
    "!": ("00100", "00100", "00100", "00100", "00100", "00000", "00100"),
    "?": ("01110", "10001", "00001", "00010", "00100", "00000", "00100"),
    "'": ("00100", "00100", "00000", "00000", "00000", "00000", "00000"),
    "(": ("00010", "00100", "01000", "01000", "01000", "00100", "00010"),
    ")": ("01000", "00100", "00010", "00010", "00010", "00100", "01000"),
    "+": ("00000", "00100", "00100", "11111", "00100", "00100", "00000"),
    "<": ("00010", "00100", "01000", "10000", "01000", "00100", "00010"),
    ">": ("01000", "00100", "00010", "00001", "00010", "00100", "01000"),
    "=": ("00000", "00000", "11111", "00000", "11111", "00000", "00000"),
}


def glyph_image(ch: str, scale: int = 1) -> Image.Image:
    img = Image.new("RGBA", (8 * scale, 8 * scale), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    rows = FONT.get(ch, FONT["?"])
    for y, row in enumerate(rows):
        for x, bit in enumerate(row):
            if bit == "1":
                d.rectangle([x * scale + scale, y * scale,
                             x * scale + 2 * scale - 1, y * scale + scale - 1],
                            fill=(255, 255, 255, 255))
    return img


def make_glyphs() -> None:
    for ch in FONT:
        if ch == " ":
            continue
        cp = ord(ch)
        for scale, sub in ((1, "glyph8"), (2, "glyph16")):
            img = glyph_image(ch, scale)
            path = os.path.join(OUT, sub, f"{cp}.png")
            os.makedirs(os.path.dirname(path), exist_ok=True)
            img.save(path)
    print(f"  glyph8/glyph16              {len(FONT) - 1} printable glyphs")


# ---------------------------------------------------------------- 背景 / 战场
def make_backgrounds() -> None:
    w, h = 640, 480
    img = Image.new("RGB", (w, h), (5, 7, 11))
    d = ImageDraw.Draw(img)
    # 极暗的纵向扫描线：全屏背景有一点 CRT 质感，不抢战场。
    for y in range(0, h, 4):
        d.line([(0, y), (w, y)], fill=(7, 9, 13))
    for x in range(0, w, 64):
        d.line([(x, 0), (x, h)], fill=(6, 8, 12))
    save(img, "bg.png")

    img = Image.new("RGB", (416, 416), (17, 20, 25))
    d = ImageDraw.Draw(img)
    for y in range(0, 416, 16):
        d.line([(0, y), (415, y)], fill=(22, 26, 32))
    for x in range(0, 416, 16):
        d.line([(x, 0), (x, 415)], fill=(22, 26, 32))
    rng = random.Random(1990)
    for _ in range(260):
        x, y = rng.randrange(416), rng.randrange(416)
        d.point((x, y), fill=(27, 31, 38))
    save(img, "field.png")

    # 战场外框（432x432，中心透明），压在 field 之上。
    img = Image.new("RGBA", (432, 432), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, 431, 431], outline=(116, 124, 138, 255), width=4)
    d.rectangle([4, 4, 427, 427], outline=(44, 50, 60, 255), width=2)
    d.rectangle([8, 8, 423, 423], outline=(8, 10, 14, 255), width=2)
    save(img, "frame.png")

    # 右侧 HUD 面板。
    img = Image.new("RGB", (176, 416), (8, 10, 14))
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, 2, 415], fill=(50, 56, 68))
    d.rectangle([3, 0, 5, 415], fill=(20, 24, 31))
    for y in range(0, 416, 64):
        d.line([(8, y), (175, y)], fill=(12, 15, 20))
    save(img, "hud_bg.png")


# ---------------------------------------------------------------- 地块
def make_tiles() -> None:
    # 砖块：2 行砖，交错缝。每格 16x16 是一个可击碎的 8x8 源像素块放大 2 倍。
    img = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, 15, 15], fill=(74, 35, 24, 255))
    for row in range(2):
        y = row * 8
        off = 0 if row == 0 else 4
        for i in range(-1, 3):
            x = i * 8 + off
            d.rectangle([x + 1, y + 1, x + 6, y + 6], fill=(178, 73, 40, 255))
            d.line([(x + 1, y + 1), (x + 6, y + 1)], fill=(238, 128, 74, 255))
            d.line([(x + 1, y + 1), (x + 1, y + 6)], fill=(212, 97, 54, 255))
            d.line([(x + 2, y + 6), (x + 6, y + 6)], fill=(118, 43, 26, 255))
    save(img, "tile_brick.png")

    # 钢块：四块 7x7 的亮面钢砖，普通子弹打不穿。
    img = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, 15, 15], fill=(42, 48, 58, 255))
    for y in (1, 9):
        for x in (1, 9):
            d.rectangle([x, y, x + 6, y + 6], fill=(148, 158, 170, 255))
            d.line([(x, y), (x + 6, y)], fill=(232, 238, 244, 255))
            d.line([(x, y), (x, y + 6)], fill=(198, 208, 220, 255))
            d.line([(x + 1, y + 6), (x + 6, y + 6)], fill=(88, 96, 108, 255))
            d.line([(x + 6, y + 1), (x + 6, y + 6)], fill=(108, 116, 128, 255))
            d.point((x + 3, y + 2), fill=(250, 252, 255, 255))
    save(img, "tile_steel.png")

    # 水：两帧，蓝底 + 高光波纹；坦克不能过，子弹可以。
    for k in range(2):
        img = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
        d = ImageDraw.Draw(img)
        d.rectangle([0, 0, 15, 15], fill=(16, 42, 104, 255))
        for y in range(1, 16, 4):
            shift = 2 if (k + y // 4) % 2 else 0
            d.line([(shift, y), (shift + 8, y)], fill=(42, 102, 196, 255))
            d.line([(shift + 3, y + 1), (shift + 11, y + 1)], fill=(104, 180, 250, 255))
            d.line([(shift + 6, y), (shift + 11, y)], fill=(166, 220, 255, 255))
        save(img, f"tile_water{k}.png")

    # 树：半透明覆盖层，坦克从下面穿过时还能隐约看到。
    img = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    rng = random.Random(5150)
    for cy in (3, 8, 12):
        for cx in (3, 8, 13):
            r = 3 + (cx + cy) % 2
            d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(16, 58, 28, 255))
            d.ellipse([cx - r + 1, cy - r + 1, cx + r - 1, cy + r - 1],
                      fill=(30 + rng.randrange(20), 86 + rng.randrange(40), 38, 255))
            d.point((cx - 1, cy - 1), fill=(96, 160, 66, 255))
    # 打一些透明孔，避免树变成实心绿块。
    for _ in range(18):
        x, y = rng.randrange(16), rng.randrange(16)
        img.putpixel((x, y), (0, 0, 0, 0))
    save(img, "tile_tree.png")

    # 冰：淡蓝反光 + 裂纹；坦克会打滑。
    img = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, 15, 15], fill=(158, 196, 220, 255))
    d.line([(0, 1), (15, 1)], fill=(214, 236, 248, 255))
    d.line([(0, 14), (15, 14)], fill=(112, 154, 184, 255))
    for pts, c in (
        ([(2, 4), (6, 8), (5, 12)], (240, 250, 255, 255)),
        ([(10, 2), (8, 7), (12, 11), (14, 9)], (96, 140, 176, 255)),
        ([(3, 13), (7, 10)], (224, 242, 252, 255)),
    ):
        d.line(pts, fill=c)
    save(img, "tile_ice.png")


# ---------------------------------------------------------------- 坦克
TANK_PALETTES = {
    "player0": ((196, 148, 34), (248, 210, 74), (110, 78, 18)),
    "player1": ((204, 156, 36), (255, 220, 88), (112, 80, 20)),
    "player2": ((212, 164, 38), (255, 228, 112), (116, 82, 22)),
    "player3": ((224, 178, 46), (255, 240, 150), (120, 86, 22)),
    "basic": ((132, 136, 140), (196, 200, 204), (64, 68, 74)),
    "fast": ((144, 166, 184), (218, 238, 252), (66, 84, 100)),
    "power": ((84, 152, 68), (154, 224, 112), (40, 76, 34)),
    "armor4": ((72, 140, 76), (132, 208, 116), (32, 72, 38)),
    "armor3": ((164, 150, 44), (236, 216, 92), (78, 66, 24)),
    "armor2": ((174, 100, 40), (242, 166, 74), (86, 44, 22)),
    "armor1": ((164, 64, 56), (232, 122, 104), (76, 28, 26)),
}


def tank_up(kind: str, level: int = 0) -> Image.Image:
    body, light, dark = TANK_PALETTES[kind]
    img = Image.new("RGBA", (32, 32), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    # 履带（左右各一条，带分段）
    for x0 in (2, 24):
        d.rectangle([x0, 5, x0 + 5, 28], fill=(38, 42, 48, 255))
        d.rectangle([x0 + 1, 5, x0 + 5, 28], fill=(70, 76, 84, 255))
        for y in range(6, 29, 4):
            d.rectangle([x0 + 1, y, x0 + 5, y + 1], fill=(22, 24, 28, 255))
            d.point((x0 + 2, y + 2), fill=(112, 118, 126, 255))

    # 车体：前方收窄，后方宽，带明暗面。
    d.polygon([(8, 8), (23, 8), (25, 26), (6, 26)], fill=dark)
    d.rectangle([8, 8, 23, 25], fill=body)
    d.polygon([(8, 8), (23, 8), (22, 12), (9, 12)], fill=light)
    d.line([(8, 13), (8, 25)], fill=light)
    d.line([(9, 25), (22, 25)], fill=dark)

    # 炮塔：圆盘 + 中心高光。
    d.ellipse([9, 9, 22, 22], fill=dark)
    d.ellipse([10, 10, 21, 21], fill=body)
    d.ellipse([12, 11, 19, 17], fill=light)
    d.point((15, 13), fill=(255, 255, 230, 255))

    # 炮管：等级越高越长/越多。
    barrels = [(14, 1, 17, 15)] if level < 2 else [(12, 1, 14, 15), (17, 1, 19, 15)]
    if level == 1:
        barrels = [(14, 0, 17, 15)]
    for x0, y0, x1, y1 in barrels:
        d.rectangle([x0, y0, x1, 13], fill=dark)
        d.rectangle([x0 + 1, y0, x1 - 1, 12], fill=light)
        d.rectangle([x0, y0, x1, y0 + 1], fill=(242, 246, 250, 255))

    # 等级标记（炮塔下方的小星点），玩家升级一眼可见。
    for i in range(level):
        x = 12 + i * 4
        d.point((x, 23), fill=(255, 250, 190, 255))
        d.point((x + 1, 23), fill=(140, 110, 30, 255))

    # 兵种特征
    if kind == "fast":
        d.rectangle([3, 29, 8, 30], fill=(190, 210, 226, 255))
        d.rectangle([23, 29, 28, 30], fill=(190, 210, 226, 255))
    elif kind == "power":
        d.rectangle([14, 15, 17, 17], fill=(250, 250, 180, 255))
    elif kind.startswith("armor"):
        d.rectangle([7, 9, 8, 24], fill=light)
        d.rectangle([23, 9, 24, 24], fill=light)
        d.rectangle([10, 7, 21, 8], fill=light)
    return img


def rotate_dir(img: Image.Image, direction: str) -> Image.Image:
    if direction == "u":
        return img
    if direction == "r":
        return img.rotate(-90, resample=Image.NEAREST)
    if direction == "d":
        return img.rotate(180, resample=Image.NEAREST)
    return img.rotate(90, resample=Image.NEAREST)


def make_tanks() -> None:
    dirs = "urdl"
    for level in range(4):
        base = tank_up(f"player{level}", level)
        for d in dirs:
            save(rotate_dir(base, d), f"tank_player_l{level}_{d}.png")
    for kind in ("basic", "fast", "power"):
        base = tank_up(kind)
        for d in dirs:
            save(rotate_dir(base, d), f"tank_{kind}_{d}.png")
    for hp in (4, 3, 2, 1):
        base = tank_up(f"armor{hp}")
        for d in dirs:
            save(rotate_dir(base, d), f"tank_armor_hp{hp}_{d}.png")


# ---------------------------------------------------------------- 基地 / 子弹 / 道具
def make_title_tanks() -> None:
    """标题画面的小坦克放大到 64x64（用最近邻，像素块保持锐利）。"""
    for src, name in (("tank_player_l3_r.png", "title_player.png"),
                      ("tank_armor_hp4_l.png", "title_armor.png")):
        img = Image.open(os.path.join(OUT, src)).convert("RGBA")
        img = img.resize((img.width * 2, img.height * 2), Image.NEAREST)
        save(img, name)


def make_base() -> None:
    img = Image.new("RGBA", (32, 32), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, 31, 31], fill=(34, 36, 40, 255))
    d.rectangle([1, 1, 30, 30], outline=(92, 98, 108, 255))
    d.rectangle([3, 3, 28, 28], fill=(58, 62, 70, 255))
    # 鹰徽：双翼 + 身体 + 金色底座
    d.polygon([(4, 13), (12, 15), (15, 10), (19, 10), (23, 15), (28, 13),
               (25, 20), (19, 18), (18, 24), (14, 24), (13, 18), (7, 20)],
              fill=(226, 228, 232, 255))
    d.polygon([(10, 15), (15, 12), (17, 12), (21, 15), (18, 17), (14, 17)],
              fill=(156, 160, 168, 255))
    d.rectangle([14, 16, 18, 23], fill=(232, 234, 238, 255))
    d.rectangle([13, 23, 19, 25], fill=(220, 168, 40, 255))
    d.point((16, 13), fill=(20, 20, 20, 255))
    d.point((14, 12), fill=(20, 20, 20, 255))
    d.point((18, 12), fill=(20, 20, 20, 255))
    d.polygon([(15, 14), (17, 14), (16, 16)], fill=(242, 180, 40, 255))
    save(img, "base_eagle.png")

    img = Image.new("RGBA", (32, 32), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, 31, 31], fill=(26, 24, 22, 255))
    for i in range(46):
        rng = random.Random(900 + i)
        x, y = rng.randrange(2, 30), rng.randrange(2, 30)
        c = rng.choice([(70, 62, 54), (96, 80, 58), (54, 50, 46)])
        d.rectangle([x, y, x + 2, y + 2], fill=(*c, 255))
    d.line([(6, 26), (13, 15), (19, 21), (26, 9)], fill=(40, 36, 32, 255), width=2)
    d.line([(14, 8), (20, 14), (24, 12)], fill=(210, 140, 52, 255), width=2)
    save(img, "base_dead.png")


def make_bullet() -> None:
    img = Image.new("RGBA", (8, 8), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rectangle([3, 0, 5, 7], fill=(255, 246, 206, 255))
    d.rectangle([2, 2, 6, 6], fill=(255, 210, 88, 255))
    d.rectangle([3, 3, 5, 5], fill=(255, 255, 255, 255))
    d.point((2, 1), fill=(232, 120, 40, 255))
    d.point((5, 6), fill=(232, 120, 40, 255))
    save(img, "bullet.png")


def make_powerups() -> None:
    def frame() -> tuple[Image.Image, ImageDraw.ImageDraw]:
        img = Image.new("RGBA", (32, 32), (0, 0, 0, 0))
        d = ImageDraw.Draw(img)
        d.rectangle([0, 0, 31, 31], fill=(8, 8, 10, 255))
        d.rectangle([1, 1, 30, 30], outline=(238, 200, 62, 255), width=2)
        d.rectangle([4, 4, 27, 27], outline=(96, 78, 24, 255))
        return img, d

    img, d = frame()
    star = []
    for i in range(10):
        ang = -math.pi / 2 + i * math.pi / 5
        r = 12 if i % 2 == 0 else 5
        star.append((16 + math.cos(ang) * r, 16 + math.sin(ang) * r))
    d.polygon(star, fill=(255, 226, 76, 255), outline=(255, 250, 200, 255))
    save(img, "power_star.png")

    img, d = frame()
    d.ellipse([9, 12, 23, 26], fill=(54, 60, 68, 255), outline=(180, 190, 200, 255))
    d.rectangle([13, 8, 18, 13], fill=(96, 104, 112, 255))
    d.line([(17, 8), (22, 4), (25, 6)], fill=(238, 200, 62, 255), width=2)
    d.point((25, 5), fill=(255, 230, 120, 255))
    save(img, "power_grenade.png")

    img, d = frame()
    d.pieslice([7, 7, 25, 25], 180, 360, fill=(214, 228, 240, 255))
    d.rectangle([7, 16, 25, 22], fill=(214, 228, 240, 255))
    d.rectangle([5, 21, 27, 24], fill=(160, 178, 196, 255))
    d.line([(15, 10), (15, 21)], fill=(250, 252, 255, 255))
    save(img, "power_helmet.png")

    img, d = frame()
    d.line([(11, 8), (20, 21)], fill=(170, 120, 72, 255), width=3)
    d.polygon([(8, 26), (13, 17), (22, 17), (27, 26)], fill=(190, 198, 208, 255))
    d.line([(12, 25), (21, 19)], fill=(240, 246, 252, 255))
    save(img, "power_shovel.png")

    img, d = frame()
    d.rectangle([9, 15, 23, 25], fill=(86, 156, 72, 255))
    d.rectangle([13, 9, 19, 17], fill=(132, 208, 116, 255))
    d.rectangle([15, 5, 17, 11], fill=(200, 224, 190, 255))
    d.rectangle([8, 24, 12, 27], fill=(44, 48, 54, 255))
    d.rectangle([20, 24, 24, 27], fill=(44, 48, 54, 255))
    save(img, "power_tank.png")

    img, d = frame()
    d.ellipse([7, 7, 25, 25], outline=(226, 234, 244, 255), width=2)
    d.line([(16, 16), (16, 10)], fill=(255, 230, 90, 255), width=2)
    d.line([(16, 16), (21, 18)], fill=(255, 230, 90, 255), width=2)
    d.point((16, 16), fill=(255, 255, 255, 255))
    d.rectangle([14, 4, 18, 6], fill=(226, 234, 244, 255))
    save(img, "power_timer.png")


# ---------------------------------------------------------------- 特效
def make_fx() -> None:
    # 爆炸 5 帧
    for k in range(5):
        img = Image.new("RGBA", (32, 32), (0, 0, 0, 0))
        d = ImageDraw.Draw(img)
        rng = random.Random(700 + k)
        radius = (5, 10, 14, 12, 8)[k]
        colors = [
            [(255, 255, 226), (255, 218, 82)],
            [(255, 230, 128), (246, 146, 36)],
            [(255, 164, 40), (196, 68, 20)],
            [(210, 84, 26), (98, 44, 22)],
            [(118, 104, 92), (54, 50, 48)],
        ][k]
        for _ in range(20):
            ang = rng.random() * math.pi * 2
            r = rng.random() * 2
            x = 16 + math.cos(ang) * radius * r
            y = 16 + math.sin(ang) * radius * r
            rr = max(1, int(5 - k) + rng.randrange(3))
            d.ellipse([x - rr, y - rr, x + rr, y + rr], fill=(*colors[rng.randrange(2)], 255))
        if k < 2:
            d.ellipse([16 - radius // 2, 16 - radius // 2,
                       16 + radius // 2, 16 + radius // 2], fill=(255, 255, 238, 255))
        save(img, f"explosion{k}.png")

    # 出生/无敌出现：旋转四角星
    for k in range(4):
        img = Image.new("RGBA", (32, 32), (0, 0, 0, 0))
        d = ImageDraw.Draw(img)
        r = 6 + k * 4
        ang = k * math.pi / 4
        pts = []
        for i in range(8):
            a = ang + i * math.pi / 4
            rr = r if i % 2 == 0 else r * 0.28
            pts.append((16 + math.cos(a) * rr, 16 + math.sin(a) * rr))
        d.polygon(pts, fill=(250, 252, 255, 255), outline=(130, 200, 255, 255))
        d.point((16, 16), fill=(255, 236, 120, 255))
        save(img, f"spawn{k}.png")

    # 护盾：两帧旋转方括号
    for k in range(2):
        img = Image.new("RGBA", (32, 32), (0, 0, 0, 0))
        d = ImageDraw.Draw(img)
        r = 15
        off = 1 if k == 0 else 2
        for sx, sy in ((-1, -1), (1, -1), (-1, 1), (1, 1)):
            cx = 16 + sx * r
            cy = 16 + sy * r
            d.line([(cx - sx * 5, cy), (cx, cy)], fill=(116, 220, 255, 255), width=2)
            d.line([(cx, cy), (cx, cy - sy * 5)], fill=(230, 250, 255, 255), width=1)
        d.ellipse([2 + off, 2 + off, 29 - off, 29 - off], outline=(42, 118, 184, 255))
        save(img, f"shield{k}.png")


def make_hud_icons() -> None:
    for name, color in (("hud_enemy", (156, 162, 170)), ("hud_life", (238, 200, 72))):
        img = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
        d = ImageDraw.Draw(img)
        d.rectangle([2, 3, 5, 13], fill=(46, 50, 56, 255))
        d.rectangle([10, 3, 13, 13], fill=(46, 50, 56, 255))
        d.rectangle([5, 5, 10, 12], fill=color)
        d.rectangle([6, 6, 9, 9], fill=(230, 236, 240, 255))
        d.rectangle([7, 1, 8, 6], fill=color)
        save(img, f"{name}.png")


# ---------------------------------------------------------------- 标题
def draw_pattern_text(d: ImageDraw.ImageDraw, text: str, x: int, y: int, scale: int,
                      fill, shadow=None) -> int:
    if shadow:
        for i, ch in enumerate(text):
            rows = FONT[ch]
            for ry, row in enumerate(rows):
                for rx, bit in enumerate(row):
                    if bit == "1":
                        d.rectangle([x + i * 6 * scale + rx * scale + scale,
                                     y + ry * scale + scale,
                                     x + i * 6 * scale + rx * scale + 2 * scale - 1,
                                     y + ry * scale + 2 * scale - 1], fill=shadow)
    for i, ch in enumerate(text):
        rows = FONT[ch]
        for ry, row in enumerate(rows):
            for rx, bit in enumerate(row):
                if bit == "1":
                    d.rectangle([x + i * 6 * scale + rx * scale, y + ry * scale,
                                 x + i * 6 * scale + rx * scale + scale - 1,
                                 y + ry * scale + scale - 1], fill=fill)
    return x + len(text) * 6 * scale


def make_logo() -> None:
    img = Image.new("RGBA", (320, 96), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # 粗黑描边 + 血影 + 金黄主色，模仿街机标题字。
    for dx in (-3, 0, 3):
        for dy in (-3, 0, 3):
            draw_pattern_text(d, "BATTLE", 16 + dx, 5 + dy, 6,
                              (72, 28, 16) if (dx or dy) else (0, 0, 0))
    draw_pattern_text(d, "BATTLE", 16, 5, 6, (250, 206, 66), (142, 56, 20))
    draw_pattern_text(d, "CITY", 64, 48, 6, (255, 230, 120), (142, 56, 20))
    # 顶部小星条
    for x in range(4, 316, 24):
        d.rectangle([x, 0, x + 8, 1], fill=(70, 60, 40, 255))
    save(img, "logo.png")


def main() -> None:
    os.makedirs(OUT, exist_ok=True)
    make_backgrounds()
    make_tiles()
    make_tanks()
    make_title_tanks()
    make_base()
    make_bullet()
    make_powerups()
    make_fx()
    make_hud_icons()
    make_logo()
    make_glyphs()
    print(f"tank art -> {OUT}")


if __name__ == "__main__":
    main()
