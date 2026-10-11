/* Inspector 面板辅助：base64/PPM 解码、分块拉取、纹理封装、字体图集与画面预览面板。
 *   · 分块拉取：控制面是行分隔 JSON → 大二进制走 base64 分块（每块 ≤24KB），逐帧推进不阻塞 UI
 *   · 纹理：ImGui 1.92+ 的 ImTextureData（后端 SDLRenderer2 负责创建/更新/销毁） */
#pragma once
#include "client.h"

#include <cstdint>
#include <algorithm>
#include <cstring>
#include <print>
#include <string>
#include <vector>

#include "imgui.h"
#include <SDL.h>

namespace insp {

/* ---- base64 解码 ---- */
inline std::vector<uint8_t> b64_decode(const std::string& in) {
    static int8_t table[256];
    static bool init = false;
    if (!init) {
        std::memset(table, -1, sizeof table);
        const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) table[(uint8_t)T[i]] = (int8_t)i;
        init = true;
    }
    std::vector<uint8_t> out;
    out.reserve(in.size() / 4 * 3);
    uint32_t acc = 0;
    int bits = 0;
    for (char ch : in) {
        const int8_t v = table[(uint8_t)ch];
        if (v < 0) continue;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)((acc >> bits) & 0xFF));
        }
    }
    return out;
}

/* ---- PPM(P6) → RGBA ---- */
inline bool ppm_to_rgba(const std::vector<uint8_t>& ppm, int* w, int* h, std::vector<uint8_t>* rgba) {
    if (ppm.size() < 10 || ppm[0] != 'P' || ppm[1] != '6') return false;
    size_t p = 2;
    int vals[3] = { 0, 0, 0 };
    for (int i = 0; i < 3; ++i) {
        while (p < ppm.size() && (ppm[p] == ' ' || ppm[p] == '\n' || ppm[p] == '\r' || ppm[p] == '\t')) ++p;
        while (p < ppm.size() && ppm[p] >= '0' && ppm[p] <= '9') vals[i] = vals[i] * 10 + (ppm[p++] - '0');
    }
    if (p >= ppm.size()) return false;
    ++p;                                        /* 头部结束的单个空白 */
    const int iw = vals[0], ih = vals[1];
    if (iw <= 0 || ih <= 0) return false;
    if (ppm.size() < p + (size_t)iw * ih * 3) return false;
    rgba->resize((size_t)iw * ih * 4);
    for (size_t i = 0; i < (size_t)iw * ih; ++i) {
        (*rgba)[i * 4 + 0] = ppm[p + i * 3 + 0];
        (*rgba)[i * 4 + 1] = ppm[p + i * 3 + 1];
        (*rgba)[i * 4 + 2] = ppm[p + i * 3 + 2];
        (*rgba)[i * 4 + 3] = 255;
    }
    *w = iw;
    *h = ih;
    return true;
}

/* ---- 二进制分块拉取状态机 ----
 *   · 在途请求上限 + **递增偏移**：一次性推多块时若都用当前 offset，会把同一块重复请求并重复追加
 *     （曾导致图集数据损坏）；TCP 保序 → 按响应顺序拼接即等于按偏移顺序拼接 */
struct Fetch {
    static constexpr size_t kChunk = 16384;   /* 小分块：单块远小于服务端响应缓冲，避免截断 */
    static constexpr int kMaxInFlight = 1;   /* 一次一块：顺序拼接，避免乱序/背压 */

    std::string method;
    std::string params_prefix;
    bool active = false;
    bool done = false;
    size_t next_offset = 0;             /* 下一个待请求偏移 */
    size_t total = (size_t)-1;          /* 首次响应后得知 */
    int in_flight = 0;
    std::vector<int> ids;               /* 本次拉取发出的请求 id（陈旧响应必须丢弃，否则数据错位） */
    std::vector<uint8_t> data;
    int w = 0, h = 0;
    int restarts = 0;
    std::vector<uint8_t> rgba;

    /* 单次模式：一个请求取完整载荷（图集/预览这类 ≤1MB 的小图）。
     * 分块模式（single=false）留给大载荷；面板默认走单次，避开分块重组的边界问题。 */
    bool single = true;                      /* 面板默认单次取整图（服务端缓冲可到 512KB）；大载荷用分块 */

