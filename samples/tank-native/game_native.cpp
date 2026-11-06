// game_native.cpp —— 《坦克大战》native 迁移切片（plans/26-cpp-scripts.md）
//
// 与 samples/tank/game.lua 共用同一套 .scn / prefab / 美术 / 音频；
// 根脚本改为 C++ guest，HUD 仍是独立的 Lua 场景（混合语言，通过 global_* 通道通信）。
//
// 本切片覆盖：标题 → 开始 → 动态加载 stage → 玩家移动/开火 → 砖/钢/基地碰撞 →
// 敌人池/AI/子弹 → 计分/生命 → 暂停/游戏结束。未覆盖（仍在 Lua 版）：
// 道具、铁铲、冰面滑行、树/水细节、autoplay、全部测试钩子。
#include "maque/script.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr float FX = 16.0F, FY = 32.0F, CELL = 16.0F;
constexpr int COLS = 26, ROWS = 26;
constexpr float FW = COLS * CELL, FH = ROWS * CELL;
constexpr float FIELD_TOP = FY + FH, FIELD_RIGHT = FX + FW;
constexpr int STAGE_COUNT = 4, ENEMIES_PER_STAGE = 20, MAX_ACTIVE_ENEMIES = 4;
constexpr float TANK_HALF = 14.0F, TANK_SPEED = 84.0F;
constexpr float PLAYER_BULLET_SPEED = 190.0F, PLAYER_FIRE_CD = 0.30F;
constexpr float ENEMY_FIRE_CD_MIN = 0.55F, ENEMY_FIRE_CD_MAX = 1.30F;
constexpr float PLAYER_SHIELD_TIME = 3.0F, RESPAWN_TIME = 1.35F;

enum class St { Title, Loading, Intro, Play, Pause, Clear, Over };
enum class Dir { U, R, D, L };

struct V2 { int dx = 0, dy = 0; };

V2 dir_vec(Dir d) {
    switch (d) {
        case Dir::U: return { 0, 1 };
        case Dir::R: return { 1, 0 };
        case Dir::D: return { 0, -1 };
        case Dir::L: return { -1, 0 };
    }
    return { 0, 0 };
}

const char* dir_name(Dir d) {
    switch (d) {
        case Dir::U: return "u";
        case Dir::R: return "r";
        case Dir::D: return "d";
        case Dir::L: return "l";
    }
    return "u";
}

struct Player {
    int id = -1;
    int shield_id = -1;
    float x = 0, y = 0;
    Dir dir = Dir::U;
    int level = 0;
    bool alive = false;
    float shield = 0, fire_cd = 0, respawn = 0, muzzle_t = 0, slide = 0, flash = 0;
    bool moved = false;
};

struct Enemy {
    int idx = 0;
    int id = -1;
    bool active = false, spawning = false, dead = true;
    float x = 0, y = 0;
    Dir dir = Dir::D;
    std::string kind = "basic";
    int hp = 1, max_hp = 1;
    float speed = 36.0F;
    bool carrier = false;
    std::string power_kind = "star";
    float fire_cd = 1.0F, think = 0.0F, flash = 0.0F, invuln = 0.0F, spawn_t = 0.0F, stuck = 0.0F;
};

struct Bullet {
    int id = -1;
    bool active = false;
    int team = 0;      // 0=player, 1=enemy
    int owner = -1;    // enemy idx
    float x = 0, y = 0;
    float vx = 0, vy = 0, speed = 0;
    bool power = false;
    float life = 0;
};

struct Explosion {
    int id = -1;
    bool active = false;
    float x = 0, y = 0, t = 0, scale = 1.0F;
};

struct SpawnFx {
    int id = -1;
    bool active = false;
    float t = 0;
};

struct SpawnPoint { float x = 0, y = 0; };
struct EnemySpec { std::string kind = "basic"; bool carrier = false; std::string power_kind = "star"; };

struct EnemyDef { float speed = 36, bullet = 145; int hp = 1, score = 100; };

EnemyDef enemy_def(const std::string& kind) {
    if (kind == "fast") return { 74.0F, 175.0F, 1, 200 };
    if (kind == "power") return { 38.0F, 245.0F, 1, 300 };
    if (kind == "armor") return { 34.0F, 170.0F, 4, 400 };
    return { 36.0F, 145.0F, 1, 100 };
}

bool is_block_tank(char ch) { return ch == '#' || ch == '@' || ch == '~' || ch == 'B'; }
bool is_block_bullet(char ch) { return ch == '#' || ch == '@' || ch == 'B'; }

char tile_char(const std::string& t) {
    if (t == "brick") return '#';
    if (t == "steel") return '@';
    if (t == "water") return '~';
    if (t == "tree") return 'T';
    if (t == "ice") return 'I';
    return 0;
}

const char* tile_tex(char ch) {
    switch (ch) {
        case '#': return "/art/tile_brick.png";
        case '@': return "/art/tile_steel.png";
        case '~': return "/art/tile_water0.png";
        case 'T': return "/art/tile_tree.png";
        case 'I': return "/art/tile_ice.png";
        default: return "/art/tile_brick.png";
    }
}

int tile_z(char ch) {
    if (ch == 'T') return 40;
    if (ch == 'I') return 5;
    return 10;
}

float snap_to(float v, float base, float step) {
    return base + std::floor((v - base) / step + 0.5F) * step;
}

std::string pad_num(int n, int width) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%0*d", width, n);
    return buf;
}

} // namespace

struct TankNative final : maque::Script {
    // ---- 状态 ----
    St state = St::Title;
    int frame = 0;
    int stage = 1, pending_stage = 1;
    int score = 0, hi_score = 0, lives = 2, player_level = 0, next_extra = 20000;
    bool base_alive = true, want_stage = false, stage_tried = false;
    bool audio_on = false, bgm_on = true;
    double bgm_voice = 0;
    bool env_god = false, env_skip_title = false, env_trace = false, env_autoplay = false;
    int stage_override = 0;
    int hook_brick_at = 0, hook_base_at = 0, hook_power_at = 0, hook_clear_at = 0;
    std::string hook_power_kind = "star";

    Player player;
    std::array<Enemy, MAX_ACTIVE_ENEMIES> enemies{};
    std::array<Bullet, 20> bullets{};
    std::array<Explosion, 14> explosions{};
    std::array<SpawnFx, MAX_ACTIVE_ENEMIES> spawn_fx{};
    int muzzle = -1;
    int powerup_id = -1;
    bool powerup_active = false;
    std::string powerup_kind = "star";
    float powerup_x = 0, powerup_y = 0, powerup_t = 0;
    std::vector<std::pair<int, int>> shovel_ring;
    float freeze_timer = 0, shovel_timer = 0;
    std::vector<std::pair<int, int>> water_cells;
    float water_anim_t = 0;
    int water_anim_frame = 0;
    std::vector<std::pair<int, int>> bot_path;
    int bot_path_age = 0;

    std::array<std::array<char, COLS + 1>, ROWS + 1> map{};
    std::array<std::array<int, COLS + 1>, ROWS + 1> cell_node{};
    std::unordered_map<int, std::string> tex_of;

    std::vector<SpawnPoint> spawn_points;
    std::vector<EnemySpec> spawn_queue;
    float spawn_timer = 0, intro_timer = 0, stage_clear_timer = 0, over_timer = 0;
    bool over_accept = false;

    float base_x = 224.0F, base_y = 48.0F;
    float player_start_x = 160.0F, player_start_y = 48.0F;
    int base_spr = -1, stage_inst = -1, stage_root = -1, stage_host = -1;
    int bg = -1, field_bg = -1, field_frame = -1;

    double hud_score = -1, hud_hi = -1, hud_stage = -1, hud_lives = -1, hud_enemy = -1, hud_prompt = -1;
    std::string hud_mode, hud_overlay;

    // ---- 基础节点操作 ----
    int find(const char* name) { return (int)host.entity_find(name); }
    void log(const std::string& s) { host.log(s); }
    void park(int id) { if (id >= 0) host.node_set_active((float)id, 0); }
    void unpark(int id) { if (id >= 0) host.node_set_active((float)id, 1); }
    void place(int id, float x, float y) { if (id >= 0) host.node_set_position((float)id, x, y); }
    void set_tex(int id, const std::string& path) {
        if (id < 0) return;
        auto it = tex_of.find(id);
        if (it != tex_of.end() && it->second == path) return;
        host.node_set_string((float)id, "scn.Sprite", "texture", path);
        tex_of[id] = path;
    }
    void set_rgb(int id, float r, float g, float b) {
        if (id < 0) return;
        host.node_set_number((float)id, "scn.Sprite", "r", r);
        host.node_set_number((float)id, "scn.Sprite", "g", g);
        host.node_set_number((float)id, "scn.Sprite", "b", b);
    }
    std::string envv(const char* k) { return host.env(k); }
    float rngf() {
        float v = host.rng();
        if (v < 0) return 0;
        if (v >= 1) return 0.999999F;
        return v;
    }
    bool key(int code) { return host.input_key((float)code) != 0; }

