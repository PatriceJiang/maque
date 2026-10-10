/* maque 运行时（"浏览器"）—— 加载并运行一个场景资源。
 * 引擎提供通用能力（GPU/反射/脚本/循环/绘制），游戏逻辑全在资源(脚本)里。
 * 核心入口就一句：mq_run_scene("ball.scn")。 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 运行场景资源；返回 0 表示由脚本正常结束(quit)。 */
int mq_run_scene(const char* scn_path);

/* 同上，且把每帧渲染输出为 PPM 到 frames_dir（无显示环境"观看"用）；frames_dir 可为 NULL。 */
int mq_run_scene_ex(const char* scn_path, const char* frames_dir);

#ifdef __cplusplus
}
#endif
