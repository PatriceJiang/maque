/* tank —— 这个游戏工程的**宿主**。
 *
 * 刻意保持"薄"：它只做一件事 —— 把场景交给引擎，然后让引擎自己跑。
 * 没有任何游戏逻辑、没有引用任何引擎内部头文件、没有链接除 mq_runtime 之外的模块
 * （所有玩法都在同目录的 tank.scn + tank.js 里）。这就是"解耦"的验收标准：
 * 宿主源代码里出现任何游戏概念（坦克/炮弹/敌人/AI…），就说明耦合漏进来了。
 *
 * 用法：
 *   maque_tank [--shot <out.png>] [--shot-frame N] [--frames N] [<scene.scn>]
 * 默认场景 = `tank.scn`（相对当前工作目录）；也可以显式传路径，
 * 例如 `maque_tank samples/tank/tank.scn`。 */
#include "runtime/mq_runtime.h"
#include "core/compat/mq_env.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <print>
#include <string>

static void usage() {
    std::print("usage: maque_tank [--shot <out.png|.ppm>] [--shot-frame N] [--frames N] [scene.scn]\n"
               "  scene.scn   默认 tank.scn（工作目录相对路径）\n"
               "  --frames N  最多跑 N 帧后退出（配合 headless 做冒烟/回归）\n"
               "  --shot F    渲染到第 --shot-frame 帧（默认 1）写一张图后退出\n");
}

int main(int argc, char** argv) {
    std::string scene = "tank.scn";
    bool scene_set = false;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) { usage(); return 0; }
        if (std::strcmp(a, "--shot") == 0 && i + 1 < argc) { maque::compat::set_env("MAQUE_SHOT", argv[++i]); continue; }
        if (std::strcmp(a, "--shot-frame") == 0 && i + 1 < argc) { maque::compat::set_env("MAQUE_SHOT_FRAME", argv[++i]); continue; }
        if (std::strcmp(a, "--frames") == 0 && i + 1 < argc) { maque::compat::set_env("MAQUE_MAX_FRAMES", argv[++i]); continue; }
        if (a[0] == '-') { std::print(stderr, "maque_tank: 未知参数 '{}'\n", a); usage(); return 2; }
        if (!scene_set) { scene = a; scene_set = true; }
        else { std::print(stderr, "maque_tank: 多余参数 '{}'\n", a); return 2; }
    }
    return mq_run_scene(scene.c_str());
}