    // ---- 地图 ----
    void new_grid() {
        for (auto& row : map) row.fill('.');
        for (auto& row : cell_node) row.fill(-1);
        spawn_points.clear();
        spawn_queue.clear();
    }
    char cell_at(int c, int r) const {
        if (c < 1 || c > COLS || r < 1 || r > ROWS) return '@';
        return map[(size_t)r][(size_t)c];
    }
    void set_cell(int c, int r, char ch) {
        if (c < 1 || c > COLS || r < 1 || r > ROWS) return;
        map[(size_t)r][(size_t)c] = ch;
        const int id = cell_node[(size_t)r][(size_t)c];
        if (id < 0) {
            if (ch != '.') log("tank: warning missing tile c=" + std::to_string(c) +
                               " r=" + std::to_string(r) + " ch=" + ch);
            return;
        }
        if (ch == '.' || ch == 'B') { park(id); return; }
        unpark(id);
        host.node_set_z_index((float)id, (float)tile_z(ch));
        set_tex(id, tile_tex(ch));
    }

    void update_water(float dt) {
        if (water_cells.empty()) return;
        water_anim_t += dt;
        if (water_anim_t < 0.45F) return;
        water_anim_t = 0;
        water_anim_frame = 1 - water_anim_frame;
        const char* path = water_anim_frame == 0 ? "/art/tile_water0.png" : "/art/tile_water1.png";
        for (const auto& p : water_cells) {
            if (map[(size_t)p.second][(size_t)p.first] == '~')
                set_tex(cell_node[(size_t)p.second][(size_t)p.first], path);
        }
    }

    bool player_on_ice() const {
        const int c = std::clamp((int)std::floor((player.x - FX) / CELL) + 1, 1, COLS);
        const int r = std::clamp(ROWS - (int)std::floor((player.y - FY) / CELL), 1, ROWS);
        return cell_at(c, r) == 'I';
    }

    // ---- 自走测试（autoplay；与 Lua 版 BFS 等价）----
    void cell_of_world(float x, float y, int& c, int& r) const {
        c = std::clamp((int)std::floor((x - FX) / CELL) + 1, 1, COLS);
        r = std::clamp(ROWS - (int)std::floor((y - FY) / CELL), 1, ROWS);
    }

    void tank_cell(float x, float y, int& c, int& r) const {
        c = std::clamp((int)std::floor((x - CELL - FX) / CELL) + 1, 1, COLS - 1);
        const int bottom = (int)std::floor((y - CELL - FY) / CELL);
        r = std::clamp(ROWS - bottom - 1, 1, ROWS - 1);
    }

    static float dist2(float ax, float ay, float bx, float by) {
        const float dx = ax - bx, dy = ay - by;
        return dx * dx + dy * dy;
    }

    bool map_open_for_tank(int c, int r) const {
        if (c < 1 || c > COLS - 1 || r < 1 || r > ROWS - 1) return false;
        for (int rr = r; rr <= r + 1; ++rr)
            for (int cc = c; cc <= c + 1; ++cc)
                if (is_block_tank(cell_at(cc, rr))) return false;
        return true;
    }

    std::vector<std::pair<int, int>> bfs_path(int sc, int sr, int tc, int tr) const {
        if (!map_open_for_tank(tc, tr)) {
            int best_c = 0, best_r = 0, best_d = 1 << 30;
            for (int r = 1; r <= ROWS - 1; ++r)
                for (int c = 1; c <= COLS - 1; ++c)
                    if (map_open_for_tank(c, r)) {
                        const int d = std::abs(c - tc) + std::abs(r - tr);
                        if (d < best_d) { best_d = d; best_c = c; best_r = r; }
                    }
            if (best_d == (1 << 30)) return {};
            tc = best_c;
            tr = best_r;
        }
        if (!map_open_for_tank(sc, sr)) {
            int best_c = 0, best_r = 0, best_d = 1 << 30;
            static const Dir order[] = { Dir::U, Dir::R, Dir::D, Dir::L };
            for (Dir d : order) {
                const V2 v = dir_vec(d);
                const int c = sc + (v.dx > 0 ? 1 : 0);
                const int r = sr + (v.dy < 0 ? 1 : 0);
                if (map_open_for_tank(c, r)) {
                    const int dd = std::abs(c - tc) + std::abs(r - tr);
                    if (dd < best_d) { best_d = dd; best_c = c; best_r = r; }
                }
            }
            if (best_d == (1 << 30)) return {};
            sc = best_c;
            sr = best_r;
        }

        bool seen[COLS + 1][ROWS + 1] = {};
        std::pair<int, int> prev[COLS + 1][ROWS + 1];
        for (auto& row : prev) for (auto& p : row) p = { -1, -1 };
        std::vector<std::pair<int, int>> q;
        q.reserve(COLS * ROWS);
        q.push_back({ sc, sr });
        seen[sc][sr] = true;
        size_t head = 0;
        bool found = false;
        while (head < q.size()) {
            const auto cur = q[head++];
            if (cur.first == tc && cur.second == tr) { found = true; break; }
            static const Dir order[] = { Dir::U, Dir::R, Dir::D, Dir::L };
            for (Dir d : order) {
                const V2 v = dir_vec(d);
                const int c = cur.first + v.dx;
                const int r = cur.second - v.dy;
                if (c < 1 || c > COLS || r < 1 || r > ROWS) continue;
                if (!map_open_for_tank(c, r) || seen[c][r]) continue;
                seen[c][r] = true;
                prev[c][r] = cur;
                q.push_back({ c, r });
            }
        }
        if (!found) return {};
        std::vector<std::pair<int, int>> path;
        int c = tc, r = tr;
        while (!(c == sc && r == sr)) {
            path.push_back({ c, r });
            const auto p = prev[c][r];
            if (p.first < 0) return {};
            c = p.first;
            r = p.second;
        }
        std::reverse(path.begin(), path.end());
        return path;
    }

    bool bot_pick_target(int& tc, int& tr) const {
        if (powerup_active) { tank_cell(powerup_x, powerup_y, tc, tr); return true; }
        const Enemy* best = nullptr;
        float bd = 1e18F;
        for (const auto& e : enemies) {
            if (!e.active || e.spawning) continue;
            const float d = dist2(e.x, e.y, player.x, player.y);
            if (d < bd) { bd = d; best = &e; }
        }
        if (!best) return false;
        tank_cell(best->x, best->y, tc, tr);
        return true;
    }

    void bot_repath() {
        if (!player.alive) { bot_path.clear(); return; }
        int sc = 0, sr = 0, tc = 0, tr = 0;
        tank_cell(player.x, player.y, sc, sr);
        if (!bot_pick_target(tc, tr)) { bot_path.clear(); return; }
        bot_path = bfs_path(sc, sr, tc, tr);
        bot_path_age = 0;
    }

    bool bot_fire_blocked_by_base() const {
        const V2 v = dir_vec(player.dir);
        float bx = player.x, by = player.y;
        for (int i = 0; i < 22; ++i) {
            bx += (float)v.dx * 16.0F;
            by += (float)v.dy * 16.0F;
            int c = 0, r = 0;
            cell_of_world(bx, by, c, r);
            const char ch = cell_at(c, r);
            if (ch == 'B') return true;
            if (is_block_bullet(ch)) return false;
        }
        return false;
    }

    bool bot_has_target_ahead() const {
        for (const auto& e : enemies) {
            if (!e.active || e.spawning) continue;
            if (aligned_shot(player.x, player.y, e.x, e.y, player.dir, 300)) {
                const V2 v = dir_vec(player.dir);
                int c = 0, r = 0;
                cell_of_world(player.x + (float)v.dx * 20.0F, player.y + (float)v.dy * 20.0F, c, r);
                if (!is_block_tank(cell_at(c, r))) return true;
            }
        }
        return false;
    }

    bool bot_want_fire() const {
        if (!player.alive) return false;
        if (bot_has_target_ahead()) return true;
        const V2 v = dir_vec(player.dir);
        int c = 0, r = 0;
        cell_of_world(player.x + (float)v.dx * 28.0F, player.y + (float)v.dy * 28.0F, c, r);
        if (cell_at(c, r) == '#' && !bot_fire_blocked_by_base()) return true;
        return false;
    }

    bool bot_dir(Dir& out) {
        if (!player.alive) return false;
        ++bot_path_age;
        if (bot_path_age > 20 || bot_path.empty()) bot_repath();
        int sc = 0, sr = 0;
        tank_cell(player.x, player.y, sc, sr);
        while (!bot_path.empty()) {
            const auto p = bot_path.front();
            if (p.first == sc && p.second == sr) bot_path.erase(bot_path.begin());
            else break;
        }
        if (!bot_path.empty()) {
            const auto p = bot_path.front();
            if (p.first > sc) out = Dir::R;
            else if (p.first < sc) out = Dir::L;
            else if (p.second < sr) out = Dir::U;
            else if (p.second > sr) out = Dir::D;
            else return false;
            return true;
        }
        if (std::abs(player.x - base_x) > 24) { out = player.x < base_x ? Dir::R : Dir::L; return true; }
        if (std::abs(player.y - base_y) > 24) { out = player.y < base_y ? Dir::U : Dir::D; return true; }
        return false;
    }

    // ---- 场景/关卡 ----
    void walk_stage(int id, const std::function<void(int, const std::string&)>& fn) {
        if (id < 0) return;
        const std::string name = host.entity_name((float)id);
        fn(id, name);
        const int n = (int)host.node_child_count((float)id);
        for (int i = 0; i < n; ++i) walk_stage((int)host.node_child_at((float)id, (float)i), fn);
    }

    std::string stage_path(int st) const {
        const int idx = (st - 1) % STAGE_COUNT + 1;
        return "stages/stage" + std::to_string(idx) + ".scn";
    }

