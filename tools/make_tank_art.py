"""生成 tank 对战样例的位图素材（程序化、确定性；不需要外部素材库）。

设计意图（与 tools/make_flappy_art.py 同一套路）：
  · 素材是**游戏的东西**，放在游戏工程自己的目录（samples/tank/art/）；
    引擎侧不需要认识任何一张图 —— "引擎即浏览器、游戏即资源"。
  · 全部程序化生成 → 仓库里不塞二进制美术资源，重跑即得同一批图（固定种子）。
  · 尺寸按 640x480 设计分辨率的 1:1 像素画来定，避免缩放糊掉。

俯视坦克的素材拆分（**为什么车体/炮塔是两张图**）：
  · 车体 `hull.png`：履带 + 车体 + 舱盖，左右**对称** → 不依赖朝向也能看出是坦克。
  · 炮塔 `turret.png`：圆形炮塔（旋转对称）—— 引擎这一版 `node_set_angle` 不会旋转
    精灵图元（实测，见 samples/tank/README.md「引擎现状」），所以炮管的**朝向**由脚本
    用一串小方块沿瞄准方向摆位来表达；炮塔本体做成圆盘就没有"朝向错了"的观感问题。
  · 敌我靠配色区分（盟军橄榄绿 / 敌军锈红）。

用法： python tools/make_tank_art.py
"""
import os
import random

from PIL import Image, ImageDraw

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "samples", "tank", "art")
OUT = os.path.normpath(OUT)


def save(img: Image.Image, name: str) -> None:
    path = os.path.join(OUT, name)
    img.save(path)
    print(f"  {name:22s} {img.size[0]}x{img.size[1]}  {os.path.getsize(path)} bytes")


# ---------------------------------------------------------------- 地面 / 装饰

def make_ground() -> Image.Image:
    """整张草地（**整幅设计分辨率**）：底色 + 大格深浅斑块 + 细碎草点 + 几颗石子。

    为什么画整幅而不是小块平铺：渲染器的 sampler 是 ClampToEdge（不能按 UV 平铺），
    平铺会露边；整幅 640x480 一次画完最省事，也保证确定性。
    """
    w, h = 640, 480
    rng = random.Random(20241016)
    img = Image.new("RGB", (w, h), (84, 122, 62))
    d = ImageDraw.Draw(img)
    # 大格深浅（64px 格）：让空旷的草地有层次，不呆板
    for cy in range(0, h, 64):
        for cx in range(0, w, 64):
            if (cx // 64 + cy // 64) % 2 == 0:
                d.rectangle([cx, cy, cx + 63, cy + 63], fill=(88, 128, 66))
    # 细碎草点：深浅各一批
    for _ in range(2600):
        x, y = rng.randrange(w), rng.randrange(h)
        c = rng.choice([(72, 108, 54), (96, 138, 72), (78, 116, 58), (102, 142, 76)])
        d.point((x, y), fill=c)
    for _ in range(420):
        x, y = rng.randrange(w), rng.randrange(h)
        d.line([(x, y), (x + rng.randint(1, 3), y)], fill=(70, 104, 52))
    # 石子/土块：少量即可，多了会脏
    for _ in range(26):
        x, y = rng.randrange(8, w - 8), rng.randrange(8, h - 8)
        r = rng.randint(1, 2)
        d.ellipse([x - r, y - r, x + r, y + r], fill=(118, 120, 106))
    return img


def make_dirt() -> Image.Image:
    """土斑（96x72，带 alpha）：平滑有机轮廓 + 深浅颗粒。

    轮廓用"半径随角度做低频正弦扰动"的多边形 —— 自然但不长疙瘩。
    （试过随机圆斑扰动：会鼓出一堆疙瘩，远看像被啃过，很脏。）
    """
    import math
    w, h = 96, 72
    rng = random.Random(77)
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx, cy = w * 0.5, h * 0.5
    rx, ry = w * 0.5 - 2.0, h * 0.5 - 2.0
    pts = []
    for i in range(72):
        t = i / 72.0 * math.pi * 2.0
        k = 1.0 + 0.055 * math.sin(3.0 * t + 1.2) + 0.045 * math.sin(5.0 * t + 0.4) + 0.03 * math.sin(7.0 * t + 2.1)
        pts.append((cx + math.cos(t) * rx * k, cy + math.sin(t) * ry * k))
    d.polygon(pts, fill=(150, 124, 86, 255))
    d.line(pts + [pts[0]], fill=(120, 96, 64, 255), width=2)          # 深色边，土斑有厚度
    for _ in range(300):
        x, y = rng.randrange(10, w - 10), rng.randrange(10, h - 10)
        if not img.getpixel((x, y))[3]:
            continue
        d.point((x, y), fill=rng.choice([(128, 104, 70, 255), (166, 140, 98, 255), (138, 112, 76, 255)]))
    d.ellipse([28, 20, 36, 28], fill=(168, 142, 100, 255))
    d.ellipse([58, 42, 64, 48], fill=(126, 100, 68, 255))
    return img


def make_shadow() -> Image.Image:
    """车底阴影（72x56 软椭圆）：让坦克从草地上"浮"起来，一眼能分辨。

    为什么用独立实体而不是把阴影画进车体贴图：贴图矩形就是车身大小，
    阴影必须比车身**大一圈**才有效；而引擎这一版不支持给单个精灵加缩放
    （node_set_scale 对 sprite 无效），所以阴影是单独的场景实体、单独摆位。
    """
    w, h = 72, 56
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx, cy, rx, ry = w * 0.5, h * 0.5, w * 0.5 - 1.0, h * 0.5 - 1.0
    for i in range(14, 0, -1):                              # 由外向内叠 alpha → 软边
        k = i / 14.0
        a = int(8 + 74 * (1.0 - k) ** 1.4)
        d.ellipse([cx - rx * k, cy - ry * k, cx + rx * k, cy + ry * k], fill=(10, 16, 8, a))
    return img


def make_crate() -> Image.Image:
    """木箱掩体（48x48）：木板 + 描边 + 对角加固条。"""
    w = h = 48
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, w - 1, h - 1], fill=(146, 104, 58, 255), outline=(78, 52, 26, 255))
    for y in range(6, h - 4, 10):
        d.line([(2, y), (w - 3, y)], fill=(120, 84, 46, 255))
    d.line([(3, 3), (w - 4, h - 4)], fill=(170, 126, 74, 255), width=3)
    d.line([(w - 4, 3), (3, h - 4)], fill=(120, 84, 46, 255), width=3)
    d.rectangle([0, 0, w - 1, h - 1], outline=(62, 40, 20, 255))
    return img


