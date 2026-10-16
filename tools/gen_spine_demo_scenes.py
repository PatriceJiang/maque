#!/usr/bin/env python3
"""生成 samples/spine 的两个「多实体」场景：demo-batching.scn 与 all.scn。

为什么用生成器：这两个场景分别有 50 个骨架实体与 12 个总览格子，手写 JSON 既冗长又容易
改漏一处。生成物**入库**（随仓库提交），本脚本是它们的单一真相：

    python tools/gen_spine_demo_scenes.py

其余 demo 场景（demo-*.scn）是手写的：每个只有 20 来行、要一处处看 HUD 文案，
生成器反而更难读。两者共用同一份官方行为表（见 samples/spine/demos.js 的 DEMOS）。
"""
import json
import os

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(REPO, "samples", "spine")

# 官方 Batching：NUM_SKELETONS = 50（.deps/official/cocos/BatchingExample.cpp:36）
BATCH_COUNT = 50

# 总览网格：4 列 x 3 行（12 个骨架），每格 400x400（画布 1600x1200）
CELLS = [
    ("Spineboy",   "/art/spineboy-pro.json",      "/art/spineboy.atlas"),
    ("Goblins",    "/art/goblins-pro.json",       "/art/goblins.atlas"),
    ("Coin",       "/art/coin-pro.skel",          "/art/coin.atlas"),
    ("Raptor",     "/art/raptor-pro.json",        "/art/raptor.atlas"),
    ("Tank",       "/art/tank-pro.skel",          "/art/tank.atlas"),
    ("MixAndMatch", "/art/mix-and-match-pro.skel", "/art/mix-and-match.atlas"),
    ("Sequence",   "/art/dragon-ess.skel",        "/art/dragon-pma.atlas"),
    ("IK",         "/art/spineboy-pro.json",      "/art/spineboy.atlas"),
    ("Physics",    "/art/celestial-circus-pro.skel", "/art/celestial-circus.atlas"),
    ("Separator",  "/art/spineboy-pro.json",      "/art/spineboy.atlas"),
    ("glfw",       "/art/spineboy-pro.skel",      "/art/spineboy-pma.atlas"),
    ("sdl",        "/art/spineboy-pro.json",      "/art/spineboy-pma.atlas"),
]

GRID_COLS, GRID_ROWS = 4, 3
CELL_W, CELL_H = 400, 400
BOTTOM_TEXT = 60      # 底部给 HUD/说明文字留出的空档（文字放底部：骨架站立会向上长，放顶部会撞上）
TOP_MARGIN = 40
LABEL_FONT = 16


def skel(name, x, y, skel_ref, atlas_ref, z=0, autoplay=False):
    return {
        "name": name,
        "components": [
            {"type": "scn.Transform2D", "x": x, "y": y},
            {"type": "anim.SpineSkeleton", "skeleton": skel_ref, "atlas": atlas_ref,
             "animation": 0, "autoplay": autoplay, "play_mode": "PlayMode::Loop",
             "speed": 1.0, "z_index": z},
        ],
    }


def label(name, x, y, text, size, rgb):
    return {
        "name": name,
        "components": [
            {"type": "scn.Transform2D", "x": x, "y": y},
            {"type": "scn.Label", "text": text, "font_size": size,
             "r": rgb[0], "g": rgb[1], "b": rgb[2]},
        ],
    }


def batching():
    ents = []
    # 官方：位置 x ∈ [10%,90%] 内容宽、y ∈ [0,70%] 内容高，逐个体随机；
    # 引擎侧由脚本用 **引擎 rng()** 在 on_start 里设置（确定性复现，见 demos.js setupBatching）。
    ents.append(skel("demo.batching", 640.0, 20.0, "/art/spineboy-pro.json", "/art/spineboy.atlas"))
    for i in range(1, BATCH_COUNT):
        ents.append(skel("batch%d" % i, 640.0, 20.0, "/art/spineboy-pro.json", "/art/spineboy.atlas"))
    ents.append(label("hud", 12, 930,
                      "Batching (official): %d spineboy skeletons share one atlas, each walks then jumps after a random delay (engine rng), then runs"
                      % BATCH_COUNT, 24, (0.08, 0.08, 0.10)))
    ents.append(label("hint", 12, 12,
                      "UP/DOWN anim   SPACE pause   N next demo   CLICK x3 = official cycle", 16, (0.16, 0.16, 0.20)))
    return {
        "title": "Spine demo: Batching (50 skeletons)",
        "script": "demos.js",
        # 官方 demo 用**设备尺寸**；50 个 0.6 缩放的 spineboy 在 800x600 里太挤（实测糊成一团），
        # 所以放大到 1280x960 —— 位置仍按官方的百分比公式（x∈[10%,90%] 宽、y∈[0,70%] 高）。
        "width": 1280,
        "height": 960,
        "seed": 20241016,
        "entities": ents,
    }


def all_overview():
    ents = []
    W = GRID_COLS * CELL_W
    H = BOTTOM_TEXT + GRID_ROWS * CELL_H + TOP_MARGIN
    # 标记实体：demos.js 靠"场景里存在某个 demo.* 实体"识别场景种类（见 samples/spine/demos.js）。
    # 它只有 Transform2D，不渲染任何东西。
    ents.append({
        "name": "demo.all",
        "components": [{"type": "scn.Transform2D", "x": 0.0, "y": 0.0}],
    })
    for i, (name, skel_ref, atlas_ref) in enumerate(CELLS):
        col, row = i % GRID_COLS, i // GRID_COLS
        cx = CELL_W // 2 + col * CELL_W
        # 骨架"脚"落在格子的下方：世界 y 向上 → row 0 在画布上方
        cy = BOTTOM_TEXT + CELL_H // 2 + (GRID_ROWS - 1 - row) * CELL_H
        ents.append(skel("cell%d" % i, float(cx), float(cy), skel_ref, atlas_ref, z=i))
        ents.append(label("cellname%d" % i, float(cx - 190), float(cy - 185), name, LABEL_FONT, (0.72, 0.75, 0.80)))
    # HUD/说明放**底部**：骨架从各自的 y 往上长，放顶部会与第 0 行重叠（实测被 Spineboy 的头压住）
    ents.append(label("hud", 12, 20,
                      "Official spine demos overview: %d skeletons in one scene (Batching = one representative; its own scene has 50)"
                      % len(CELLS), 24, (0.92, 0.94, 0.96)))
    ents.append(label("hint", 12, 62,
                      "all 11 official cocos2d-x demos + the two official C++ entry demos (spine-glfw / spine-sdl); each demo also has its own scene",
                      16, (0.62, 0.66, 0.72)))
    return {
        "title": "Spine demos: ALL (overview)",
        "script": "demos.js",
        "width": W,
        "height": H,
        "seed": 20241016,
        "entities": ents,
    }


def write(name, obj):
    path = os.path.join(OUT, name)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(obj, f, indent=2, ensure_ascii=False)
        f.write("\n")
    print("wrote %s (%d entities)" % (path, len(obj["entities"])))


if __name__ == "__main__":
    write("demo-batching.scn", batching())
    write("all.scn", all_overview())