    bool collect_stage() {
        if (stage_host < 0) stage_host = find("StageHost");
        if (stage_host < 0) return false;
        const int n = (int)host.node_child_count((float)stage_host);
        if (n <= 0) return false;
        stage_root = (int)host.node_child_at((float)stage_host, 0);
        if (stage_root < 0) return false;

        new_grid();
        base_spr = -1;
        water_cells.clear();
        water_anim_t = 0;
        water_anim_frame = 0;
        int tile_count = 0;

        walk_stage(stage_root, [&](int id, const std::string& name) {
            int c = 0, r = 0;
            char type[32] = { 0 };
            if (std::sscanf(name.c_str(), "tile_c%d_r%d_%31s", &c, &r, type) == 3) {
                const char ch = tile_char(type);
                if (ch != 0 && c >= 1 && c <= COLS && r >= 1 && r <= ROWS) {
                    map[(size_t)r][(size_t)c] = ch;
                    cell_node[(size_t)r][(size_t)c] = id;
                    unpark(id);
                    host.node_set_z_index((float)id, (float)tile_z(ch));
                    set_tex(id, tile_tex(ch));
                    if (ch == '~') water_cells.emplace_back(c, r);
                    ++tile_count;
                }
            } else if (name == "base_eagle") {
                base_spr = id;
                base_x = host.entity_x((float)id);
                base_y = host.entity_y((float)id);
                host.node_set_z_index((float)id, 12);
                set_tex(id, "/art/base_eagle.png");
            } else if (name == "player_start") {
                player_start_x = host.entity_x((float)id);
                player_start_y = host.entity_y((float)id);
            } else if (std::sscanf(name.c_str(), "spawn_%d", &c) == 1) {
                spawn_points.push_back({ host.entity_x((float)id), host.entity_y((float)id) });
            }
        });

        const int bc = (int)std::floor((base_x - 8 - FX) / CELL) + 1;
        const int br = ROWS - (int)std::floor((base_y - 8 - FY) / CELL);
        for (int dr = 0; dr <= 1; ++dr) {
            for (int dc = 0; dc <= 1; ++dc) {
                const int cc = bc + dc, rr = br - dr;
                if (cc < 1 || cc > COLS || rr < 1 || rr > ROWS) continue;
                map[(size_t)rr][(size_t)cc] = 'B';
                if (cell_node[(size_t)rr][(size_t)cc] >= 0) {
                    park(cell_node[(size_t)rr][(size_t)cc]);
                    cell_node[(size_t)rr][(size_t)cc] = -1;
                }
            }
        }

        std::sort(spawn_points.begin(), spawn_points.end(), [](const SpawnPoint& a, const SpawnPoint& b) {
            if (a.x != b.x) return a.x < b.x;
            return a.y < b.y;
        });

        base_alive = true;
        if (base_spr >= 0) { set_tex(base_spr, "/art/base_eagle.png"); unpark(base_spr); }
        if (stage_root >= 0) unpark(stage_root);
        log("tank: stage scene ready stage=" + std::to_string(pending_stage) +
            " tiles=" + std::to_string(tile_count) +
            " spawns=" + std::to_string(spawn_points.size()) +
            " base=(" + std::to_string((int)base_x) + "," + std::to_string((int)base_y) + ")" +
            " frame=" + std::to_string(frame));
        return base_spr >= 0 && !spawn_points.empty();
    }

    void reset_actor_pools() {
        for (auto& e : enemies) { e.active = false; e.spawning = false; e.dead = true; park(e.id); }
        for (auto& b : bullets) { b.active = false; park(b.id); }
        for (auto& x : explosions) { x.active = false; park(x.id); }
        for (auto& s : spawn_fx) { s.active = false; park(s.id); }
        if (powerup_active) park(powerup_id);
        powerup_active = false;
        powerup_kind = "star";
        powerup_t = 0;
        shovel_ring.clear();
        freeze_timer = 0;
        shovel_timer = 0;
        park(muzzle);
        over_timer = 0;
        bot_path.clear();
        bot_path_age = 0;
    }

    std::vector<EnemySpec> make_enemy_queue(int st) const {
        const int loop = (st - 1) / STAGE_COUNT;
        const int m = (st - 1) % STAGE_COUNT + 1;
        int basic = 14, fast = 4, power = 1, armor = 1;
        if (m == 2) { basic = 10; fast = 5; power = 3; armor = 2; }
        else if (m == 3) { basic = 7; fast = 5; power = 5; armor = 3; }
        else if (m == 4) { basic = 5; fast = 4; power = 6; armor = 5; }
        std::vector<std::string> list;
        for (int i = 0; i < basic; ++i) list.push_back("basic");
        for (int i = 0; i < fast; ++i) list.push_back("fast");
        for (int i = 0; i < power; ++i) list.push_back("power");
        for (int i = 0; i < armor; ++i) list.push_back("armor");
        for (int i = (int)list.size() - 1; i > 0; --i) {
            int j = (int)(host.rng() * (float)(i + 1));
            if (j < 0) j = 0;
            if (j > i) j = i;
            std::swap(list[(size_t)i], list[(size_t)j]);
        }
        std::vector<bool> carrier(list.size(), false);
        const char* kinds[] = { "star", "helmet", "grenade", "shovel", "tank", "timer" };
        for (int k = 0; k < 3; ++k) {
            int idx = 4 + k * 5 + (int)(host.rng() * 2.0F);
            if (idx >= (int)list.size()) idx = (int)list.size() - 1;
            if (idx >= 0) carrier[(size_t)idx] = true;
        }
        std::vector<EnemySpec> q;
        q.reserve(list.size());
        for (int i = 0; i < (int)list.size(); ++i) {
            std::string kind = list[(size_t)i];
            if (loop > 0 && kind == "basic" && (i + 1) % 4 == 0) kind = "armor";
            q.push_back({ kind, carrier[(size_t)i], kinds[(i + loop) % 6] });
        }
        return q;
    }

    void activate_stage(int st) {
        stage = st;
        pending_stage = st;
        if (!collect_stage()) {
            log("tank: stage load failed stage=" + std::to_string(st));
            trigger_game_over("stage_load");
            return;
        }
        reset_actor_pools();
        spawn_queue = make_enemy_queue(st);
        spawn_timer = 0;
        player.level = player_level;
        player.shield = 0;
        player.alive = false;
        player.respawn = 0;
        spawn_player();
        intro_timer = 1.6F;
        state = St::Intro;
        start_bgm();
        log("tank: stage " + std::to_string(stage) + " start enemies=" +
            std::to_string(spawn_queue.size()) + " lives=" + std::to_string(lives) +
            " level=" + std::to_string(player.level) + " frame=" + std::to_string(frame));
    }

    void request_stage(int st) {
        stage = st;
        pending_stage = st;
        want_stage = true;
        stage_tried = false;
        state = St::Loading;
        spawn_queue.clear();
        reset_actor_pools();
        if (player.id >= 0) {
            player.alive = false;
            park(player.id);
            park(player.shield_id);
        }
        if (stage_root >= 0) host.node_set_active((float)stage_root, 0);
        if (stage_inst >= 0) {
            host.scene_unload((float)stage_inst);
            stage_inst = -1;
            stage_root = -1;
        }
        new_grid();
        base_spr = -1;
        if (field_bg >= 0) unpark(field_bg);
        if (field_frame >= 0) unpark(field_frame);
        start_bgm();
        log("tank: stage " + std::to_string(st) + " loading scene=" + stage_path(st) +
            " frame=" + std::to_string(frame));
    }

    void update_loading() {
        if (!want_stage) return;
        if (stage_host < 0) stage_host = find("StageHost");
        if (stage_host < 0) return;
        const int kids = (int)host.node_child_count((float)stage_host);
        if (stage_inst < 0) {
            if (kids > 0) return;
            if (stage_tried) return;
            stage_tried = true;
            stage_inst = (int)host.scene_instantiate((float)stage_host, stage_path(pending_stage), "{}");
            log("tank: stage instance request handle=" + std::to_string(stage_inst) +
                " frame=" + std::to_string(frame));
            if (stage_inst < 0) {
                log("tank: stage instance failed path=" + stage_path(pending_stage));
                trigger_game_over("stage_load");
            }
            return;
        }
        if (kids > 0) {
            activate_stage(pending_stage);
        }
    }

    // ---- 玩家 ----
    std::string player_texture() const {
        return std::string("/art/tank_player_l") + std::to_string(std::clamp(player.level, 0, 3)) +
               "_" + dir_name(player.dir) + ".png";
    }

    void update_player_sprite() {
        if (!player.alive) return;
        set_tex(player.id, player_texture());
        place(player.id, player.x, player.y);
        if (player.shield > 0) {
            unpark(player.shield_id);
            place(player.shield_id, player.x, player.y);
            set_tex(player.shield_id, std::string("/art/shield") + std::to_string((frame / 8) % 2) + ".png");
        } else {
            park(player.shield_id);
        }
        if (player.muzzle_t > 0) {
            const V2 v = dir_vec(player.dir);
            unpark(muzzle);
            place(muzzle, player.x + (float)v.dx * 22.0F, player.y + (float)v.dy * 22.0F);
            host.node_set_opacity((float)muzzle, std::clamp(player.muzzle_t / 0.06F, 0.0F, 1.0F));
        } else {
            park(muzzle);
        }
    }

