// 输入模拟器单测：场景解析（绝对帧/相对 delay/click 展开/坐标空间）、到期查询、
// 稳定排序、loop、错误处理、自省 API。
#include "sim/mq_sim.h"

#include <print>
#include <cstring>
#include <string>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::print("FAIL {}:{}  {}\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static int load(mq_sim* s, const char* json, std::string* err = nullptr) {
    char buf[256] = { 0 };
    const int rc = mq_sim_load_json(s, json, std::strlen(json), buf, sizeof buf);
    if (rc != 0 && !err) std::print("load error: rc={} err='{}'\n", rc, buf);
    if (err) *err = buf;
    return rc;
}

int main() {
    // ---- 1) 解析：绝对帧 + 相对 delay + click 展开 ----
    {
        mq_sim* s = mq_sim_create();
        CHECK(s != nullptr);
        const char* json = R"({
          "name": "t1", "space": "window",
          "events": [
            { "frame": 10, "type": "pointer_down", "x": 100, "y": 150, "button": 0 },
            { "delay": 5,  "type": "pointer_move", "x": 120, "y": 160 },
            { "frame": 20, "type": "click", "x": 400, "y": 300 },
            { "frame": 30, "type": "wheel", "dy": -1.0, "x": 400, "y": 300 },
            { "frame": 40, "type": "key_down", "key": 32 },
            { "frame": 42, "type": "text", "text": "hello" }
          ]
        })";
        CHECK(load(s, json) == 0);
        CHECK(mq_sim_loaded(s) == 1);
        // click 展开为 down+up → 共 7 条
        CHECK(mq_sim_count(s) == 7);
        CHECK(mq_sim_last_frame(s) == 42);

        mq_sim_event e{};
        CHECK(mq_sim_event_at(s, 0, &e) == 0);
        CHECK(e.frame == 10 && e.kind == MQ_SIM_POINTER_DOWN);
        CHECK(e.x == 100.0F && e.y == 150.0F);
        CHECK(e.space == MQ_SIM_SPACE_WINDOW);
        CHECK(mq_sim_event_at(s, 1, &e) == 0);
        CHECK(e.frame == 15 && e.kind == MQ_SIM_POINTER_MOVE);      // delay=5 → 10+5
        CHECK(mq_sim_event_at(s, 2, &e) == 0);
        CHECK(e.frame == 20 && e.kind == MQ_SIM_POINTER_DOWN);      // click → down
        CHECK(mq_sim_event_at(s, 3, &e) == 0);
        CHECK(e.frame == 20 && e.kind == MQ_SIM_POINTER_UP);        // click → up（同帧有序）
        CHECK(mq_sim_event_at(s, 4, &e) == 0);
        CHECK(e.kind == MQ_SIM_POINTER_WHEEL && e.dy == -1.0F);
        CHECK(mq_sim_event_at(s, 6, &e) == 0);
        CHECK(e.kind == MQ_SIM_TEXT && e.text && std::strcmp(e.text, "hello") == 0);
        CHECK(mq_sim_finished(s) == 0);

        // ---- 到期查询：逐帧步进（真实宿主用法）→ 时间线完全可复现 ----
        mq_sim_event buf[8];
        uint32_t n = 0;
        uint32_t total = 0;
        for (uint64_t f = 0; f <= 42; ++f) {
            CHECK(mq_sim_due(s, f, buf, 8, &n) == 0);
            if (f == 10) { CHECK(n == 1 && buf[0].kind == MQ_SIM_POINTER_DOWN); }
            else if (f == 15) { CHECK(n == 1 && buf[0].kind == MQ_SIM_POINTER_MOVE); }
            else if (f == 20) { CHECK(n == 2 && buf[0].kind == MQ_SIM_POINTER_DOWN &&
                                         buf[1].kind == MQ_SIM_POINTER_UP); }
            else if (f == 30) { CHECK(n == 1 && buf[0].kind == MQ_SIM_POINTER_WHEEL); }
            else if (f == 40) { CHECK(n == 1 && buf[0].kind == MQ_SIM_KEY_DOWN); }
            else if (f == 42) { CHECK(n == 1 && buf[0].kind == MQ_SIM_TEXT &&
                                         std::strcmp(buf[0].text, "hello") == 0); }
            else { CHECK(n == 0); }
            total += n;
        }
        CHECK(total == 7);
        CHECK(mq_sim_finished(s) == 1);
        // 到期/补发语义：一次跳到很后面也能补齐（不漏事件）
        mq_sim_reset(s);
        CHECK(mq_sim_due(s, 100, buf, 8, &n) == 0);
        CHECK(n <= 8);                                        // 单次受 cap 限制
        uint32_t got = n, guard = 0;
        while (mq_sim_due(s, 100, buf, 8, &n) == 0 && n > 0 && ++guard < 8) got += n;
        CHECK(got == 7);

        // reset 后可重放（确定性：同帧序列 → 同样的事件序列）
        mq_sim_reset(s);
        CHECK(mq_sim_cursor(s) == 0);
        CHECK(mq_sim_due(s, 9, buf, 8, &n) == 0 && n == 0);
        CHECK(mq_sim_due(s, 10, buf, 8, &n) == 0 && n == 1);
        CHECK(mq_sim_finished(s) == 0);
        mq_sim_destroy(s);
    }

    // ---- 2) 乱序声明 → 按帧稳定排序（同帧保持声明顺序）----
    {
        mq_sim* s = mq_sim_create();
        CHECK(load(s, R"({"events":[
            {"frame":30,"type":"key_up","key":1},
            {"frame":10,"type":"key_down","key":1},
            {"frame":10,"type":"text","text":"a"},
            {"frame":20,"type":"key_down","key":2}]})") == 0);
        mq_sim_event e{};
        CHECK(mq_sim_event_at(s, 0, &e) == 0 && e.frame == 10 && e.kind == MQ_SIM_KEY_DOWN);
        CHECK(mq_sim_event_at(s, 1, &e) == 0 && e.frame == 10 && e.kind == MQ_SIM_TEXT);   // 同帧保序
        CHECK(mq_sim_event_at(s, 2, &e) == 0 && e.frame == 20);
        CHECK(mq_sim_event_at(s, 3, &e) == 0 && e.frame == 30);
        mq_sim_destroy(s);
    }

    // ---- 3) 坐标空间：场景默认 + 事件覆盖 ----
    {
        mq_sim* s = mq_sim_create();
        CHECK(load(s, R"({"space":"world","events":[
            {"frame":1,"type":"pointer_move","x":5,"y":6},
            {"frame":2,"type":"pointer_move","x":7,"y":8,"space":"window"}]})") == 0);
        mq_sim_event e{};
        CHECK(mq_sim_event_at(s, 0, &e) == 0 && e.space == MQ_SIM_SPACE_WORLD);
        CHECK(mq_sim_event_at(s, 1, &e) == 0 && e.space == MQ_SIM_SPACE_WINDOW);
        mq_sim_destroy(s);
    }

    // ---- 4) loop：播完回绕（演示用）----
    {
        mq_sim* s = mq_sim_create();
        CHECK(load(s, R"({"loop":true,"events":[{"frame":0,"type":"key_down","key":9}]})") == 0);
        mq_sim_event buf[4]; uint32_t n = 0;
        CHECK(mq_sim_last_frame(s) == 0);            // 场景长度 = 1 帧
        CHECK(mq_sim_due(s, 0, buf, 4, &n) == 0 && n == 1);
        CHECK(mq_sim_finished(s) == 0);              // loop 时永不 finished
        CHECK(mq_sim_due(s, 1, buf, 4, &n) == 0 && n == 1);   // 第二轮：回绕重放
        CHECK(mq_sim_due(s, 2, buf, 4, &n) == 0 && n == 1);   // 第三轮
        mq_sim_set_loop(s, 0);
        CHECK(mq_sim_due(s, 3, buf, 4, &n) == 0 && n == 0);   // 关掉 loop：不再回绕
        CHECK(mq_sim_finished(s) == 1);
        mq_sim_destroy(s);
    }

    // ---- 5) 错误处理：非法 JSON / 未知类型 / 未知空间 / 空事件 ----
    {
        mq_sim* s = mq_sim_create();
        std::string err;
        CHECK(load(s, "{not json", &err) != 0);
        CHECK(!err.empty());
        CHECK(load(s, R"({"events":[{"frame":0,"type":"nope"}]})", &err) != 0);
        CHECK(err.find("unknown type") != std::string::npos);
        CHECK(load(s, R"({"space":"galaxy","events":[{"frame":0,"type":"key_down"}]})", &err) != 0);
        CHECK(err.find("unknown space") != std::string::npos);
        CHECK(load(s, R"({"events":[]})", &err) != 0);
        CHECK(mq_sim_loaded(s) == 0);
        // 失败不应破坏已加载的场景
        CHECK(load(s, R"({"events":[{"frame":3,"type":"key_down","key":7}]})") == 0);
        CHECK(mq_sim_count(s) == 1);
        CHECK(load(s, "{bad", &err) != 0);
        CHECK(mq_sim_count(s) == 1);                 // 仍是上一次成功的内容
        mq_sim_destroy(s);
    }

    // ---- 6) 容量不足报告（调用方可先探测）----
    {
        mq_sim* s = mq_sim_create();
        CHECK(load(s, R"({"events":[{"frame":1,"type":"key_down"},{"frame":1,"type":"key_up"}]})") == 0);
        mq_sim_event one{};
        uint32_t n = 0;
        CHECK(mq_sim_due(s, 1, &one, 1, &n) == 2);   // cap 不足 → 返回 2
        CHECK(n == 2);                               // 但仍报告所需条数
        mq_sim_destroy(s);
    }

    if (g_failures == 0) { std::print("sim_test: all checks passed\n"); return 0; }
    std::print("sim_test: {} failure(s)\n", g_failures);
    return 1;
}
