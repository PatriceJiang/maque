/* Inspector 面板辅助：base64/PPM 解码、分块拉取、纹理封装、字体图集与画面预览面板。
 *   · 分块拉取：控制面是行分隔 JSON → 大二进制走 base64 分块（每块 ≤24KB），逐帧推进不阻塞 UI
 *   · 纹理：ImGui 1.92+ 的 ImTextureData（后端 SDLRenderer2 负责创建/更新/销毁） */
#pragma once
#include "client.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"      // RegisterUserTexture（外部纹理注册，实验性 API）

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
    static constexpr size_t kChunk = 24576;
    static constexpr int kMaxInFlight = 4;

    std::string method;
    std::string params_prefix;
    bool active = false;
    bool done = false;
    size_t next_offset = 0;             /* 下一个待请求偏移 */
    size_t total = (size_t)-1;          /* 首次响应后得知 */
    int in_flight = 0;
    std::vector<uint8_t> data;
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;

    void start(const std::string& m, const std::string& prefix) {
        method = m;
        params_prefix = prefix;
        active = true;
        done = false;
        next_offset = 0;
        total = (size_t)-1;
        in_flight = 0;
        data.clear();
        rgba.clear();
        w = h = 0;
    }
    void pump(Client& c) {
        while (active && in_flight < kMaxInFlight && next_offset < total) {
            char buf[256];
            std::snprintf(buf, sizeof buf, "%s,\"offset\":%zu,\"max\":%zu}",
                          params_prefix.c_str(), next_offset, kChunk);
            if (c.send(method, buf) < 0) return;
            next_offset += kChunk;
            ++in_flight;
        }
    }
    /* 返回 true 表示该响应属于当前拉取 */
    bool on_reply(const std::string& raw) {
        if (!active) return false;
        if (in_flight > 0) --in_flight;
        const auto chunk = b64_decode(Client::str_field(raw, "data"));
        data.insert(data.end(), chunk.begin(), chunk.end());
        total = (size_t)Client::num_field(raw, "total", 0);
        if (next_offset >= total && in_flight == 0) {
            active = false;
            done = ppm_to_rgba(data, &w, &h, &rgba);
        }
        return true;
    }
};

/* ---- 纹理封装（ImGui 1.92+ ImTextureData）----
 *   · 自建纹理必须**注册到 ImGui::GetPlatformIO().Textures** —— 后端在 RenderDrawData 里遍历该列表
 *     处理 WantCreate/WantUpdates/WantDestroy；不注册的话后端从不创建 SDL_Texture，
 *     绘制命令拿到无效 TexID（表现为一块白图）。
 *   · 销毁：置 WantDestroy，后端销毁后把状态改为 Destroyed → 我们再 delete 并移出列表。 */
struct Texture {
    ImTextureData* tex = nullptr;

    void upload(const std::vector<uint8_t>& rgba, int w, int h) {
        if (w <= 0 || h <= 0 || rgba.size() < (size_t)w * h * 4) return;
        if (tex && (tex->Width != w || tex->Height != h)) release();
        if (!tex) {
            tex = IM_NEW(ImTextureData)();
            tex->Format = ImTextureFormat_RGBA32;
            tex->Width = w;
            tex->Height = h;
            tex->Pixels = (unsigned char*)ImGui::MemAlloc((size_t)w * h * 4);
            tex->SetStatus(ImTextureStatus_WantCreate);
            ImGui::RegisterUserTexture(tex);        // 官方外部纹理注册：ImGui 会把它并入平台纹理表
        } else {
            tex->SetStatus(ImTextureStatus_WantUpdates);
            tex->UpdateRect.x = 0;
            tex->UpdateRect.y = 0;
            tex->UpdateRect.w = (short)w;
            tex->UpdateRect.h = (short)h;
        }
        std::memcpy(tex->Pixels, rgba.data(), (size_t)w * h * 4);
    }

    void release() {
        if (tex) tex->SetStatus(ImTextureStatus_WantDestroy);   // 后端销毁后由 gc() 回收
        tex = nullptr;
    }

    /* 每帧调用：回收后端已销毁的纹理（含 release() 掉的） */
    static void gc() {
        ImVector<ImTextureData*>& list = ImGui::GetPlatformIO().Textures;
        for (int i = list.Size - 1; i >= 0; --i) {
            ImTextureData* t = list[i];
            if (t->Status == ImTextureStatus_Destroyed) {
                ImGui::MemFree(t->Pixels);
                t->Pixels = nullptr;
                IM_DELETE(t);
                list.erase(list.begin() + i);
            }
        }
    }

    ImTextureID id() const { return tex ? tex->GetTexID() : (ImTextureID)0; }
    bool valid() const { return tex != nullptr; }
};

} // namespace insp