    bool tank_map_collides(float x, float y) const {
        if (x < FX + CELL || x > FIELD_RIGHT - CELL || y < FY + CELL || y > FIELD_TOP - CELL) return true;
        const int c0 = (int)std::floor((x - TANK_HALF - FX) / CELL) + 1;
        const int c1 = (int)std::floor((x + TANK_HALF - FX) / CELL) + 1;
        const int j0 = (int)std::floor((y - TANK_HALF - FY) / CELL);
        const int j1 = (int)std::floor((y + TANK_HALF - FY) / CELL);
        for (int j = j0; j <= j1; ++j) {
            const int r = ROWS - j;
            for (int c = c0; c <= c1; ++c) {
                if (is_block_tank(cell_at(c, r))) return true;
            }
        }
        return false;
    }

    static bool tanks_overlap(float ax, float ay, float bx, float by) {
        return std::abs(ax - bx) < TANK_HALF * 2.0F && std::abs(ay - by) < TANK_HALF * 2.0F;
    }
    static bool rect_hits(float x, float y, float ox, float oy, float half) {
        return std::abs(x - ox) < half && std::abs(y - oy) < half;
    }

    bool player_can_move(float x, float y) const {
        if (tank_map_collides(x, y)) return false;
        for (const auto& e : enemies) {
            if (e.active && !e.spawning && tanks_overlap(x, y, e.x, e.y)) return false;
        }
        return true;
    }

    void set_player_dir(Dir d) {
        if (player.dir == d) return;
        const float ox = player.x, oy = player.y;
        if (d == Dir::U || d == Dir::D) player.x = snap_to(player.x, FX + CELL, CELL);
        else player.y = snap_to(player.y, FY + CELL, CELL);
        if (!player_can_move(player.x, player.y)) { player.x = ox; player.y = oy; }
        player.dir = d;
        update_player_sprite();
    }

    void move_player(Dir d, float dt) {
        if (player.dir != d) set_player_dir(d);
        const V2 v = dir_vec(player.dir);
        const bool on_ice = player_on_ice();
        const float speed = on_ice ? TANK_SPEED * 1.28F : TANK_SPEED;
        const float nx = player.x + (float)v.dx * speed * dt;
        const float ny = player.y + (float)v.dy * speed * dt;
        if (player_can_move(nx, ny)) {
            player.x = nx;
            player.y = ny;
            player.moved = true;
            if (on_ice) player.slide = 0.12F;
        } else {
            player.moved = false;
        }
        update_player_sprite();
    }

    int player_bullet_count() const {
        int n = 0;
        for (const auto& b : bullets) if (b.active && b.team == 0) ++n;
        return n;
    }

    bool fire_player() {
        if (!player.alive || state != St::Play || player.fire_cd > 0) return false;
        const int max_b = player.level >= 2 ? 2 : 1;
        if (player_bullet_count() >= max_b) return false;
        const V2 v = dir_vec(player.dir);
        const float speed = PLAYER_BULLET_SPEED + (player.level >= 1 ? 45.0F : 0.0F);
        if (!bullet_spawn(0, -1, player.x + (float)v.dx * 20.0F, player.y + (float)v.dy * 20.0F,
                          player.dir, speed, player.level >= 3))
            return false;
        player.fire_cd = PLAYER_FIRE_CD - (player.level >= 1 ? 0.06F : 0.0F);
        player.muzzle_t = 0.06F;
        sfx("fire", 0.38F);
        log("tank: player fire dir=" + std::string(dir_name(player.dir)) +
            " level=" + std::to_string(player.level) + " frame=" + std::to_string(frame));
        return true;
    }

    void spawn_player() {
        player.alive = true;
        player.x = player_start_x;
        player.y = player_start_y;
        player.dir = Dir::U;
        player.level = player_level;
        player.shield = PLAYER_SHIELD_TIME;
        player.fire_cd = 0;
        player.muzzle_t = 0;
        player.respawn = 0;
        player.slide = 0;
        player.flash = 0;
        unpark(player.id);
        update_player_sprite();
        log("tank: player spawn x=" + std::to_string((int)player.x) +
            " y=" + std::to_string((int)player.y) + " level=" + std::to_string(player.level) +
            " shield=" + std::to_string(player.shield) + " frame=" + std::to_string(frame));
    }

    void damage_player() {
        if (!player.alive || env_god) return;
        player.alive = false;
        park(player.id);
        park(player.shield_id);
        park(muzzle);
        spawn_explosion(player.x, player.y, 1.35F);
        sfx("player_hit", 0.85F);
        if (lives > 0) {
            --lives;
            player.respawn = RESPAWN_TIME;
            log("tank: player dead hp=0 lives=" + std::to_string(lives) +
                " frame=" + std::to_string(frame));
        } else {
            lives = 0;
            log("tank: player dead hp=0 lives=0 frame=" + std::to_string(frame));
            trigger_game_over("player");
        }
    }

    void update_player(float dt) {
        if (player.id < 0) return;
        if (!player.alive) {
            if (player.respawn > 0) {
                player.respawn -= dt;
                if (player.respawn <= 0) {
                    player.respawn = 0;
                    player_level = 0;
                    spawn_player();
                }
            }
            return;
        }
        if (player.fire_cd > 0) player.fire_cd -= dt;
        if (player.muzzle_t > 0) player.muzzle_t -= dt;
        if (player.shield > 0) player.shield -= dt;

        Dir want = player.dir;
        bool has = false;
        if (env_autoplay) {
            has = bot_dir(want);
        } else {
            if (key(8) || key(87) || key(119)) { want = Dir::U; has = true; }
            else if (key(9) || key(83) || key(115)) { want = Dir::D; has = true; }
            else if (key(6) || key(65) || key(97)) { want = Dir::L; has = true; }
            else if (key(7) || key(68) || key(100)) { want = Dir::R; has = true; }
        }

        if (has) {
            move_player(want, dt);
            if (env_autoplay && bot_want_fire()) fire_player();
        } else if (player_on_ice() && player.slide > 0) {
            player.slide -= dt;
            const V2 v = dir_vec(player.dir);
            const float nx = player.x + (float)v.dx * TANK_SPEED * 0.72F * dt;
            const float ny = player.y + (float)v.dy * TANK_SPEED * 0.72F * dt;
            if (player_can_move(nx, ny)) {
                player.x = nx;
                player.y = ny;
            }
        } else {
            player.slide = 0;
        }

        if (!env_autoplay &&
            (key(2) || key(32) || key(74) || key(106) || key(90) || key(122) ||
             host.input_down() != 0))
            fire_player();
        update_player_sprite();
    }

    // ---- 敌人 ----
    bool any_enemy_bullet(int owner) const {
        for (const auto& b : bullets)
            if (b.active && b.team == 1 && b.owner == owner) return true;
        return false;
    }

    bool enemy_can_move(const Enemy& e, float x, float y) const {
        if (tank_map_collides(x, y)) return false;
        if (player.alive && tanks_overlap(x, y, player.x, player.y)) return false;
        for (const auto& o : enemies) {
            if (&o != &e && o.active && !o.spawning && tanks_overlap(x, y, o.x, o.y)) return false;
        }
        return true;
    }

    bool enemy_dir_valid(Enemy& e, Dir d) {
        const V2 v = dir_vec(d);
        const float ox = e.x, oy = e.y;
        if (d == Dir::U || d == Dir::D) e.x = snap_to(e.x, FX + CELL, CELL);
        else e.y = snap_to(e.y, FY + CELL, CELL);
        const bool ok = enemy_can_move(e, e.x + (float)v.dx * 4.0F, e.y + (float)v.dy * 4.0F);
        e.x = ox;
        e.y = oy;
        return ok;
    }

    void set_enemy_dir(Enemy& e, Dir d) {
        if (e.dir == d) return;
        const float ox = e.x, oy = e.y;
        if (d == Dir::U || d == Dir::D) e.x = snap_to(e.x, FX + CELL, CELL);
        else e.y = snap_to(e.y, FY + CELL, CELL);
        if (!enemy_can_move(e, e.x, e.y)) { e.x = ox; e.y = oy; }
        e.dir = d;
        update_enemy_sprite(e);
    }

    void choose_enemy_dir(Enemy& e) {
        static const Dir order[] = { Dir::U, Dir::R, Dir::D, Dir::L };
        Dir candidates[4];
        int n = 0;
        for (Dir d : order) if (enemy_dir_valid(e, d)) candidates[n++] = d;
        if (n == 0) return;
        if (player.alive) {
            const float dx = player.x - e.x, dy = player.y - e.y;
            if (std::abs(dx) < 10) {
                const Dir want = dy > 0 ? Dir::U : Dir::D;
                if (enemy_dir_valid(e, want)) { set_enemy_dir(e, want); return; }
            } else if (std::abs(dy) < 10) {
                const Dir want = dx > 0 ? Dir::R : Dir::L;
                if (enemy_dir_valid(e, want)) { set_enemy_dir(e, want); return; }
            }
        }
        Dir best = candidates[0];
        float best_w = -1e9F;
        for (int i = 0; i < n; ++i) {
            const Dir d = candidates[i];
            float w = rngf() * 1.5F;
            if (d == Dir::D && e.y > base_y) w += 3.0F;
            if (player.alive) {
                const float pdx = player.x - e.x, pdy = player.y - e.y;
                if (std::abs(pdx) > std::abs(pdy)) {
                    if (d == (pdx > 0 ? Dir::R : Dir::L)) w += 1.6F;
                } else {
                    if (d == (pdy > 0 ? Dir::U : Dir::D)) w += 1.6F;
                }
            }
            if (d == e.dir) w += 1.1F;
            const Dir back = e.dir == Dir::U ? Dir::D : e.dir == Dir::D ? Dir::U
                           : e.dir == Dir::L ? Dir::R : Dir::L;
            if (d == back) w -= 1.3F;
            if (w > best_w) { best_w = w; best = d; }
        }
        set_enemy_dir(e, best);
    }

