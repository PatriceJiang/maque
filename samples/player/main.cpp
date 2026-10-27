/* maque player —— 引擎运行时宿主。**所有 sample 都用它打开**：
 * 游戏逻辑全在资源（.scn + .js / .lua）里，宿主源码里不该出现任何游戏概念。
 * 核心就一句：mq_run_scene(scene)。
 *
 * 用法：
 *   maque_player <scene.scn> [frames_dir]        运行（frames_dir 非空则逐帧写 PPM）
 *   maque_player --shot <out.png|.ppm> [--shot-frame N] <scene.scn>
 *                                              渲染到第 N 帧（默认 1）写一张图片后退出
 *   maque_player --shot-seq <tmpl> --shot-frames 1,30,60 <scene.scn>
 *                                              批量截图（tmpl 里 {} → 帧号）
 *   maque_player --shot-every 30 --shot-count 4 <scene.scn>
 *                                              每 30 帧一张，共 4 张
 *   maque_player --frames N <scene.scn>        最多跑 N 帧（冒烟/回归）
 *   maque_player --help
 *
 * 所有 sample（flappy / tank / spine / scenes/**）都用这一支可执行文件：
 * 之前每个 sample 各带一份 main.cpp，彼此只差"默认场景名"，属于纯复制 —— 已删除。
 *
 * 窗口/画布/输入等运行期开关见 AGENTS.md（MAQUE_WINDOW / MAQUE_PRESENT / MAQUE_STORAGE …）。 */
#include "runtime/mq_runtime.h"
#include "core/compat/mq_env.h"   // setenv/unsetenv 的跨平台封装（Windows 无 setenv）

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <print>

static void usage() {
    std::print("usage: maque_player [options] <scene.scn> [frames_dir]\n"
               "  --shot <file>        渲染一张图后退出（.png 走编码器；.ppm 原样；默认第 1 帧）\n"
               "  --shot-frame <n>     截第 n 帧（渲染到该帧；场景脚本照常运行）\n"
               "  --shot-seq <tmpl>    批量截图：tmpl 里的 {{}} 换成帧号（无 {{}} 则插到扩展名前）\n"
               "                         例：--shot-seq out/f{{}}.png --shot-frames 1,30,60\n"
               "  --shot-frames <csv>  批量截图的精确帧号（逗号分隔）\n"
               "  --shot-every <n>     每 n 帧拍一张（配 --shot-count）\n"
               "  --shot-count <m>     最多拍 m 张后退出\n"
               "  --frames <n>         最多跑 n 帧后退出（等价 MAQUE_MAX_FRAMES；冒烟/回归用）\n"
               "  frames_dir           位置参数（可选）：逐帧 PPM 输出目录\n"
               "环境：MAQUE_WINDOW / MAQUE_WINDOW_MAXIMIZED / MAQUE_WINDOW_EVENT_AT / MAQUE_PRESENT /\n"
               "      MAQUE_RENDER_SCALE / MAQUE_MAX_FRAMES / MAQUE_STORAGE …\n");
}

int main(int argc, char** argv) {
    const char* scene = nullptr;
    const char* frames = nullptr;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) { usage(); return 0; }
        /* 截图相关参数统一写进环境变量：引擎侧只读 MAQUE_*，
         * 这样"命令行"与"只设环境变量"两条路径共用一份实现（也方便脚本调用）。 */
        if (std::strcmp(a, "--shot") == 0 && i + 1 < argc) { maque::compat::set_env("MAQUE_SHOT", argv[++i]); continue; }
        if (std::strcmp(a, "--shot-frame") == 0 && i + 1 < argc) { maque::compat::set_env("MAQUE_SHOT_FRAME", argv[++i]); continue; }
        if (std::strcmp(a, "--shot-seq") == 0 && i + 1 < argc) { maque::compat::set_env("MAQUE_SHOT_SEQ", argv[++i]); continue; }
        if (std::strcmp(a, "--shot-frames") == 0 && i + 1 < argc) { maque::compat::set_env("MAQUE_SHOT_FRAMES", argv[++i]); continue; }
        if (std::strcmp(a, "--shot-every") == 0 && i + 1 < argc) { maque::compat::set_env("MAQUE_SHOT_EVERY", argv[++i]); continue; }
        if (std::strcmp(a, "--shot-count") == 0 && i + 1 < argc) { maque::compat::set_env("MAQUE_SHOT_COUNT", argv[++i]); continue; }
        if (std::strcmp(a, "--frames") == 0 && i + 1 < argc) { maque::compat::set_env("MAQUE_MAX_FRAMES", argv[++i]); continue; }
        if (a[0] == '-') { std::print(stderr, "maque_player: 未知参数 '{}'\n", a); usage(); return 2; }
        if (!scene) scene = a;
        else if (!frames) frames = a;
        else { std::print(stderr, "maque_player: 多余参数 '{}'\n", a); return 2; }
    }
    if (!scene) { usage(); return 2; }
    return mq_run_scene_ex(scene, frames);
}
