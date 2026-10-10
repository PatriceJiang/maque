/* MANUAL 头：在头文件里内联实现某些引擎接口（docs/21）。
 * 由生成器通过 `#include "math.custom.h"` 引入；无需单独 .cpp。 */
#pragma once

namespace math {

inline float saturate(float x) {
    return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

} // namespace math
