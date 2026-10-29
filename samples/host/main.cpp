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
        std::print(stderr, "usage: maque_host <scene.scn> [frames] [--two-instances] [--step-only]\n");
        return 2;
    }
    const char* scene = argv[1];
    const int frames = argc > 2 ? std::atoi(argv[2]) : 30;
    bool two = false;
    bool step_only = false;
    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--two-instances") == 0) two = true;
        else if (std::strcmp(argv[i], "--step-only") == 0) step_only = true;
    }

    // Host-owned loop. Web uses step_only with an rAF timestamp accumulator.
    auto drive = [&](mq_host_scene* s, const char* label) -> bool {
        int w = 0, h = 0;
        mq_host_size(s, &w, &h);
        std::vector<uint8_t> frame;
        if (!step_only) frame.resize((size_t)w * (size_t)h * 4u);
        std::print("{}: {}x{}\n", label, w, h);
        for (int f = 0; f < frames; ++f) {
            if (f == 3) mq_host_push_pointer(s, MQ_HOST_POINTER_DOWN, 0, (float)(w / 2), (float)(h / 2));
            if (f == 4) mq_host_push_pointer(s, MQ_HOST_POINTER_UP, 0, (float)(w / 2), (float)(h / 2));
            const int rc = step_only
                ? mq_host_step(s)
                : mq_host_tick(s, frame.data(), frame.size());
            if (rc != 0) {
                std::print(stderr, "{}: {} failed\n", label, step_only ? "step" : "tick");
                return false;
            }
            if ((f + 1) % 10 == 0)
                std::print("{}: frame={} checksum={}\n", label,
                           (unsigned long long)mq_host_frame(s),
                           (unsigned long long)mq_host_checksum(s));
        }
        const char* resp = mq_host_control(s, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"Runtime.frame\"}");
        std::print("{}: control({})\n", label, resp ? resp : "(null)");
        return true;
    };

    mq_host_desc desc{};
    desc.struct_size = (uint32_t)sizeof desc;
    mq_host_scene* a = mq_host_scene_create(scene, &desc);
    if (!a) { std::print(stderr, "maque_host: cannot create instance for '{}'\n", scene); return 1; }
    if (!drive(a, "instance-A")) {
        mq_host_scene_destroy(a);
        return 1;
    }

    if (two) {
        // 多实例（ADR-0014）：同一场景两个独立实例并行驱动，互不影响
        mq_host_scene* b = mq_host_scene_create(scene, &desc);
        if (!b) { std::print(stderr, "maque_host: cannot create second instance\n"); mq_host_scene_destroy(a); return 1; }
        if (!drive(b, "instance-B")) {
            mq_host_scene_destroy(b);
            mq_host_scene_destroy(a);
            return 1;
        }
        const bool same = mq_host_checksum(a) == mq_host_checksum(b);
        std::print("multi-instance: A={} B={} identical={}\n",
                   (unsigned long long)mq_host_checksum(a),
                   (unsigned long long)mq_host_checksum(b), same ? "yes" : "no");
        if (!same) { std::print(stderr, "maque_host: instances diverged (should be independent & deterministic)\n"); mq_host_scene_destroy(b); mq_host_scene_destroy(a); return 1; }
        mq_host_scene_destroy(b);
    }

    mq_host_scene_destroy(a);
    return 0;
}