# ---------------------------------------------------------------- 坦克

def _treads(d: ImageDraw.ImageDraw, x0: int, x1: int, y0: int, y1: int, dark, light) -> None:
    """画一条履带（纵向）：底色 + 每 5px 一格。"""
    d.rectangle([x0, y0, x1, y1], fill=dark, outline=(28, 28, 32, 255))
    for y in range(y0 + 2, y1 - 1, 5):
        d.line([(x0 + 1, y), (x1 - 1, y)], fill=light)


def make_hull() -> Image.Image:
    """玩家车体（44x34，左右对称）：两条履带 + 浅橄榄绿车体 + 舱盖 + 前后装甲带。

    配色刻意**比草地亮一档、描边更黑**：玩家车要在满地绿色里一眼可辨
    （实测：第一版用深橄榄绿，截图里车和草地糊在一起）。"""
    w, h = 44, 34
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    _treads(d, 0, 10, 1, h - 2, (42, 44, 50, 255), (74, 76, 84, 255))
    _treads(d, w - 11, w - 1, 1, h - 2, (42, 44, 50, 255), (74, 76, 84, 255))
    d.rectangle([8, 1, w - 9, h - 2], fill=(24, 30, 18, 255))          # 黑色描边（车身整体外廓）
    d.rectangle([9, 2, w - 10, h - 3], fill=(126, 148, 84, 255), outline=(58, 72, 40, 255))
    d.rectangle([11, 4, w - 12, h - 5], fill=(142, 166, 96, 255))
    d.rectangle([13, 2, w - 14, 5], fill=(112, 134, 74, 255))          # 前装甲带
    d.rectangle([13, h - 6, w - 14, h - 3], fill=(112, 134, 74, 255))  # 后装甲带
    d.ellipse([16, 11, 28, 23], fill=(168, 190, 116, 255), outline=(48, 60, 32, 255))
    return img


def make_turret() -> Image.Image:
    """玩家炮塔（26x26，旋转对称的圆盘 + 舱盖）。朝向由脚本摆的炮管方块表达。"""
    w = h = 26
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.ellipse([0, 0, w - 1, h - 1], fill=(24, 30, 18, 255))
    d.ellipse([1, 1, w - 2, h - 2], fill=(134, 158, 90, 255), outline=(58, 72, 40, 255))
    d.ellipse([4, 4, w - 5, h - 5], fill=(156, 180, 106, 255))
    d.ellipse([9, 9, 16, 16], fill=(112, 134, 74, 255), outline=(48, 60, 32, 255))
    return img


