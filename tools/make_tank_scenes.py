"""从 13x13 大格地图生成 samples/tank/stages/stageN.scn。

为什么让 Python 生成 .scn（而不是在游戏脚本里写地图）：
  · 关卡是**场景数据**，不是玩法代码；`stageN.scn` 就是这一关的唯一真相。
  · 每个非空格子是一个 16x16 的 sprite 节点，节点名带 (列,行,类型)：
        tile_c09_r10_brick
    父场景脚本只遍历一次子树就能建出碰撞网格 —— 不复制第二份地图常量。
  · 基地、玩家出生点、3 个敌人出生点也是场景节点；换关 = 卸载/实例化一个 .scn。
  · 生成器同时强制保证基地 2x2 单元和周围一圈砖存在（shovel 道具要复用这些节点）。

坐标：26x26 单元，cell(c,r) 中心 = (16 + (c-1)*16 + 8, 32 + (26-r)*16 + 8)。
用法：python tools/make_tank_scenes.py
"""
from __future__ import annotations

import json
import os

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
OUT = os.path.join(ROOT, "samples", "tank", "stages")

# 13x13 大格；每个大格铺成 2x2 个 16px 单元。
MAPS = [
    [
        ".............",
        ".............",
        ".##.##.##.##.",
        ".##.##.##.##.",
        ".##.##.##.##.",
        ".##.##.##.##.",
        ".............",
        "..@.......@..",
        ".............",
        ".###.###.###.",
        ".###.###.###.",
        ".....###.....",
        ".....#E#.....",
    ],
    [
        ".............",
        "....##.##....",
        ".##.##.##.##.",
        ".##.......##.",
        "....@@.@@....",
        ".##.......##.",
        ".##.##.##.##.",
        "....@@.@@....",
        ".##.......##.",
        ".##.##.##.##.",
        "....#####....",
        ".....#E#.....",
        ".....###.....",
    ],
    [
        ".............",
        "..TT.....TT..",
        "T.##.###.##.T",
        "T.##.###.##.T",
        "T...........T",
        "T.@@.~I~.@@.T",
        "T....~I~....T",
        "T.##.~I~.##.T",
        "T...........T",
        "T.##.##.##.##",
        "T....###....T",
        "T....#E#....T",
        "T....###....T",
    ],
    [
        ".............",
        "..@@.....@@..",
        "..@@.....@@..",
        ".............",
        ".II.......II.",
        ".II..###..II.",
        ".....###.....",
        ".II..###..II.",
        ".II.......II.",
        ".............",
        "..@@.....@@..",
        "....#####....",
        ".....#E#.....",
    ],
]

CHAR = {"#": "brick", "@": "steel", "~": "water", "T": "tree", "I": "ice"}
TEX = {
    "brick": "/art/tile_brick.png",
    "steel": "/art/tile_steel.png",
    "water": "/art/tile_water0.png",
    "tree": "/art/tile_tree.png",
    "ice": "/art/tile_ice.png",
}

# 26x26 cell 坐标：行 1 在上，行 26 在下。
BASE_C = (13, 14)
BASE_R = (25, 26)
PLAYER_START = (160, 48)
SPAWNS = [(32, 432), (224, 432), (416, 432)]
# shovel 会把这 16 个格子在 steel/brick 之间切换；生成器保证节点存在。
SHOVEL_CELLS = [
    (12, 23), (13, 23), (14, 23), (15, 23),
    (12, 24), (13, 24), (14, 24), (15, 24),
    (11, 25), (12, 25), (11, 26), (12, 26),
    (15, 25), (16, 25), (15, 26), (16, 26),
]


def cell_center(c: int, r: int) -> tuple[float, float]:
    return (16.0 + (c - 1) * 16.0 + 8.0,
            32.0 + (26 - r) * 16.0 + 8.0)


def expand(big: list[str]) -> list[list[str]]:
    grid = [["." for _ in range(26)] for _ in range(26)]
    for br, row in enumerate(big, start=1):
        for bc in range(1, 14):
            ch = row[bc - 1]
            if ch == "E":
                t = "B"
            else:
                t = ch
            for dr in (0, 1):
                for dc in (0, 1):
                    grid[(br - 1) * 2 + dr][(bc - 1) * 2 + dc] = t
    # 基地 2x2
    for r in (24, 25):          # 0-based rows 24/25 = 1-based 25/26
        for c in (12, 13):      # 0-based cols 12/13 = 1-based 13/14
            grid[r][c] = "B"
    # 基地旁边一圈确保有砖块节点（shovel 需要原地复用一个节点，不能运行时凭空创建）。
    for (c, r) in SHOVEL_CELLS:
        if grid[r - 1][c - 1] != "B":
            grid[r - 1][c - 1] = "#"
    # 出生点/玩家出生点 2x2 区域清空，避免被地图堵住。
    def clear_at(x: float, y: float) -> None:
        # 坦克中心 (x,y) 覆盖 2x2 单元（左/右各 16px，上/下各 16px）。
        c = int((x - 32) // 16) + 1
        r = 25 - int((y - 48) // 16)
        for rr in (r, r + 1):
            for cc in (c, c + 1):
                if 1 <= rr <= 26 and 1 <= cc <= 26 and grid[rr - 1][cc - 1] != "B":
                    grid[rr - 1][cc - 1] = "."
    clear_at(*PLAYER_START)
    for p in SPAWNS:
        clear_at(*p)
    return grid


def sprite(name: str, x: float, y: float, w: float, h: float, texture: str) -> dict:
    return {
        "name": name,
        "components": [
            {"type": "scn.Transform2D", "x": x, "y": y},
            {"type": "scn.Sprite", "width": w, "height": h,
             "r": 1, "g": 1, "b": 1, "texture": texture},
        ],
    }


def marker(name: str, x: float, y: float) -> dict:
    return {
        "name": name,
        "components": [{"type": "scn.Transform2D", "x": x, "y": y}],
    }


def build_stage(index: int, grid: list[list[str]]) -> dict:
    children = []
    for r in range(1, 27):
        for c in range(1, 27):
            ch = grid[r - 1][c - 1]
            if ch not in CHAR:
                continue
            t = CHAR[ch]
            x, y = cell_center(c, r)
            children.append(sprite(f"tile_c{c:02d}_r{r:02d}_{t}", x, y, 16, 16, TEX[t]))
    # 基地 2x2 单元（列 13..14、行 25..26）的中心。
    bx, by = 224.0, 48.0
    children.append(sprite("base_eagle", bx, by, 32, 32, "/art/base_eagle.png"))
    children.append(marker("player_start", PLAYER_START[0], PLAYER_START[1]))
    for i, (x, y) in enumerate(SPAWNS, start=1):
        children.append(marker(f"spawn_{i}", x, y))
    return {
        "title": f"Battle City - Stage {index}",
        "entities": [
            {
                "name": "stage_root",
                "components": [{"type": "scn.Transform2D", "x": 0, "y": 0}],
                "children": children,
            }
        ],
    }


def main() -> None:
    os.makedirs(OUT, exist_ok=True)
    for i, big in enumerate(MAPS, start=1):
        assert len(big) == 13, (i, len(big))
        assert all(len(row) == 13 for row in big), (i, [len(row) for row in big])
        data = build_stage(i, expand(big))
        path = os.path.join(OUT, f"stage{i}.scn")
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
            f.write("\n")
        print(f"  stage{i}.scn  {os.path.getsize(path)} bytes  "
              f"{len(data['entities'][0]['children'])} nodes")
    print(f"tank stages -> {OUT}")


if __name__ == "__main__":
    main()
