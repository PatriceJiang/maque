/* maque player —— 引擎运行时宿主。demo 里没有任何游戏逻辑，
 * 游戏逻辑全在资源（.scn + .js）里。核心就一句：mq_run_scene(argv[1])。 */
#include "runtime/mq_runtime.h"

int main(int argc, char** argv) {
    const char* scene  = (argc > 1) ? argv[1] : "scenes/ball.scn";
    const char* frames = (argc > 2) ? argv[2] : nullptr;   // 可选：输出 PPM 帧
    return mq_run_scene_ex(scene, frames);
}
