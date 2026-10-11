/* maque_host —— 嵌入宿主示例（P3，docs/05）：**宿主拥有主循环**。
 *
 * 该示例故意不调用 mq_run_scene（引擎自带主循环），而是用 mq_host_* 接口
 * 自己驱动：创建实例 → 每帧 tick 到自己的 RGBA 缓冲 → 注入输入 → 观察校验和。
 * 浏览器/WASM（emscripten_set_main_loop）、嵌入式（RTOS 任务）与 CI 的接法与此一致。 */
#include "runtime/mq_host.h"

#include <print>
#include <vector>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::print(stderr, "usage: maque_host <scene.scn> [frames] [--two-instances]\n");
        return 2;
    }
    const char* scene = argv[1];
    const int frames = argc > 2 ? std::atoi(argv[2]) : 30;
    const bool two = argc > 3 && std::strcmp(argv[3], "--two-instances") == 0;

    // 宿主侧驱动循环（无引擎主循环）
    auto drive = [&](mq_host_scene* s, const char* label) {
        int w = 0, h = 0;
        mq_host_size(s, &w, &h);
        std::vector<uint8_t> frame((size_t)w * (size_t)h * 4u);
        std::printf("%s: %dx%d\n", label, w, h);
        for (int f = 0; f < frames; ++f) {
            if (f == 3) mq_host_push_pointer(s, MQ_HOST_POINTER_DOWN, 0, (float)(w / 2), (float)(h / 2));
            if (f == 4) mq_host_push_pointer(s, MQ_HOST_POINTER_UP, 0, (float)(w / 2), (float)(h / 2));
            if (mq_host_tick(s, frame.data(), frame.size()) != 0) { std::printf("%s: tick failed\n", label); return; }
            if ((f + 1) % 10 == 0)
                std::printf("%s: frame=%llu checksum=%llu\n", label,
                            (unsigned long long)mq_host_frame(s),
                            (unsigned long long)mq_host_checksum(s));
        }
        // 宿主的"AI 面"：控制面文本进出（可挂到 WebSocket/PostMessage）
        const char* resp = mq_host_control(s, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"Runtime.frame\"}");
        std::printf("%s: control(%s)\n", label, resp ? resp : "(null)");
    };

    mq_host_desc desc{};
    desc.struct_size = (uint32_t)sizeof desc;
    mq_host_scene* a = mq_host_scene_create(scene, &desc);
    if (!a) { std::print(stderr, "maque_host: cannot create instance for '{}'\n", scene); return 1; }
    drive(a, "instance-A");

    if (two) {
        // 多实例（ADR-0014）：同一场景两个独立实例并行驱动，互不影响
        mq_host_scene* b = mq_host_scene_create(scene, &desc);
        if (!b) { std::print(stderr, "maque_host: cannot create second instance\n"); return 1; }
        drive(b, "instance-B");
        const bool same = mq_host_checksum(a) == mq_host_checksum(b);
        std::printf("multi-instance: A=%llu B=%llu identical=%s\n",
                    (unsigned long long)mq_host_checksum(a),
                    (unsigned long long)mq_host_checksum(b), same ? "yes" : "no");
        if (!same) { std::print(stderr, "maque_host: instances diverged (should be independent & deterministic)\n"); return 1; }
        mq_host_scene_destroy(b);
    }

    mq_host_scene_destroy(a);
    return 0;
}