    std::string enemy_texture(const Enemy& e) const {
        if (e.kind == "armor")
            return std::string("/art/tank_armor_hp") + std::to_string(std::clamp(e.hp, 1, 4)) +
                   "_" + dir_name(e.dir) + ".png";
        return std::string("/art/tank_") + e.kind + "_" + dir_name(e.dir) + ".png";
    }

    void update_enemy_sprite(const Enemy& e) {
        if (!e.active || e.spawning) return;
        set_tex(e.id, enemy_texture(e));
        place(e.id, e.x, e.y);
        if (e.flash > 0) { set_rgb(e.id, 2.6F, 2.6F, 2.6F); host.node_set_opacity((float)e.id, 1.0F); return; }
        float k = 1.0F;
        if (e.carrier && (frame / 8) % 2 == 0) k = 1.9F;
        set_rgb(e.id, k, k, k);
        host.node_set_opacity((float)e.id, 1.0F);
    }

    static bool aligned_shot(float x1, float y1, float x2, float y2, Dir d, float max_dist) {
        switch (d) {
            case Dir::U: return std::abs(x1 - x2) < 12 && y2 > y1 && (y2 - y1) < max_dist;
            case Dir::D: return std::abs(x1 - x2) < 12 && y2 < y1 && (y1 - y2) < max_dist;
            case Dir::R: return std::abs(y1 - y2) < 12 && x2 > x1 && (x2 - x1) < max_dist;
            case Dir::L: return std::abs(y1 - y2) < 12 && x2 < x1 && (x1 - x2) < max_dist;
        }
        return false;
    }

    void enemy_fire(Enemy& e) {
        if (any_enemy_bullet(e.idx)) return;
        const V2 v = dir_vec(e.dir);
        const EnemyDef def = enemy_def(e.kind);
        if (bullet_spawn(1, e.idx, e.x + (float)v.dx * 20.0F, e.y + (float)v.dy * 20.0F,
                         e.dir, def.bullet, false))
            e.fire_cd = ENEMY_FIRE_CD_MIN + rngf() * (ENEMY_FIRE_CD_MAX - ENEMY_FIRE_CD_MIN);
    }

    bool spawn_enemy(const EnemySpec& spec) {
        Enemy* e = nullptr;
        for (auto& x : enemies) if (!x.active) { e = &x; break; }
        if (!e || spawn_points.empty()) return false;
        SpawnPoint chosen{};
        bool found = false;
        const int n = (int)spawn_points.size();
        const int start = (e->idx + stage) % std::max(1, n);
        for (int k = 0; k < n; ++k) {
            const SpawnPoint& p = spawn_points[(size_t)((start + k) % n)];
            bool occupied = false;
            if (player.alive && tanks_overlap(p.x, p.y, player.x, player.y)) occupied = true;
            for (const auto& o : enemies)
                if (o.active && tanks_overlap(p.x, p.y, o.x, o.y)) occupied = true;
            if (!occupied) { chosen = p; found = true; break; }
        }
        if (!found) return false;
        const EnemyDef def = enemy_def(spec.kind);
        e->active = true;
        e->spawning = true;
        e->spawn_t = 0.80F;
        e->invuln = 0.80F;
        e->x = chosen.x;
        e->y = chosen.y;
        e->dir = Dir::D;
        e->kind = spec.kind;
        e->hp = def.hp;
        e->max_hp = def.hp;
        e->speed = def.speed * (1.0F + (float)std::max(0, stage - 4) * 0.03F);
        e->carrier = spec.carrier;
        e->power_kind = spec.power_kind;
        e->fire_cd = 0.45F + rngf() * 0.55F;
        e->think = 0.15F;
        e->flash = 0;
        e->stuck = 0;
        place(e->id, e->x, e->y);
        set_tex(e->id, enemy_texture(*e));
        host.node_set_opacity((float)e->id, 1.0F);
        set_rgb(e->id, 1, 1, 1);
        park(e->id);
        show_spawn_fx(e->idx, e->x, e->y);
        log("tank: enemy spawn idx=" + std::to_string(e->idx) + " kind=" + e->kind +
            " carrier=" + std::string(e->carrier ? "1" : "0") +
            " x=" + std::to_string((int)e->x) + " y=" + std::to_string((int)e->y) +
            " remain=" + std::to_string(spawn_queue.size() + (size_t)active_enemies()) +
            " frame=" + std::to_string(frame));
        return true;
    }

    void update_enemies(float dt) {
        for (auto& e : enemies) {
            if (!e.active) continue;
            if (e.spawning) {
                e.spawn_t -= dt;
                if (e.spawn_t <= 0) {
                    e.spawning = false;
                    e.spawn_t = 0;
                    e.invuln = 1.0F;
                    unpark(e.id);
                    update_enemy_sprite(e);
                    log("tank: enemy enter idx=" + std::to_string(e.idx) +
                        " frame=" + std::to_string(frame));
                }
                continue;
            }
            if (e.invuln > 0) e.invuln -= dt;
            if (e.flash > 0) e.flash -= dt;
            if (e.fire_cd > 0) e.fire_cd -= dt;
            if (freeze_timer <= 0) {
                e.think -= dt;
                if (e.think <= 0) {
                    choose_enemy_dir(e);
                    e.think = 0.30F + rngf() * 0.75F;
                }
                const V2 v = dir_vec(e.dir);
                const float nx = e.x + (float)v.dx * e.speed * dt;
                const float ny = e.y + (float)v.dy * e.speed * dt;
                if (enemy_can_move(e, nx, ny)) {
                    e.x = nx;
                    e.y = ny;
                    e.stuck = 0;
                } else {
                    e.stuck += dt;
                    if (e.stuck > 0.05F) {
                        choose_enemy_dir(e);
                        e.think = 0.20F + rngf() * 0.35F;
                        e.stuck = 0;
                    }
                }
                if (e.fire_cd <= 0 && !any_enemy_bullet(e.idx)) {
                    bool want = false;
                    if (player.alive && aligned_shot(e.x, e.y, player.x, player.y, e.dir, 290)) want = true;
                    if (aligned_shot(e.x, e.y, base_x, base_y, e.dir, 420)) want = true;
                    if (rngf() < 0.18F) want = true;
                    if (want) enemy_fire(e);
                }
            }
            update_enemy_sprite(e);
        }
    }

    void damage_enemy(Enemy& e, int dmg, float hx, float hy) {
        if (!e.active || e.spawning || e.invuln > 0) return;
        e.hp -= dmg;
        e.flash = 0.12F;
        spawn_explosion(hx, hy, 0.55F);
        sfx("hit", 0.4F);
        log("tank: hit enemy=" + std::to_string(e.idx) + " kind=" + e.kind +
            " hp=" + std::to_string(e.hp) + " frame=" + std::to_string(frame));
        if (e.hp <= 0) kill_enemy(e);
        else update_enemy_sprite(e);
    }

    int active_enemies() const {
        int n = 0;
        for (const auto& e : enemies) if (e.active) ++n;
        return n;
    }

    void add_score(int n) {
        score += n;
        if (score > hi_score) hi_score = score;
        while (score >= next_extra) {
            ++lives;
            next_extra += 20000;
            sfx("powerup", 0.55F);
            log("tank: extra life score=" + std::to_string(score) +
                " lives=" + std::to_string(lives) + " frame=" + std::to_string(frame));
        }
    }

    void kill_enemy(Enemy& e, bool silent = false) {
        if (!e.active) return;
        e.active = false;
        e.dead = true;
        park(e.id);
        spawn_explosion(e.x, e.y, 1.25F);
        sfx("explode", 0.62F);
        const int pts = enemy_def(e.kind).score;
        add_score(pts);
        log("tank: kill idx=" + std::to_string(e.idx) + " kind=" + e.kind +
            " score=" + std::to_string(pts) + " total_score=" + std::to_string(score) +
            " remaining=" + std::to_string(spawn_queue.size() + (size_t)active_enemies()) +
            " frame=" + std::to_string(frame));
        if (e.carrier && !silent && !e.power_kind.empty())
            spawn_powerup(e.power_kind, e.x, e.y);
        if (spawn_queue.empty() && active_enemies() == 0) start_stage_clear();
    }

    // ---- 道具（6 种）----
    static const char* power_tex(const std::string& kind) {
        if (kind == "helmet") return "/art/power_helmet.png";
        if (kind == "grenade") return "/art/power_grenade.png";
        if (kind == "shovel") return "/art/power_shovel.png";
        if (kind == "tank") return "/art/power_tank.png";
        if (kind == "timer") return "/art/power_timer.png";
        return "/art/power_star.png";
    }

    void remove_powerup() {
        if (!powerup_active) return;
        powerup_active = false;
        park(powerup_id);
    }