def make_enemy_hull() -> Image.Image:
    """敌军车体（40x32）：同构型、锈红涂装 + 迷彩斑。"""
    w, h = 40, 32
    rng = random.Random(4242)
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    _treads(d, 0, 9, 1, h - 2, (46, 42, 44, 255), (72, 66, 70, 255))
    _treads(d, w - 10, w - 1, 1, h - 2, (46, 42, 44, 255), (72, 66, 70, 255))
    d.rectangle([8, 2, w - 9, h - 3], fill=(152, 86, 60, 255), outline=(84, 44, 30, 255))
    d.rectangle([10, 4, w - 11, h - 5], fill=(168, 96, 66, 255))
    for _ in range(14):                                     # 迷彩斑（确定性）
        x = rng.randrange(11, w - 14)
        y = rng.randrange(5, h - 7)
        d.rectangle([x, y, x + rng.randint(2, 4), y + rng.randint(2, 3)], fill=(126, 72, 50, 255))
    d.rectangle([12, 1, w - 13, 4], fill=(132, 74, 52, 255))
    d.rectangle([12, h - 5, w - 13, h - 2], fill=(132, 74, 52, 255))
    return img


def make_enemy_turret() -> Image.Image:
    """敌军炮塔（24x24 圆盘）。"""
    w = h = 24
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.ellipse([0, 0, w - 1, h - 1], fill=(168, 96, 66, 255), outline=(84, 44, 30, 255))
    d.ellipse([3, 3, w - 4, h - 4], fill=(184, 108, 74, 255))
    d.ellipse([8, 8, 15, 15], fill=(140, 78, 54, 255), outline=(84, 44, 30, 255))
    return img


def make_barrel() -> Image.Image:
    """炮管（12x32，**朝向 = 贴图的"上"**）：深色管身 + 中段高光 + 炮口制退器 + 亮色管口。

    为什么把"朝向"做成贴图：引擎现在支持 `node_set_angle` 旋转精灵图元（见 README「引擎现状」），
    所以炮管是一张**朝上**的长条贴图，脚本把它转到瞄准方向即可 —— 比"沿方向排一串方块"自然得多。
    管口那一端画亮一档，就是给旋转做**方向参照**的（否则长条上下对称，转 180° 看不出来）。
    """
    w, h = 12, 32
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rectangle([2, 0, w - 3, h - 1], fill=(44, 46, 54, 255), outline=(20, 22, 26, 255))
    d.rectangle([4, 3, 6, h - 4], fill=(74, 78, 90, 255))          # 高光
    d.rectangle([1, 2, w - 2, 8], fill=(58, 60, 70, 255), outline=(20, 22, 26, 255))   # 炮口制退器
    d.rectangle([3, 0, w - 4, 2], fill=(150, 156, 168, 255))       # 管口（亮）→ 方向参照
    return img


def make_wreck() -> Image.Image:
    """残骸（44x36）：焦黑车体 + 破口。击毁后铺在原地（复用一张贴图，不旋转）。"""
    w, h = 44, 36
    rng = random.Random(99)
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rectangle([2, 2, w - 3, h - 3], fill=(52, 48, 46, 255), outline=(28, 26, 24, 255))
    d.rectangle([8, 6, w - 9, h - 7], fill=(38, 35, 34, 255))
    for _ in range(20):
        x = rng.randrange(4, w - 6)
        y = rng.randrange(4, h - 6)
        d.rectangle([x, y, x + rng.randint(1, 3), y + rng.randint(1, 2)], fill=(70, 62, 58, 255))
    d.ellipse([16, 12, 28, 24], fill=(24, 22, 22, 255))     # 破口
    return img


def main() -> int:
    os.makedirs(OUT, exist_ok=True)
    print(f"生成 tank 素材 → {OUT}")
    save(make_ground(), "ground.png")
    save(make_dirt(), "dirt.png")
    save(make_crate(), "crate.png")
    save(make_shadow(), "shadow.png")
    save(make_barrel(), "barrel.png")
    save(make_hull(), "hull.png")
    save(make_turret(), "turret.png")
    save(make_enemy_hull(), "enemy_hull.png")
    save(make_enemy_turret(), "enemy_turret.png")
    save(make_wreck(), "wreck.png")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
