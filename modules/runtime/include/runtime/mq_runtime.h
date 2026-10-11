/* maque 运行时（"浏览器"）—— 加载并运行一个场景资源。
 * 引擎提供通用能力（GPU/反射/脚本/循环/绘制），游戏逻辑全在资源(脚本)里。
 * 核心入口就一句：mq_run_scene("ball.scn")。 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 运行场景资源；返回 0 表示由脚本正常结束(quit)。 */
int mq_run_scene(const char* scn_path);

/* 同上，且把每帧渲染输出为 PPM 到 frames_dir（无显示环境"观看"用）；frames_dir 可为 NULL。
 *
 * 环境变量（无参数接口，供 CI/AI/回放驱动；docs/10/11/19）：
 *   MAQUE_HEADLESS=1        强制无窗口（dummy 视频驱动）
 *   MAQUE_CONTROL=<json|->  控制平面：单条请求，或 "-" 从 stdin 逐行读请求
 *   MAQUE_REPLAY=<path>     回放输入日志（JSON，mq_input_log_json 产物）→ 逐帧确定复现
 *   MAQUE_RECORD=<path>     录制本局输入日志到该路径（JSON）
 *   MAQUE_SNAPSHOT=<path>   退出时把最终世界快照（二进制）写入该路径
 *   MAQUE_MAX_FRAMES=<n>    主循环帧数上限（默认 3600）
 *   MAQUE_RENDERER=<path>   渲染插件 .so 路径（默认 libmaque_gfx.so）
 */
int mq_run_scene_ex(const char* scn_path, const char* frames_dir);

#ifdef __cplusplus
}
#endif
