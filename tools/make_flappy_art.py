"""生成 flappy 的位图素材（程序化，确定性；不需要外部素材库）。

设计意图：
  · 素材是**游戏的东西**，所以放在游戏工程自己的目录（samples/flappy/art/），
    引擎侧不需要认识任何一张图 —— "引擎即浏览器、游戏即资源"。
  · 全部程序化生成 → 仓库里不塞二进制美术资源的历史包袱，重跑即得同一批图（确定性）。
  · 尺寸按 320x240 设计分辨率的 1:1 像素画来定，避免缩放糊掉。

用法： python tools/make_flappy_art.py
"""
import os
from PIL import Image, ImageDraw

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "samples", "flappy", "art")
OUT = os.path.normpath(OUT)


def save(img: Image.Image, name: str) -> None:
    path = os.path.join(OUT, name)
    img.save(path)
    print(f"  {name:22s} {img.size[0]}x{img.size[1]}  {os.path.getsize(path)} bytes")


def make_sky() -> Image.Image:
    """竖直渐变天：上深下浅，纯色填充时也能看出层次。"""
    w, h = 320, 240
    img = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(img)
    top, bot = (92, 168, 214), (186, 224, 236)
    for y in range(h):
        t = y / (h - 1)
        d.line([(0, y), (w, y)], fill=tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3)))
    return img


def make_cloud() -> Image.Image:
    """一朵云：几个圆叠出来，带 1px 描边让它从天上分得出来。"""
    w, h = 68, 20
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    body = (252, 252, 255, 255)
    outline = (214, 226, 240, 255)
    for cx, cy, r in ((16, 12, 8), (30, 8, 10), (46, 11, 9), (56, 14, 6)):
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=body, outline=outline)
    return img


def make_bird() -> Image.Image:
    """鸟（面向右）：圆身 + 喙 + 眼 + 翅。半径 9 → 画 18x18。"""
    w, h = 18, 18
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.ellipse([0, 1, 15, 16], fill=(250, 208, 60, 255), outline=(196, 148, 24, 255))
    d.ellipse([12, 6, 17, 11], fill=(240, 120, 40, 255), outline=(186, 84, 20, 255))   # 喙
    d.ellipse([10, 4, 13, 7], fill=(255, 255, 255, 255))                               # 眼白
    d.ellipse([11, 5, 12, 6], fill=(24, 24, 32, 255))                                  # 瞳孔
    d.ellipse([2, 8, 9, 14], fill=(238, 178, 44, 255), outline=(186, 132, 20, 255))    # 翅膀
    return img


def make_pipe() -> Image.Image:
    """水管一段：绿管 + 1px 深色边 + 高光竖条（可竖直平铺）。30 宽。"""
    w, h = 30, 16
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, w - 1, h - 1], fill=(94, 190, 74, 255), outline=(38, 106, 34, 255))
    d.rectangle([4, 0, 9, h - 1], fill=(132, 214, 106, 255))     # 高光
    d.rectangle([w - 8, 0, w - 3, h - 1], fill=(58, 146, 50, 255))  # 暗面
    return img


def make_pipe_cap() -> Image.Image:
    """管口（比管略宽，flappy 的视觉标志）。36 宽。"""
    w, h = 36, 14
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, w - 1, h - 1], fill=(104, 200, 82, 255), outline=(38, 106, 34, 255))
    d.rectangle([3, 2, 10, h - 3], fill=(140, 220, 112, 255))
    return img


def make_ground() -> Image.Image:
    """地面整条（**整幅宽**）：渲染器的 sampler 是 ClampToEdge（不能按 UV 平铺），
    所以滚动靠"两张同图并排平移"，条纹本身要足够长（>= 设计宽度）才不会露边。
    画 320 宽、20 高，含 8 个重复瓦片。"""
    w, h, tile = 320, 20, 40
    img = Image.new("RGB", (w, h), (196, 168, 116))
    d = ImageDraw.Draw(img)
    for x0 in range(0, w, tile):
        for x in range(x0, x0 + tile, 8):
            d.line([(x, h), (x + h, 0)], fill=(178, 148, 98), width=2)
        d.line([(x0, 0), (x0, h - 1)], fill=(170, 140, 92), width=1)
    d.rectangle([0, 0, w - 1, 1], fill=(150, 122, 78))
    return img


def make_grass() -> Image.Image:
    """草皮整条（整幅宽），同理不平铺。"""
    w, h = 320, 6
    img = Image.new("RGB", (w, h), (108, 196, 84))
    d = ImageDraw.Draw(img)
    for x in range(0, w, 4):
        d.line([(x, h - 1), (x + 2, 0)], fill=(80, 168, 62), width=1)
    d.rectangle([0, 0, w - 1, 1], fill=(140, 216, 108))
    return img


def main() -> int:
    os.makedirs(OUT, exist_ok=True)
    print(f"生成 flappy 素材 → {OUT}")
    save(make_sky(), "sky.png")
    save(make_cloud(), "cloud.png")
    save(make_bird(), "bird.png")
    save(make_pipe(), "pipe.png")
    save(make_pipe_cap(), "pipe_cap.png")
    save(make_ground(), "ground.png")
    save(make_grass(), "grass.png")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
