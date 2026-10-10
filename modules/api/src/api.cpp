/* 引擎 API 实现（对应 schema.yaml api_list 的声明）。
 * 生成的头 maque_api.h 声明签名，这里提供实现；绑定由生成代码完成。 */
#include "maque_api.h"

namespace math {

float clamp(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

} // namespace math