    void spawn_powerup(const std::string& kind, float x, float y) {
        if (powerup_active) remove_powerup();
        powerup_active = true;
        powerup_kind = kind;
        powerup_x = x;
        powerup_y = y;
        powerup_t = 15.0F;
        place(powerup_id, x, y);
        set_tex(powerup_id, power_tex(kind));
        host.node_set_opacity((float)powerup_id, 1.0F);
        unpark(powerup_id);
        log("tank: powerup spawn kind=" + kind + " x=" + std::to_string((int)x) +
            " y=" + std::to_string((int)y) + " frame=" + std::to_string(frame));
    }

    void activate_shovel() {
        shovel_ring.clear();
        static const int cells[][2] = {
            { 12, 23 }, { 13, 23 }, { 14, 23 }, { 15, 23 },
            { 12, 24 }, { 13, 24 }, { 14, 24 }, { 15, 24 },
            { 11, 25 }, { 12, 25 }, { 11, 26 }, { 12, 26 },
            { 15, 25 }, { 16, 25 }, { 15, 26 }, { 16, 26 },
        };
        for (const auto& cell : cells) {
            shovel_ring.emplace_back(cell[0], cell[1]);
            set_cell(cell[0], cell[1], '@');
        }
        shovel_timer = 12.0F;
        log("tank: powerup shovel base walls steel frame=" + std::to_string(frame));
    }

    void update_shovel(float dt) {
        if (shovel_timer <= 0) return;
        shovel_timer -= dt;
        if (shovel_timer > 0) {
            const bool blink = shovel_timer <= 3.0F && ((frame / 8) % 2 == 0);
            for (const auto& p : shovel_ring) {
                const char ch = map[(size_t)p.second][(size_t)p.first];
                if (ch == '@' || ch == '#') set_cell(p.first, p.second, blink ? '#' : '@');
            }
        } else {
            shovel_timer = 0;
            for (const auto& p : shovel_ring) {
                const char ch = map[(size_t)p.second][(size_t)p.first];
                if (ch != 'B' && ch != '.') set_cell(p.first, p.second, '#');
            }
            shovel_ring.clear();
            log("tank: shovel expired frame=" + std::to_string(frame));
        }
    }

    void apply_powerup(const std::string& kind) {
        remove_powerup();
        sfx("powerup", 0.7F);
        if (kind == "star") {
            if (player.level < 3) {
                ++player.level;
                player_level = player.level;
                update_player_sprite();
            } else {
                add_score(1000);
            }
            log("tank: powerup star level=" + std::to_string(player.level) +
                " frame=" + std::to_string(frame));
        } else if (kind == "helmet") {
            player.shield = std::max(player.shield, 10.0F);
            log("tank: powerup helmet shield=10 frame=" + std::to_string(frame));
        } else if (kind == "grenade") {
            int n = 0;
            for (auto& e : enemies) {
                if (e.active) {
                    kill_enemy(e, true);
                    add_score(200);
                    ++n;
                }
            }
            log("tank: powerup grenade kills=" + std::to_string(n) +
                " frame=" + std::to_string(frame));
        } else if (kind == "shovel") {
            activate_shovel();
        } else if (kind == "tank") {
            ++lives;
            log("tank: powerup tank lives=" + std::to_string(lives) +
                " frame=" + std::to_string(frame));
        } else if (kind == "timer") {
            freeze_timer = 8.0F;
            log("tank: powerup timer freeze=8 frame=" + std::to_string(frame));
        }
    }

    void update_powerup(float dt) {
        if (!powerup_active) return;
        if (player.alive && std::abs(player.x - powerup_x) < 24 &&
            std::abs(player.y - powerup_y) < 24) {
            apply_powerup(powerup_kind);
            return;
        }
        powerup_t -= dt;
        if (powerup_t <= 0) { remove_powerup(); return; }
        host.node_set_opacity((float)powerup_id,
                              (frame / 10) % 2 == 0 ? 1.0F : 0.45F);
    }

    void update_spawns(float dt) {
        if (state != St::Play) return;
        if (spawn_queue.empty() || active_enemies() >= MAX_ACTIVE_ENEMIES) return;
        spawn_timer -= dt;
        if (spawn_timer > 0) return;
        if (spawn_enemy(spawn_queue.front())) {
            spawn_queue.erase(spawn_queue.begin());
            spawn_timer = 2.2F;
        } else {
            spawn_timer = 0.5F;
        }
    }

    void spawn_initial_wave() {
        int n = 0;
        while (n < MAX_ACTIVE_ENEMIES && !spawn_queue.empty()) {
            if (!spawn_enemy(spawn_queue.front())) break;
            spawn_queue.erase(spawn_queue.begin());
            ++n;
        }
        spawn_timer = 2.6F;
    }

    void begin_play() {
        state = St::Play;
        spawn_initial_wave();
        log("tank: stage " + std::to_string(stage) + " play frame=" + std::to_string(frame));
    }

    // ---- 子弹 ----
    Bullet* bullet_spawn(int team, int owner, float x, float y, Dir d, float speed, bool power) {
        Bullet* b = nullptr;
        for (auto& x2 : bullets) if (!x2.active) { b = &x2; break; }
        if (!b) return nullptr;
        const V2 v = dir_vec(d);
        b->active = true;
        b->team = team;
        b->owner = owner;
        b->x = x;
        b->y = y;
        b->vx = (float)v.dx;
        b->vy = (float)v.dy;
        b->speed = speed;
        b->power = power;
        b->life = 0;
        place(b->id, x, y);
        set_tex(b->id, "/art/bullet.png");
        unpark(b->id);
        if (team == 1) sfx("fire", 0.25F);
        return b;
    }

    void bullet_kill(Bullet& b, bool spark) {
        if (!b.active) return;
        b.active = false;
        park(b.id);
        if (spark) spawn_explosion(b.x, b.y, 0.45F);
    }

    int break_cell_group(int c, int r, char ch) {
        const int gc = c - ((c - 1) % 2);
        const int gr = r - ((r - 1) % 2);
        bool full = true;
        for (int rr = gr; rr <= gr + 1; ++rr)
            for (int cc = gc; cc <= gc + 1; ++cc)
                if (cc < 1 || cc > COLS || rr < 1 || rr > ROWS || map[(size_t)rr][(size_t)cc] != ch)
                    full = false;
        int n = 0;
        if (full) {
            for (int rr = gr; rr <= gr + 1; ++rr)
                for (int cc = gc; cc <= gc + 1; ++cc) { set_cell(cc, rr, '.'); ++n; }
        } else if (map[(size_t)r][(size_t)c] == ch) {
            set_cell(c, r, '.');
            n = 1;
        }
        return n;
    }

    bool bullet_check_cell(Bullet& b) {
        if (b.x < FX || b.x > FIELD_RIGHT || b.y < FY || b.y > FIELD_TOP) {
            bullet_kill(b, true);
            return true;
        }
        const int c = std::clamp((int)std::floor((b.x - FX) / CELL) + 1, 1, COLS);
        const int r = std::clamp(ROWS - (int)std::floor((b.y - FY) / CELL), 1, ROWS);
        const char ch = cell_at(c, r);
        if (ch == '#') {
            const int n = break_cell_group(c, r, '#');
            sfx("brick", 0.45F);
            spawn_explosion(b.x, b.y, 0.55F);
            bullet_kill(b, false);
            log("tank: brick broke c=" + std::to_string(c) + " r=" + std::to_string(r) +
                " cells=" + std::to_string(n) + " frame=" + std::to_string(frame));
            return true;
        }
        if (ch == '@') {
            if (b.power) {
                const int n = break_cell_group(c, r, '@');
                sfx("brick", 0.55F);
                spawn_explosion(b.x, b.y, 0.75F);
                bullet_kill(b, false);
                log("tank: steel broke c=" + std::to_string(c) + " cells=" + std::to_string(n));
            } else {
                sfx("steel", 0.32F);
                spawn_explosion(b.x, b.y, 0.35F);
                bullet_kill(b, false);
                log("tank: steel hit c=" + std::to_string(c) + " r=" + std::to_string(r));
            }
            return true;
        }
        if (ch == 'B') {
            bullet_kill(b, false);
            destroy_base(b.x, b.y);
            return true;
        }
        return false;
    }

    bool bullet_hit_tank(Bullet& b) {
        if (b.team == 0) {
            for (auto& e : enemies) {
                if (e.active && !e.spawning && e.invuln <= 0 &&
                    rect_hits(b.x, b.y, e.x, e.y, TANK_HALF + 5)) {
                    damage_enemy(e, 1, b.x, b.y);
                    bullet_kill(b, true);
                    return true;
                }
            }
        } else if (player.alive && rect_hits(b.x, b.y, player.x, player.y, TANK_HALF + 5)) {
            if (player.shield > 0 || env_god) {
                spawn_explosion(b.x, b.y, 0.5F);
                bullet_kill(b, false);
            } else {
                damage_player();
                bullet_kill(b, true);
            }
            return true;
        }
        return false;
    }

    void update_bullets(float dt) {
        for (auto& b : bullets) {
            if (!b.active) continue;
            const float travel = b.speed * dt;
            int steps = (int)std::ceil(travel / 5.0F);
            if (steps < 1) steps = 1;
            bool dead = false;
            for (int s = 0; s < steps && !dead; ++s) {
                b.x += b.vx * travel / (float)steps;
                b.y += b.vy * travel / (float)steps;
                if (bullet_check_cell(b)) { dead = true; break; }
                if (bullet_hit_tank(b)) { dead = true; break; }
                if (b.team == 0) {
                    for (auto& o : bullets) {
                        if (o.active && o.team == 1 &&
                            std::abs(o.x - b.x) < 7.0F && std::abs(o.y - b.y) < 7.0F) {
                            bullet_kill(o, true);
                            bullet_kill(b, false);
                            dead = true;
                            break;
                        }
                    }
                }
            }
            if (!dead) {
                place(b.id, b.x, b.y);
                b.life += dt;
                if (b.life > 3.0F) bullet_kill(b, false);
            }
        }
    }

