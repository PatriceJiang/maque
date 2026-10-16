/* maque_spine —— Spine 2D 素材演示工程的**宿主**（samples/spine）。
 *
 * 刻意保持"薄"（照 samples/flappy 的解耦模式）：
 *   · 只做参数解析 + mq_run_scene()，不引用任何引擎内部头文件；
 *   · 只链接 maque::runtime；
 *   · 骨架/图集/动画切换/HUD 全在资源（spine.scn / dragon.scn + spine.js）里。
 *
 * 用法：
 *   maque_spine [--shot <out.png>] [--shot-frame N] [--frames N] [<scene.scn>]
 * 默认场景 = spine.scn（相对当前工作目录）。从仓库根目录跑：
 *   ./build-win/samples/spine/maque_spine samples/spine/spine.scn
 *   ./build-win/samples/spine/maque_spine --shot shot.png --shot-frame 60 samples/spine/dragon.scn */
#include "runtime/mq_runtime.h"
#include "core/compat/mq_env.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <print>
#include <string>

static void usage() {
    std::print("usage: maque_spine [--shot <out.png|.ppm>] [--shot-frame N] [--frames N] [scene.scn]\n"
               "  scene.scn   默认 spine.scn（工作目录相对路径）；dragon.scn = 多页图集样例\n"
               "  --frames N  最多跑 N 帧后退出（配合 headless 做冒烟/回归）\n"
               "  --shot F    渲染到第 --shot-frame 帧（默认 1）写一张图后退出\n"
               "  ↑/↓ 切换动画，空格暂停（见 spine.js）\n");
}

int main(int argc, char** argv) {
    std::string scene = "spine.scn";
    bool scene_set = false;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) { usage(); return 0; }
        if (std::strcmp(a, "--shot") == 0 && i + 1 < argc) { maque::compat::set_env("MAQUE_SHOT", argv[++i]); continue; }
        if (std::strcmp(a, "--shot-frame") == 0 && i + 1 < argc) { maque::compat::set_env("MAQUE_SHOT_FRAME", argv[++i]); continue; }
        if (std::strcmp(a, "--frames") == 0 && i + 1 < argc) { maque::compat::set_env("MAQUE_MAX_FRAMES", argv[++i]); continue; }
        if (a[0] == '-') { std::print(stderr, "maque_spine: 未知参数 '{}'\n", a); usage(); return 2; }
        if (!scene_set) { scene = a; scene_set = true; }
        else { std::print(stderr, "maque_spine: 多余参数 '{}'\n", a); return 2; }
    }
    return mq_run_scene(scene.c_str());
}
