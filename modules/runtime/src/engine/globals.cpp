/* 引擎级全局标志（原 runtime.cpp 的收尾内容）。
 *
 * `MAQUE_KEEP_OPEN=1`：忽略脚本 quit()（演示/人工查看用，见 host::quit）。
 * 由 engine/setup.cpp 按环境变量置位，host API 读。 */
#include "../runtime_engine.h"

namespace maque::rt {

bool keep_open = false;

}  // namespace maque::rt