    // ---- 特效 ----
    void spawn_explosion(float x, float y, float scale) {
        for (auto& e : explosions) {
            if (e.active) continue;
            e.active = true;
            e.x = x;
            e.y = y;
            e.t = 0;
            e.scale = scale;
            place(e.id, x, y);
            host.node_set_scale((float)e.id, scale);
            host.node_set_opacity((float)e.id, 1.0F);
            unpark(e.id);
            set_tex(e.id, "/art/explosion0.png");
            return;
        }
    }

    void update_explosions(float dt) {
        for (auto& e : explosions) {
            if (!e.active) continue;
            e.t += dt;
            const int f = (int)std::floor(e.t / 0.055F);
            if (f >= 5) {
                e.active = false;
                park(e.id);
            } else {
                set_tex(e.id, "/art/explosion" + std::to_string(f) + ".png");
                host.node_set_scale((float)e.id, e.scale * (1.0F + (float)f * 0.16F));
            }
        }
    }

    void show_spawn_fx(int idx, float x, float y) {
        if (idx < 1 || idx > (int)spawn_fx.size()) return;
        SpawnFx& s = spawn_fx[(size_t)(idx - 1)];
        s.active = true;
        s.t = 0;
        place(s.id, x, y);
        host.node_set_opacity((float)s.id, 1.0F);
        unpark(s.id);
        set_tex(s.id, "/art/spawn0.png");
    }

    void update_spawn_fx(float dt) {
        for (auto& s : spawn_fx) {
            if (!s.active) continue;
            s.t += dt;
            const int f = (int)std::floor(s.t / 0.10F);
            if (f >= 4) {
                s.active = false;
                park(s.id);
            } else {
                set_tex(s.id, "/art/spawn" + std::to_string(f) + ".png");
            }
        }
    }

    // ---- 基地/关卡流程 ----
    void destroy_base(float x, float y) {
        if (!base_alive) return;
        base_alive = false;
        if (base_spr >= 0) set_tex(base_spr, "/art/base_dead.png");
        spawn_explosion(base_x, base_y, 1.7F);
        spawn_explosion(base_x - 12, base_y + 8, 1.0F);
        sfx("explode", 0.85F);
        for (auto& row : map)
            for (char& ch : row) if (ch == 'B') ch = '.';
        log("tank: base destroyed x=" + std::to_string((int)x) + " y=" +
            std::to_string((int)y) + " frame=" + std::to_string(frame));
        trigger_game_over("base");
    }

    void start_stage_clear() {
        if (state != St::Play) return;
        state = St::Clear;
        stage_clear_timer = 2.4F;
        for (auto& b : bullets) bullet_kill(b, false);
        sfx("stage_clear", 0.75F);
        log("tank: stage " + std::to_string(stage) + " clear score=" +
            std::to_string(score) + " lives=" + std::to_string(lives));
    }

    void trigger_game_over(const char* reason) {
        if (state == St::Over) return;
        if (score > hi_score) hi_score = score;
        state = St::Over;
        over_timer = 0;
        over_accept = false;
        stop_bgm();
        for (auto& b : bullets) bullet_kill(b, false);
        sfx("game_over", 0.8F);
        log(std::string("tank: game over reason=") + reason + " score=" +
            std::to_string(score) + " stage=" + std::to_string(stage) +
            " lives=" + std::to_string(lives) + " frame=" + std::to_string(frame));
    }

    void hide_actors() {
        park(player.id);
        park(player.shield_id);
        park(muzzle);
        for (auto& e : enemies) park(e.id);
        for (auto& b : bullets) park(b.id);
        for (auto& x : explosions) park(x.id);
        for (auto& s : spawn_fx) park(s.id);
        park(powerup_id);
    }

    void return_to_title() {
        state = St::Title;
        want_stage = false;
        stage_tried = false;
        stop_bgm();
        hide_actors();
        if (stage_root >= 0) host.node_set_active((float)stage_root, 0);
        if (stage_inst >= 0) {
            host.scene_unload((float)stage_inst);
            stage_inst = -1;
            stage_root = -1;
        }
        if (field_bg >= 0) park(field_bg);
        if (field_frame >= 0) park(field_frame);
        stage_clear_timer = 0;
        over_timer = 0;
        over_accept = false;
        log("tank: title frame=" + std::to_string(frame));
    }

    void new_game() {
        player_level = 0;
        stage = stage_override > 0 ? stage_override : 1;
        score = 0;
        lives = 2;
        next_extra = 20000;
        over_timer = 0;
        over_accept = false;
        player.level = 0;
        player.alive = false;
        player.shield = 0;
        request_stage(stage);
        log("tank: new game stage=" + std::to_string(stage) + " lives=" +
            std::to_string(lives) + " frame=" + std::to_string(frame));
    }

    // ---- HUD（与 hud.lua 的 global 通道）----
    std::string overlay_text() const {
        switch (state) {
            case St::Loading: return "STAGE " + pad_num(pending_stage, 2);
            case St::Intro: return "STAGE " + pad_num(stage, 2);
            case St::Pause: return "PAUSED";
            case St::Clear: return "STAGE " + pad_num(stage, 2) + " CLEAR";
            case St::Over: return "GAME OVER";
            default: return "";
        }
    }

    void publish_hud() {
        const std::string mode = state == St::Title ? "title" : "battle";
        const std::string overlay = state == St::Title ? "" : overlay_text();
        const double score_v = score;
        const double hi_v = std::max(hi_score, score);
        const double stage_v = state == St::Loading ? pending_stage : stage;
        const double lives_v = lives;
        const double enemy_v = state == St::Title ? 0 :
            (state == St::Loading ? ENEMIES_PER_STAGE : (double)(spawn_queue.size() + (size_t)active_enemies()));
        const double prompt_v = over_accept ? 1 : 0;
        if (mode != hud_mode) { host.global_set_text("hud.mode", mode); hud_mode = mode; }
        if (overlay != hud_overlay) { host.global_set_text("hud.overlay", overlay); hud_overlay = overlay; }
        if (score_v != hud_score) { host.global_set("hud.score", (float)score_v); hud_score = score_v; }
        if (hi_v != hud_hi) { host.global_set("hud.hi", (float)hi_v); hud_hi = hi_v; }
        if (stage_v != hud_stage) { host.global_set("hud.stage", (float)stage_v); hud_stage = stage_v; }
        if (lives_v != hud_lives) { host.global_set("hud.lives", (float)lives_v); hud_lives = lives_v; }
        if (enemy_v != hud_enemy) { host.global_set("hud.enemy", (float)enemy_v); hud_enemy = enemy_v; }
        if (prompt_v != hud_prompt) { host.global_set("hud.prompt", (float)prompt_v); hud_prompt = prompt_v; }
    }

    // ---- 音频 ----
    void sfx(const std::string& name, float vol) {
        if (!audio_on) return;
        host.audio_play("audio/" + name + ".wav", vol, 0, "sfx");
    }
    void start_bgm() {
        if (!audio_on || !bgm_on || bgm_voice != 0) return;
        bgm_voice = host.audio_play("audio/battle_loop.wav", 0.17F, 1, "music");
    }
    void stop_bgm() {
        if (!audio_on || bgm_voice == 0) return;
        host.audio_stop(bgm_voice);
        bgm_voice = 0;
    }

    // ---- 生命周期 ----
    void on_start() override {
        const std::string audio = envv("MAQUE_AUDIO");
        audio_on = host.audio_enabled() != 0 && !audio.empty() && audio != "off" && audio != "null";
        bgm_on = envv("MAQUE_TANK_BGM") != "0";
        if (!audio_on) log("tank: audio off (MAQUE_AUDIO=off/null)");
        env_god = envv("MAQUE_TANK_GOD") == "1";
        env_skip_title = envv("MAQUE_TANK_SKIP_TITLE") == "1";
        env_trace = envv("MAQUE_TANK_TRACE") == "1";
        env_autoplay = envv("MAQUE_TANK_AUTOPLAY") == "1";
        stage_override = std::atoi(envv("MAQUE_TANK_STAGE").c_str());
        hook_brick_at = std::atoi(envv("MAQUE_TANK_BRICK_AT").c_str());
        hook_base_at = std::atoi(envv("MAQUE_TANK_BASE_AT").c_str());
        hook_power_at = std::atoi(envv("MAQUE_TANK_POWER_AT").c_str());
        hook_power_kind = envv("MAQUE_TANK_POWER_KIND");
        if (hook_power_kind.empty()) hook_power_kind = "star";
        hook_clear_at = std::atoi(envv("MAQUE_TANK_CLEAR_AT").c_str());

        bg = find("bg");
        field_bg = find("field_bg");
        field_frame = find("field_frame");
        stage_host = find("StageHost");
        if (bg >= 0) host.node_set_z_index((float)bg, -100);
        if (field_bg >= 0) { host.node_set_z_index((float)field_bg, 0); park(field_bg); }
        if (field_frame >= 0) { host.node_set_z_index((float)field_frame, 2); park(field_frame); }

        new_grid();
        if (!init_actors()) return;
        state = St::Title;
        hide_actors();
        publish_hud();
        log("tank: ready 640x480 stages=" + std::to_string(STAGE_COUNT) +
            " enemies=" + std::to_string(ENEMIES_PER_STAGE) + " lua=0 autoplay=" +
            (env_autoplay ? "1" : "0"));
        if (env_skip_title || env_autoplay || stage_override > 0 ||
            hook_brick_at > 0 || hook_base_at > 0 || hook_power_at > 0)
            new_game();
    }

