/* Spine 运行时的扩展点实现（spine-cpp 4.2 要求宿主提供）。
 *
 * spine-cpp 的 `SpineExtension::getInstance()` 在首次使用时调用
 * `spine::getDefaultExtension()` —— 这是**留给宿主**的钩子（分配器 / 文件读取 / 日志）。
 * 官方各平台样例都在自己的 main 里实现它；作为引擎侧封装，我们在模块里提供一个
 * 默认实现（用上游的 `DefaultSpineExtension`：标准 malloc/free + FILE* 读文件）。
 *
 * 与引擎约定的关系：本模块的骨架/图集**都从内存进来**（调用方用 VFS 读），
 * 所以这里的 `_readFile` 只在 spine 内部真正需要读文件时才走（正常路径不会用到）。
 * 若将来要让 spine 直接走 VFS，可用 `SpineExtension::setInstance` 换一个自定义实现。 */
#include <spine/Extension.h>

namespace spine {
SpineExtension *getDefaultExtension() {
    return new DefaultSpineExtension();
}
} // namespace spine
