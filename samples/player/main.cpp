/* maque player —— 引擎运行时宿主。demo 里没有任何游戏逻辑，
 * 游戏逻辑全在资源（.scn + .js / .lua）里。核心就一句：mq_run_scene(argv[1])。
 *
 * 用法：
 *   maque_player <scene.scn> [frames_dir]              运行（frames_dir 非空则逐帧写 PPM）
 *   maque_player --shot <out.png|.ppm> [--shot-frame N] <scene.scn>
 *                                                      渲染到第 N 帧（默认 1）写一张图片后退出
 *   maque_player --help
 *
 * 窗口/画布/输入等运行期开关见 AGENTS.md（MAQUE_WINDOW / MAQUE_PRESENT / MAQUE_STORAGE …）。 */
#include "runtime/mq_runtime.h"
#include "core/compat/mq_env.h"   // setenv/unsetenv 的跨平台封装（Windows 无 setenv）

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <print>

static void usage() {
    std::print("usage: maque_player [--shot <out.png|.ppm>] [--shot-frame N] <scene.scn> [frames_dir]\n"
               "  --shot <file>      渲染一张图片后退出（.png 走编码器；.ppm 原样；默认第 1 帧）\n"
               "  --shot-frame <n>   截第 n 帧（渲染到该帧；场景脚本照常运行）\n"
               "  frames_dir         位置参数（可选）：逐帧 PPM 输出目录\n"
               "环境：MAQUE_WINDOW / MAQUE_PRESENT / MAQUE_SS / MAQUE_MAX_FRAMES / MAQUE_STORAGE …\n");
}

int main(int argc, char** argv) {
    const char* scene = nullptr;
    const char* frames = nullptr;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) { usage(); return 0; }
        /* --shot/--shot-frame 把值写进环境变量：引擎侧统一从 MAQUE_SHOT(_FRAME) 读，
         * 这样命令行与"只设环境变量"两条路径共用一份实现。 */
        if (std::strcmp(a, "--shot") == 0 && i + 1 < argc) { maque::compat::set_env("MAQUE_SHOT", argv[++i]); continue; }
        if (std::strcmp(a, "--shot-frame") == 0 && i + 1 < argc) { maque::compat::set_env("MAQUE_SHOT_FRAME", argv[++i]); continue; }
        if (a[0] == '-') { std::print(stderr, "maque_player: 未知参数 '{}'\n", a); usage(); return 2; }
        if (!scene) scene = a;
        else if (!frames) frames = a;
        else { std::print(stderr, "maque_player: 多余参数 '{}'\n", a); return 2; }
    }
    if (!scene) { usage(); return 2; }
    return mq_run_scene_ex(scene, frames);
}
