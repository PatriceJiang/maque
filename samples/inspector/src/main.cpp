/* Maque Inspector（独立进程；plans/17）：
 *   · 通过 TCP/UDS attach 运行中的游戏，复用控制面 JSON-RPC（不发明第二套协议）
 *   · 面板：连接/状态、场景树、属性（schema 驱动：Types.describe + Scene.get）、控制台、指标
 *   · 默认只读：编辑需服务端允许（游戏侧 MAQUE_INSPECT_RW=1），被拒请求在状态栏计数
 *   · --headless：不开窗口，跑一串请求并打印响应（CI 用；无显示环境可测）
 *   · 渲染用 SDL2 + SDL_Renderer2（软件），**不碰 GL/EGL**（docs/05 教训）
 *
 * 用法：
 *   maque_inspector --connect 7777                 # 连本机 TCP
 *   maque_inspector --connect uds:/tmp/maque.sock  # 连 UDS
 *   maque_inspector --connect 7777 --headless Scene.info Scene.entities
 */
#include "client.h"

#include <cstdio>
#include <cstring>
#include <print>
#include <string>
#include <vector>

#include <SDL.h>
#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"

using insp::Client;

namespace {

struct EntLite { int id; std::string name; int parent; };
std::vector<EntLite> parse_entities(const std::string& raw);      // 定义见下（pump 先用）

struct App {
    Client client;
    std::string connect_spec = "7777";
    bool read_only = true;
    std::string endpoint;
    double frame = 0, clients = 0, rejected = 0, methods = 0;
    std::string scene_info;
    std::string entities_raw;                 // Scene.entities 原始 JSON
    std::string describe_raw;                 // Types.describe 原始 JSON
    std::string selected;                     // 选中实体 id（字符串）
    std::string props_raw;                    // 选中实体的属性原始 JSON
    std::vector<std::string> log;
    char console[512] = "{\"jsonrpc\":\"2.0\",\"method\":\"Control.info\"}";
    char connect_input[128] = "7777";
    float refresh_acc = 0.0F;

    void note(const std::string& s) {
        log.push_back(s);
        if (log.size() > 400) log.erase(log.begin(), log.begin() + 100);
    }
};

void pump(App& a, float dt) {
    for (auto& [method, reply] : a.client.poll()) {
        if (!reply.ok) a.note("✗ " + method + " → " + (reply.error.empty() ? reply.raw : reply.error));
        if (method == "Control.info") {
            a.read_only = insp::Client::str_field(reply.raw, "read_only") != "false" &&
                          reply.raw.find("\"read_only\":false") == std::string::npos;
            a.endpoint = insp::Client::str_field(reply.raw, "endpoint");
            a.frame = insp::Client::num_field(reply.raw, "frame");
            a.clients = insp::Client::num_field(reply.raw, "clients");
            a.rejected = insp::Client::num_field(reply.raw, "rejected");
            a.methods = insp::Client::num_field(reply.raw, "methods");
        } else if (method == "Scene.info") {
            a.scene_info = reply.raw;
        } else if (method == "Scene.entities") {
            a.entities_raw = reply.raw;
            if (a.selected.empty() && reply.ok) {           // 首次：自动选中首个实体（属性面板不空）
                const auto ents = parse_entities(a.entities_raw);
                if (!ents.empty()) {
                    a.selected = std::to_string(ents.front().id);
                    a.client.send("Scene.get", "{\"id\":" + a.selected + ",\"path\":\"\"}");
                }
            }
        } else if (method == "Types.describe") {
            a.describe_raw = reply.raw;
        } else if (method == "Scene.get") {
            a.props_raw = reply.raw;
        } else {
            a.note((reply.ok ? "← " : "✗ ") + method + " → " + reply.raw.substr(0, 200));
        }
    }
    a.refresh_acc += dt;
    if (a.client.connected() && a.refresh_acc > 0.25F) {          // 4Hz 轮询（不打扰游戏）
        a.refresh_acc = 0.0F;
        a.client.send("Control.info");
        a.client.send("Scene.info");
        a.client.send("Scene.entities");
        if (!a.selected.empty())
            a.client.send("Scene.get", "{\"id\":" + a.selected + ",\"path\":\"\"}");
    }
}

/* 从 Scene.entities 的 JSON 里粗取实体（id/name/parent）——面板用；不做完整 JSON 解析 */
std::vector<EntLite> parse_entities(const std::string& raw) {
    std::vector<EntLite> out;
    size_t p = 0;
    while ((p = raw.find("\"id\"", p)) != std::string::npos) {
        EntLite e{ (int)insp::Client::num_field(raw.substr(p, 400), "id"), "", -1 };
        const size_t np = raw.find("\"name\"", p);
        if (np != std::string::npos && np - p < 400) e.name = insp::Client::str_field(raw.substr(np, 400), "name");
        const size_t pp = raw.find("\"parent\"", p);
        if (pp != std::string::npos && pp - p < 400) e.parent = (int)insp::Client::num_field(raw.substr(pp, 400), "parent");
        if (!e.name.empty()) out.push_back(e);
        p += 4;
    }
    return out;
}

void draw_ui(App& a) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("Maque Inspector", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_MenuBar);