    bool init_actors() {
        player.id = find("p_body");
        player.shield_id = find("shield_body");
        if (player.id < 0 || player.shield_id < 0) {
            log("tank: missing player prefab actors");
            host.quit();
            return false;
        }
        park(player.id);
        park(player.shield_id);
        host.node_set_z_index((float)player.id, 20);
        host.node_set_z_index((float)player.shield_id, 55);
        set_tex(player.id, "/art/tank_player_l0_u.png");
        set_tex(player.shield_id, "/art/shield0.png");

        for (int i = 1; i <= MAX_ACTIVE_ENEMIES; ++i) {
            const std::string nm = "e" + std::to_string(i) + "_body";
            const int id = find(nm.c_str());
            if (id < 0) { log("tank: missing enemy prefab " + std::to_string(i)); host.quit(); return false; }
            park(id);
            host.node_set_z_index((float)id, 20);
            enemies[(size_t)(i - 1)].idx = i;
            enemies[(size_t)(i - 1)].id = id;
        }
        for (int i = 1; i <= 20; ++i) {
            char nm[32];
            std::snprintf(nm, sizeof nm, "b%02d_body", i);
            const int id = find(nm);
            if (id < 0) { log("tank: missing bullet prefab"); host.quit(); return false; }
            park(id);
            host.node_set_z_index((float)id, 30);
            bullets[(size_t)(i - 1)].id = id;
        }
        for (int i = 1; i <= 14; ++i) {
            char nm[32];
            std::snprintf(nm, sizeof nm, "x%02d_body", i);
            const int id = find(nm);
            if (id < 0) { log("tank: missing explosion prefab"); host.quit(); return false; }
            park(id);
            host.node_set_z_index((float)id, 50);
            explosions[(size_t)(i - 1)].id = id;
        }
        for (int i = 1; i <= MAX_ACTIVE_ENEMIES; ++i) {
            char nm[32];
            std::snprintf(nm, sizeof nm, "s%d_body", i);
            const int id = find(nm);
            if (id < 0) { log("tank: missing spawn prefab"); host.quit(); return false; }
            park(id);
            host.node_set_z_index((float)id, 56);
            spawn_fx[(size_t)(i - 1)].id = id;
        }
        powerup_id = find("power_body");
        muzzle = find("muzzle_body");
        if (powerup_id < 0 || muzzle < 0) {
            log("tank: missing powerup/muzzle prefab");
            host.quit();
            return false;
        }
        park(powerup_id);
        host.node_set_z_index((float)powerup_id, 25);
        park(muzzle);
        host.node_set_z_index((float)muzzle, 35);
        set_tex(muzzle, "/art/explosion0.png");
        return true;
    }

    // ---- 规则探针（与 Lua 版同名同日志）----
    void hook_brick() {
        if (!player.alive) return;
        player.x = FX + 9.0F * CELL;
        player.y = FY + 8.0F * CELL;
        player.dir = Dir::U;
        for (int r = 11; r <= 16; ++r)
            for (int c = 8; c <= 11; ++c)
                if (map[(size_t)r][(size_t)c] == '#' || map[(size_t)r][(size_t)c] == '@')
                    set_cell(c, r, '.');
        for (int r = 9; r <= 10; ++r)
            for (int c = 9; c <= 10; ++c)
                set_cell(c, r, '#');
        player.level = std::max(player.level, 1);
        player.fire_cd = 0;
        player.shield = 99.0F;
        update_player_sprite();
        fire_player();
        log("tank: drill brick armed x=" + std::to_string((int)player.x) +
            " y=" + std::to_string((int)player.y) + " frame=" + std::to_string(frame));
    }

    void hook_base() {
        if (!player.alive) return;
        player.x = base_x;
        player.y = base_y + 72.0F;
        player.dir = Dir::D;
        player.shield = 99.0F;
        for (int r = 21; r <= 25; ++r)
            for (int c = 12; c <= 15; ++c)
                if (map[(size_t)r][(size_t)c] == '#' || map[(size_t)r][(size_t)c] == '@')
                    set_cell(c, r, '.');
        update_player_sprite();
        player.fire_cd = 0;
        player.alive = true;
        fire_player();
        log("tank: drill base armed x=" + std::to_string((int)player.x) +
            " y=" + std::to_string((int)player.y) + " frame=" + std::to_string(frame));
    }

    void hook_power() {
        if (!player.alive) return;
        player.shield = 99.0F;
        spawn_powerup(hook_power_kind, player.x, player.y);
        log("tank: drill power kind=" + hook_power_kind + " frame=" + std::to_string(frame));
    }

    void hook_clear() {
        spawn_queue.clear();
        int n = 0;
        for (auto& e : enemies) {
            if (e.active) {
                kill_enemy(e, true);
                ++n;
            }
        }
        log("tank: drill clear kills=" + std::to_string(n) +
            " frame=" + std::to_string(frame));
        if (state == St::Play) start_stage_clear();
    }

    void update(double dt) override {
        const float fdt = (float)dt;
        ++frame;
        if (env_trace && frame % 60 == 0) {
            log("tank: trace frame=" + std::to_string(frame) + " score=" +
                std::to_string(score) + " lives=" + std::to_string(lives) + " stage=" +
                std::to_string(stage) + " enemies=" + std::to_string(active_enemies()) +
                " queue=" + std::to_string(spawn_queue.size()));
        }
        switch (state) {
            case St::Title:
                if (env_autoplay && frame > 20) new_game();
                break;
            case St::Loading:
                update_loading();
                break;
            case St::Intro:
                intro_timer -= fdt;
                update_explosions(fdt);
                update_spawn_fx(fdt);
                if (player.alive) update_player_sprite();
                if (intro_timer <= 0) begin_play();
                break;
            case St::Play:
                if (hook_brick_at > 0 && frame == hook_brick_at) { hook_brick(); hook_brick_at = 0; }
                if (hook_base_at > 0 && frame == hook_base_at) { hook_base(); hook_base_at = 0; }
                if (hook_power_at > 0 && frame == hook_power_at) { hook_power(); hook_power_at = 0; }
                if (hook_clear_at > 0 && frame == hook_clear_at) { hook_clear(); hook_clear_at = 0; }
                if (state == St::Play) {
                    update_player(fdt);
                    update_enemies(fdt);
                    update_bullets(fdt);
                    update_powerup(fdt);
                    update_explosions(fdt);
                    update_spawn_fx(fdt);
                    update_shovel(fdt);
                    update_spawns(fdt);
                    if (freeze_timer > 0) freeze_timer -= fdt;
                    update_water(fdt);
                }
                break;
            case St::Pause:
                break;
            case St::Clear:
                stage_clear_timer -= fdt;
                update_explosions(fdt);
                update_spawn_fx(fdt);
                update_bullets(fdt);
                if (stage_clear_timer <= 0) {
                    ++stage;
                    request_stage(stage);
                }
                break;
            case St::Over:
                over_timer += fdt;
                update_explosions(fdt);
                update_spawn_fx(fdt);
                update_bullets(fdt);
                if (over_timer > 1.2F) over_accept = true;
                if (env_autoplay && over_timer > 2.6F) new_game();
                break;
        }
        publish_hud();
    }

    static bool key_is_start(int k) {
        return k == 3 || k == 13 || k == 2 || k == 32 || k == 74 || k == 106;
    }

    void on_key_down(double /*target*/, double key) override {
        const int k = (int)key;
        if (state == St::Title) {
            if (key_is_start(k)) new_game();
            return;
        }
        if (state == St::Over) {
            if (over_accept && key_is_start(k)) return_to_title();
            return;
        }
        if (state == St::Pause) {
            if (k == 80 || k == 112 || k == 3 || k == 13 || k == 1 || k == 27) {
                state = St::Play;
                if (audio_on) host.audio_bus_volume("music", 0.17F);
                log("tank: resume frame=" + std::to_string(frame));
            }
            return;
        }
        if (state == St::Play) {
            if (k == 80 || k == 112) {
                state = St::Pause;
                if (audio_on) host.audio_bus_volume("music", 0.0F);
                sfx("pause", 0.4F);
                log("tank: pause frame=" + std::to_string(frame));
                return;
            }
            if (k == 1 || k == 27) { return_to_title(); return; }
            if (k == 2 || k == 32 || k == 74 || k == 106 || k == 90 || k == 122) fire_player();
        }
    }

    void on_pointer_down(double /*target*/, double /*x*/, double /*y*/) override {
        if (state == St::Title) new_game();
        else if (state == St::Over) { if (over_accept) return_to_title(); }
        else if (state == St::Pause) {
            state = St::Play;
            if (audio_on) host.audio_bus_volume("music", 0.17F);
        } else if (state == St::Play) {
            fire_player();
        }
    }
};

#ifdef MAQUE_NATIVE_STATIC
MAQUE_SCRIPT_STATIC(TankNative, tank_native_game)
#else
MAQUE_SCRIPT(TankNative)
#endif