    void start(const std::string& m, const std::string& prefix) {
        method = m;
        params_prefix = prefix;
        /* prefix 必须是**未闭合**的 JSON 对象（形如 {"a":1），pump 负责补 ,"offset":…}。
         * 曾误传 {"max_dim":360} → 拼出非法 JSON → 服务端忽略 offset/max（每块都是第一块 → 图像平铺）。 */
        IM_ASSERT(params_prefix.empty() || params_prefix.back() != '}');
        active = true;
        done = false;
        next_offset = 0;
        total = (size_t)-1;
        in_flight = 0;
        ids.clear();
        if (data.empty()) restarts = 0;              /* 新一轮（非自愈重试）时清零重试计数 */
        data.clear();
        rgba.clear();
        w = h = 0;
    }
    void pump(Client& c) {
        const int max_in_flight = single ? 1 : kMaxInFlight;
        while (active && in_flight < max_in_flight) {
            /* 单次模式：偏移**从已收字节继续**（响应被服务端缓冲截断时自动续传 → 自愈）；
             * 分块模式：按固定步长推进偏移 */
            const size_t off = single ? data.size() : next_offset;
            if (total != (size_t)-1 && off >= total) break;
            char buf[256];
            const size_t chunk = single ? (1u << 20) : kChunk;
            std::snprintf(buf, sizeof buf, "%s,\"offset\":%zu,\"max\":%zu}",
                          params_prefix.c_str(), off, chunk);
            const int id = c.send(method, buf);
            if (id < 0) return;
            ids.push_back(id);
            if (!single) next_offset += chunk;
            ++in_flight;
        }
    }

    /* 返回 true 表示该响应属于当前拉取（按请求 id 判定：重启拉取后陈旧响应必须丢弃，
     * 否则上一轮的块会被拼进新缓冲 → 数据错位（表现为图像平铺/错乱）） */
    bool on_reply(int reply_id, const std::string& raw) {
        if (!active) return false;
        const auto it = std::find(ids.begin(), ids.end(), reply_id);
        if (it == ids.end()) return false;
        ids.erase(it);
        if (in_flight > 0) --in_flight;
        const auto chunk = b64_decode(Client::str_field(raw, "data"));
        data.insert(data.end(), chunk.begin(), chunk.end());
        total = (size_t)Client::num_field(raw, "total", 0);
        /* 完成判定必须看**已收到**的字节数（不是已请求的偏移）：
         * 曾用 next_offset >= total → 单次模式第一个响应就误判完成 → 数据截断/错乱 */
        if (data.size() >= total && in_flight == 0) {
            active = false;
            done = (total == 0 || data.size() == total) && ppm_to_rgba(data, &w, &h, &rgba);
            if (!done && restarts < 2) {                 /* 自愈：长度不符/解析失败 → 重来一次 */
                ++restarts;
                const std::string m = method, pre = params_prefix;
                start(m, pre);
            }
            if (std::getenv("MAQUE_INSPECTOR_TRACE")) {
                char head[24] = { 0 };
                for (size_t i = 0; i < 23 && i < data.size(); ++i)
                    head[i] = (data[i] >= 32 && data[i] < 127) ? (char)data[i] : '.';
                std::print("[insp] 拉取完成 {} 字节 total={} 解析={} {}x{} 头='{}'\n",
                           data.size(), total, done, w, h, head);
            }
        }
        return true;
    }
};

/* ---- 纹理封装（SDL_Texture + ImGui 裸 ID 路径）----
 *   · 走"自己创建 SDL_Texture、把指针作为 ImTextureID 交给 ImGui"这条**已验证可用**的路径。
 *     ImGui 1.93 的 ImTextureData/RegisterUserTexture 路径在本后端下能创建成功（TexID≠0、Status=OK），
 *     但 `ImGui::Image(tex->GetTexRef())` 画不出来（同样数据用裸 ID 就正常）—— 见 README/plans 记录。
 *   · 纹理由本类负责生命周期；不做 ImGui 侧注册。 */
struct Texture {
    static SDL_Renderer* renderer;          /* 由 main 在创建后端后设置 */
    SDL_Texture* sdl = nullptr;
    int w = 0, h = 0;

    void upload(const std::vector<uint8_t>& rgba, int width, int height) {
        if (std::getenv("MAQUE_INSPECTOR_TRACE"))
            std::print("[insp] upload {}x{} bytes={} renderer={}\n", width, height, rgba.size(), (void*)renderer);
        if (!renderer || width <= 0 || height <= 0 || rgba.size() < (size_t)width * height * 4) return;
        if (sdl && (w != width || h != height)) release();
        if (!sdl) {
            sdl = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, width, height);
            if (!sdl) { std::print(stderr, "[insp] SDL_CreateTexture 失败: {}\n", SDL_GetError()); return; }
            SDL_SetTextureBlendMode(sdl, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(sdl, SDL_ScaleModeNearest);
            w = width;
            h = height;
        }
        SDL_UpdateTexture(sdl, nullptr, rgba.data(), width * 4);
    }

    void release() {
        if (sdl) SDL_DestroyTexture(sdl);
        sdl = nullptr;
        w = h = 0;
    }
    void gc() {}                            /* 生命周期由本类管理（保留接口便于调用点统一） */

    int width() const { return w; }         /* 纹理自身尺寸：绘制尺寸必须用它，
                                             * 否则与最新一次拉取的尺寸不一致 → UV 越界 → 平铺重复 */
    int height() const { return h; }
    ImTextureRef ref() const { return ImTextureRef((ImTextureID)(intptr_t)sdl); }
    ImTextureID id() const { return (ImTextureID)(intptr_t)sdl; }
    bool valid() const { return sdl != nullptr; }
};
SDL_Renderer* Texture::renderer = nullptr;

} // namespace insp