    if (ImGui::BeginMenuBar()) {
        ImGui::Text("%s", a.client.connected() ? "● 已连接" : "○ 未连接");
        ImGui::SameLine();
        ImGui::TextColored(a.read_only ? ImVec4(0.9F, 0.8F, 0.3F, 1.0F) : ImVec4(0.4F, 0.9F, 0.4F, 1.0F),
                           "%s", a.read_only ? "只读" : "可编辑");
        ImGui::SameLine();
        ImGui::Text("| %s | frame %.0f | 客户端 %.0f | 方法 %.0f | 被拒 %.0f",
                    a.endpoint.empty() ? "—" : a.endpoint.c_str(), a.frame, a.clients, a.methods, a.rejected);
        ImGui::EndMenuBar();
    }

    if (!a.client.connected()) {
        ImGui::InputText("端点", a.connect_input, sizeof a.connect_input);
        ImGui::SameLine();
        if (ImGui::Button("连接")) {
            std::string err;
            if (a.client.connect(a.connect_input, &err)) a.note(std::string("已连接 ") + a.connect_input);
            else a.note("连接失败: " + err);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("（游戏侧用 MAQUE_INSPECT=7777 或 uds:/tmp/maque.sock 开启端点）");
    } else if (ImGui::Button("断开")) {
        a.client.disconnect();
    }
    ImGui::Separator();

    const float w = ImGui::GetContentRegionAvail().x;
    /* 左：场景树 */
    ImGui::BeginChild("tree", ImVec2(w * 0.30F, 0), ImGuiChildFlags_Borders);
    ImGui::TextUnformatted("场景树");
    ImGui::Separator();
    if (a.entities_raw.empty()) ImGui::TextDisabled("（等待数据…）");
    for (const EntLite& e : parse_entities(a.entities_raw)) {
        ImGui::PushID(e.id);
        char label[160];
        std::snprintf(label, sizeof label, "%s  #%d%s", e.name.c_str(), e.id,
                      e.parent >= 0 ? "" : "  (根)");
        if (ImGui::Selectable(label, a.selected == std::to_string(e.id))) {
            a.selected = std::to_string(e.id);
            a.client.send("Scene.get", "{\"id\":" + a.selected + ",\"path\":\"\"}");
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    /* 右：属性 + 控制台 */
    ImGui::BeginChild("right", ImVec2(0, 0));
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("属性")) {
            if (a.selected.empty()) ImGui::TextDisabled("在左侧选择一个实体");
            else if (a.props_raw.empty()) ImGui::TextDisabled("（读取中…）");
            else {
                ImGui::Text("实体 #%s", a.selected.c_str());
                ImGui::TextDisabled("只读（游戏侧 MAQUE_INSPECT_RW=1 可开启编辑）");
                ImGui::Separator();
                if (ImGui::BeginTable("props", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                                  ImGuiTableFlags_Resizable)) {
                    ImGui::TableSetupColumn("字段", ImGuiTableColumnFlags_WidthFixed, 150.0F);
                    ImGui::TableSetupColumn("值");
                    ImGui::TableHeadersRow();
                    /* 极简展开：按 "字段":"值" 对扫描（M0 够用；完整树留待 M1 用 schema 驱动） */
                    const std::string& j = a.props_raw;
                    size_t p = 0;
                    while ((p = j.find('"', p)) != std::string::npos) {
                        const size_t q = j.find('"', p + 1);
                        if (q == std::string::npos) break;
                        const std::string key = j.substr(p + 1, q - p - 1);
                        p = q + 1;
                        if (key == "type") {
                            const std::string t = insp::Client::str_field(j.substr(q), "type");
                            if (!t.empty()) {
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                ImGui::TextColored(ImVec4(0.6F, 0.8F, 1.0F, 1.0F), "%s", t.c_str());
                                ImGui::TableSetColumnIndex(1);
                                ImGui::TextDisabled("组件");
                            }
                            continue;
                        }
                        if (p >= j.size() || j[p] != ':') continue;
                        ++p;
                        while (p < j.size() && j[p] == ' ') ++p;
                        if (p >= j.size()) break;
                        std::string val;
                        if (j[p] == '"') {
                            const size_t e = j.find('"', p + 1);
                            if (e == std::string::npos) break;
                            val = j.substr(p, e - p + 1);
                            p = e + 1;
                        } else {
                            const size_t e = j.find_first_of(",}", p);
                            val = j.substr(p, (e == std::string::npos ? j.size() : e) - p);
                            p = e == std::string::npos ? j.size() : e;
                        }
                        if (key == "ok" || key == "id" || key == "name" || key == "components" ||
                            val == "{" || val == "[" ) continue;
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(key.c_str());
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextUnformatted(val.c_str());
                    }
                    ImGui::EndTable();
                }
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("控制台")) {
            ImGui::TextDisabled("发送任意控制面方法（JSON-RPC 单行；与 MCP 同一批方法）");
            ImGui::InputTextMultiline("##req", a.console, sizeof a.console, ImVec2(-1, 60));
            if (ImGui::Button("发送") && a.client.connected()) {
                const std::string req = a.console;
                const std::string method = insp::Client::str_field(req, "method");
                /* 直接发原始 JSON：用 params 提取太脆，这里把整行交给服务端（id 由服务端回） */
                a.note("→ 原始请求 " + req.substr(0, 120));
                a.client.send_raw(req);
            }
            ImGui::Separator();
            ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders);
            for (const std::string& l : a.log) ImGui::TextWrapped("%s", l.c_str());
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("模式")) {
            ImGui::TextWrapped("%s", a.describe_raw.empty() ? "（Types.describe 无数据）" : a.describe_raw.c_str());
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    ImGui::End();
}

int headless(App& a, const std::vector<std::string>& methods) {
    std::string err;
    if (!a.client.connect(a.connect_spec, &err)) {
        std::print(stderr, "inspector: 连接 {} 失败: {}\n", a.connect_spec, err);
        return 2;
    }
    std::print("inspector: 已连接 {}\n", a.connect_spec);
    int rc = 0;
    for (const std::string& m : methods) {
        /* 需要参数的方法：method 后面可跟 JSON（用 | 分隔） */
        const size_t bar = m.find('|');
        const std::string method = bar == std::string::npos ? m : m.substr(0, bar);
        const std::string params = bar == std::string::npos ? "" : m.substr(bar + 1);
        a.client.send(method, params);
        for (int i = 0; i < 200; ++i) {                 // 等响应（最多 ~2s）
            auto done = a.client.poll();
            bool got = false;
            for (auto& [mm, r] : done) {
                if (mm != method) continue;
                std::print("{}\n", r.raw);
                if (!r.ok) rc = 1;
                got = true;
            }
            if (got) break;
            SDL_Delay(10);
        }
    }
    a.client.disconnect();
    return rc;
}

} // namespace

int main(int argc, char** argv) {
    App app;
    std::vector<std::string> queue;
    bool is_headless = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--connect" && i + 1 < argc) app.connect_spec = argv[++i];
        else if (arg == "--headless") is_headless = true;
        else if (arg == "--help") {
            std::print("用法: maque_inspector [--connect <tcp:端口|uds:路径>] [--headless <方法…>]\n");
            return 0;
        } else queue.push_back(arg);
    }
    std::snprintf(app.connect_input, sizeof app.connect_input, "%s", app.connect_spec.c_str());
    if (is_headless) return headless(app, queue);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::print(stderr, "inspector: SDL_Init 失败: {}\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win = SDL_CreateWindow("Maque Inspector", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       1280, 800, SDL_WINDOW_RESIZABLE);
    if (!win) { std::print(stderr, "inspector: 窗口创建失败: {}\n", SDL_GetError()); SDL_Quit(); return 1; }
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) { std::print(stderr, "inspector: 渲染器创建失败: {}\n", SDL_GetError()); SDL_DestroyWindow(win); SDL_Quit(); return 1; }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    /* 中文界面：ImGui 默认字体只有 ASCII → 加载项目字体（含 CJK），字形范围取常用简体 */
    {
        const char* env = std::getenv("MAQUE_FONT");
        const char* cands[] = {
            env,
            "scenes/fonts/AlibabaPuHuiTi-3-55-Regular.ttf",
            "../scenes/fonts/AlibabaPuHuiTi-3-55-Regular.ttf",
            "../../scenes/fonts/AlibabaPuHuiTi-3-55-Regular.ttf",
            "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        };
        for (const char* c : cands) {
            if (!c || !*c) continue;
            if (FILE* f = std::fopen(c, "rb")) {
                std::fclose(f);
                if (io.Fonts->AddFontFromFileTTF(c, 18.0F, nullptr,
                                                 io.Fonts->GetGlyphRangesChineseSimplifiedCommon()))
                    break;
            }
        }
        if (io.Fonts->Fonts.empty()) io.Fonts->AddFontDefault();
    }
    ImGui_ImplSDL2_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer2_Init(ren);

    std::string err;
    if (app.client.connect(app.connect_spec, &err)) app.note(std::string("已连接 ") + app.connect_spec);
    else app.note("连接失败: " + err + "（可在界面上重试）");

    bool running = true;
    Uint32 prev = SDL_GetTicks();
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL2_ProcessEvent(&ev);
            if (ev.type == SDL_QUIT) running = false;
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) running = false;
        }
        const Uint32 now = SDL_GetTicks();
        const float dt = (float)(now - prev) / 1000.0F;
        prev = now;
        pump(app, dt);

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        draw_ui(app);
        ImGui::Render();
        SDL_SetRenderDrawColor(ren, 16, 16, 20, 255);
        SDL_RenderClear(ren);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), ren);
        SDL_RenderPresent(ren);
    }
    app.client.disconnect();
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
