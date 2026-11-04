-- Battle City —— 1990 FC《坦克大战》规则的 Lua 复刻（根场景 game.lua）。
--
-- ┌─ 场景结构（samples/tank/）────────────────────────────────────────────┐
-- │ tank.scn                 根场景：背景 + 战场外框 + 三个嵌套实例      │
-- │   ├─ actors/actors.scn   纯数据：坦克/子弹/爆炸/道具实体池（prefab）  │
-- │   ├─ ui/hud.scn          独立 VM：HUD、标题画面、过场/结算文字        │
-- │   └─ (动态) stages/N.scn  纯数据：关卡地形 + 基地 + 出生点            │
-- │ game.lua                 只有玩法：地图/移动/命中/敌人/关卡/计分      │
-- └───────────────────────────────────────────────────────────────────────┘
--
-- 为什么这样拆（也是这个示例想立的标准）：
--   1. **单一真相**：关卡地形只在 `stages/N.scn` 里写一次。根脚本实例化它、遍历
--      一次子树，从节点名（tile_c09_r10_brick）建出碰撞网格；不在 Lua 里抄一张地图。
--   2. **纯数据用 prefab，有行为才用 .scn**：actor 池是可复用的 prefab 实例；
--      HUD 有自己的 VM，所以它是嵌套 `.scn`；stage 没有行为，所以只是数据场景。
--   3. **作用域**：根脚本（父实例）操作子实例的节点是显式允许的；HUD 子脚本
--      不反向抓根节点，只读共享 `global_*` 通道（docs/32 §16.3）。
--   4. **池化**：所有实体的 create 都发生在场景装配期；运行期只 set_active / 改纹理，
--      不 spawn/destroy。关卡切换只卸载/实例化 stage 场景。
--   5. **确定性**：固定 dt、引擎 rng()、无墙钟、无异步。同输入两次运行 world_trail 一致。
--
-- 操作：方向键/WASD 移动，空格/J 开火，P 暂停，Enter 开始/确认。
-- 测试/调试环境变量：
--   MAQUE_TANK_AUTOPLAY=1 / STAGE=N / GOD=1 / TRACE=1 / SKIP_TITLE=1
--   MAQUE_TANK_BRICK_AT=N / BASE_AT=N / POWER_AT=N（真实弹道/拾取路径）
--   MAQUE_TANK_BGM=0

local FX, FY = 16.0, 32.0
local CELL = 16.0
local COLS, ROWS = 26, 26
local FW, FH = COLS * CELL, ROWS * CELL
local FIELD_TOP = FY + FH
local FIELD_RIGHT = FX + FW
local STAGE_COUNT = 4
local ENEMIES_PER_STAGE = 20

local TANK_HALF = 14.0
local TANK_SPEED = 84.0
local PLAYER_BULLET_SPEED = 190.0
local PLAYER_FIRE_CD = 0.30
local ENEMY_FIRE_CD_MIN, ENEMY_FIRE_CD_MAX = 0.55, 1.30
local PLAYER_SHIELD_TIME = 3.0
local RESPAWN_TIME = 1.35
local POWERUP_TTL = 15.0
local MAX_ACTIVE_ENEMIES = 4

local S_TITLE, S_LOADING, S_INTRO, S_PLAY, S_PAUSE, S_CLEAR, S_OVER =
    "title", "loading", "intro", "play", "pause", "clear", "gameover"

local DIRS = {
    u = { dx = 0, dy = 1 },
    r = { dx = 1, dy = 0 },
    d = { dx = 0, dy = -1 },
    l = { dx = -1, dy = 0 },
}
local DIR_ORDER = { "u", "r", "d", "l" }

local ENEMY_DEFS = {
    basic = { speed = 36.0, bullet = 145.0, hp = 1, score = 100 },
    fast = { speed = 74.0, bullet = 175.0, hp = 1, score = 200 },
    power = { speed = 38.0, bullet = 245.0, hp = 1, score = 300 },
    armor = { speed = 34.0, bullet = 170.0, hp = 4, score = 400 },
}
local POWER_KINDS = { "star", "helmet", "grenade", "shovel", "tank", "timer" }
local POWER_TEX = {
    star = "/art/power_star.png", helmet = "/art/power_helmet.png",
    grenade = "/art/power_grenade.png", shovel = "/art/power_shovel.png",
    tank = "/art/power_tank.png", timer = "/art/power_timer.png",
}
local TILE_CHAR = { brick = "#", steel = "@", water = "~", tree = "T", ice = "I" }
local TILE_TEX = {
    ["#"] = "/art/tile_brick.png", ["@"] = "/art/tile_steel.png",
    ["~"] = "/art/tile_water0.png", ["T"] = "/art/tile_tree.png",
    ["I"] = "/art/tile_ice.png",
}
local TILE_Z = { ["T"] = 40, ["I"] = 5, ["#"] = 10, ["@"] = 10, ["~"] = 10 }

-- ============================ 状态 ============================

local state = S_TITLE
local frame = 0
local stage = 1
local pending_stage = 1
local score = 0
local hi_score = 0
local lives = 2
local player_level = 0
local next_extra = 20000

local player = nil
local enemies = {}
local bullets = {}
local explosions = {}
local spawn_fx = {}
local powerup = { active = false, id = -1, kind = "star", x = 0, y = 0, t = 0 }
local base_spr = -1
local base_alive = true
local base_x, base_y = 224.0, 48.0
local spawn_points = {}
local player_start_x, player_start_y = 160.0, 48.0
local spawn_queue = {}
local spawn_timer = 0.0
local stage_clear_timer = 0.0
local intro_timer = 0.0
local over_timer = 0.0
local over_accept = false
local freeze_timer = 0.0
local shovel_timer = 0.0
local shovel_ring = {}
local water_anim_t = 0.0
local water_anim_frame = 0
local water_cells = {}

local map = {}
local cell_node = {}
local stage_inst = -1
local stage_root = -1
local stage_host = -1
local want_stage = false
local stage_instantiate_tried = false

local bg = -1
local field_bg = -1
local field_frame = -1
local muzzle = -1
local tex_of = {}

local audio_on = false
local bgm_voice = 0
local bgm_on = true

local env_autoplay = false
local env_god = false
local env_trace = false
local env_skip_title = false
local hook_brick_at = 0
local hook_base_at = 0
local hook_power_at = 0
local hook_power_kind = "star"
local hook_clear_at = 0
local stage_override = 0

local hud_num_cache = {}
local hud_text_cache = {}

-- 前向声明

-- ============================ 小工具 ============================

function clamp(v, lo, hi)
    if v < lo then return lo end
    if v > hi then return hi end
    return v
end

function dist2(ax, ay, bx, by)
    local dx, dy = ax - bx, ay - by
    return dx * dx + dy * dy
end

function pad_num(n, width)
    local s = tostring(math.floor(n + 0.5))
    while #s < width do s = "0" .. s end
    return s
end

function snap(v, base, step)
    return base + math.floor((v - base) / step + 0.5) * step
end

function cell_center(c, r)
    return FX + (c - 1) * CELL + CELL * 0.5,
           FY + (ROWS - r) * CELL + CELL * 0.5
end

function cell_of_world(x, y)
    local c = math.floor((x - FX) / CELL) + 1
    local r = ROWS - math.floor((y - FY) / CELL)
    return clamp(c, 1, COLS), clamp(r, 1, ROWS)
end

function tank_cell(x, y)
    local c = math.floor((x - CELL - FX) / CELL) + 1
    local bottom = math.floor((y - CELL - FY) / CELL)
    local r = ROWS - bottom - 1
    return clamp(c, 1, COLS - 1), clamp(r, 1, ROWS - 1)
end

function is_block_tank(ch)
    return ch == "#" or ch == "@" or ch == "~" or ch == "B"
end

function is_block_bullet(ch)
    return ch == "#" or ch == "@" or ch == "B"
end

function count_active_enemies()
    local n = 0
    for i = 1, #enemies do
        if enemies[i].active then n = n + 1 end
    end
    return n
end

function total_remaining_enemies()
    return #spawn_queue + count_active_enemies()
end

function any_enemy_bullet(owner)
    for i = 1, #bullets do
        local b = bullets[i]
        if b.active and b.team == 1 and b.owner == owner then return true end
    end
    return false
end

function player_bullet_count()
    local n = 0
    for i = 1, #bullets do
        local b = bullets[i]
        if b.active and b.team == 0 then n = n + 1 end
    end
    return n
end

function add_score(n)
    score = score + n
    if score > hi_score then hi_score = score end
    while score >= next_extra do
        lives = lives + 1
        next_extra = next_extra + 20000
        sfx("powerup", 0.55)
        log("tank: extra life score=" .. score .. " lives=" .. lives .. " frame=" .. frame)
    end
end

function set_tex(id, path)
    if id < 0 then return end
    if tex_of[id] ~= path then
        node_set_string(id, "scn.Sprite", "texture", path)
        tex_of[id] = path
    end
end

function set_rgb(id, r, g, b)
    node_set_number(id, "scn.Sprite", "r", r)
    node_set_number(id, "scn.Sprite", "g", g)
    node_set_number(id, "scn.Sprite", "b", b)
end

function park(id)
    if id >= 0 then node_set_active(id, 0) end
end

function unpark(id)
    if id >= 0 then node_set_active(id, 1) end
end

function place(id, x, y)
    if id >= 0 then node_set_position(id, x, y) end
end

-- ============================ 音频 ============================

function sfx(name, vol)
    if not audio_on then return end
    audio_play("audio/" .. name .. ".wav", vol or 0.5, 0, "sfx")
end

function start_bgm()
    if not (audio_on and bgm_on) then return end
    if bgm_voice ~= 0 then return end
    bgm_voice = audio_play("audio/battle_loop.wav", 0.17, 1, "music")
end

function stop_bgm()
    if not audio_on then return end
    if bgm_voice ~= 0 then
        audio_stop(bgm_voice)
        bgm_voice = 0
    end
end

-- ============================ 地图（从 stage.scn 派生） ============================

function new_grid()
    map = {}
    cell_node = {}
    for r = 1, ROWS do
        map[r] = {}
        cell_node[r] = {}
        for c = 1, COLS do map[r][c] = "." end
    end
end

function cell_at(c, r)
    if c < 1 or c > COLS or r < 1 or r > ROWS then return "@" end
    return map[r][c]
end

function set_cell(c, r, ch)
    if c < 1 or c > COLS or r < 1 or r > ROWS then return end
    map[r][c] = ch
    local id = cell_node[r] and cell_node[r][c]
    if not id or id < 0 then
        -- 生成器保证基地周围 16 格一定有砖块节点；这里只保护异常资产。
        if ch ~= "." then
            log("tank: warning missing tile node c=" .. c .. " r=" .. r .. " type=" .. ch)
        end
        return
    end
    if ch == "." or ch == "B" then
        park(id)
        return
    end
    unpark(id)
    node_set_z_index(id, TILE_Z[ch] or 10)
    set_tex(id, TILE_TEX[ch])
end

function update_water(dt)
    if #water_cells == 0 then return end
    water_anim_t = water_anim_t + dt
    if water_anim_t < 0.45 then return end
    water_anim_t = 0
    water_anim_frame = 1 - water_anim_frame
    local path = water_anim_frame == 0 and "/art/tile_water0.png" or "/art/tile_water1.png"
    for i = 1, #water_cells do
        local p = water_cells[i]
        if map[p.r][p.c] == "~" then set_tex(cell_node[p.r][p.c], path) end
    end
end

function tank_map_collides(x, y)
    if x < FX + CELL or x > FIELD_RIGHT - CELL or
       y < FY + CELL or y > FIELD_TOP - CELL then
        return true
    end
    local c0 = math.floor((x - TANK_HALF - FX) / CELL) + 1
    local c1 = math.floor((x + TANK_HALF - FX) / CELL) + 1
    local j0 = math.floor((y - TANK_HALF - FY) / CELL)
    local j1 = math.floor((y + TANK_HALF - FY) / CELL)
    for j = j0, j1 do
        local r = ROWS - j
        for c = c0, c1 do
            if is_block_tank(cell_at(c, r)) then return true end
        end
    end
    return false
end

function tanks_overlap(ax, ay, bx, by)
    return math.abs(ax - bx) < TANK_HALF * 2 and math.abs(ay - by) < TANK_HALF * 2
end

function rect_hits(x, y, ox, oy, half)
    return math.abs(x - ox) < half and math.abs(y - oy) < half
end

-- ============================ 场景加载（动态 stage 实例） ============================

function walk_stage(id, fn)
    local name = entity_name(id)
    fn(id, name)
    local n = node_child_count(id)
    for i = 0, n - 1 do
        walk_stage(node_child_at(id, i), fn)
    end
end

function collect_stage()
    if stage_host < 0 then stage_host = entity_find("StageHost") end
    if stage_host < 0 then return false end
    local n = node_child_count(stage_host)
    if n <= 0 then return false end
    stage_root = node_child_at(stage_host, 0)
    if stage_root < 0 then return false end

    new_grid()
    spawn_points = {}
    base_spr = -1
    water_cells = {}
    local tile_count = 0

    walk_stage(stage_root, function(id, name)
        local c, r, t = string.match(name, "^tile_c(%d+)_r(%d+)_(%a+)$")
        if c and r and t then
            c, r = tonumber(c), tonumber(r)
            local ch = TILE_CHAR[t]
            if ch and c >= 1 and c <= COLS and r >= 1 and r <= ROWS then
                map[r][c] = ch
                cell_node[r][c] = id
                unpark(id)
                node_set_z_index(id, TILE_Z[ch] or 10)
                set_tex(id, TILE_TEX[ch])
                if ch == "~" then water_cells[#water_cells + 1] = { c = c, r = r } end
                tile_count = tile_count + 1
            end
        elseif name == "base_eagle" then
            base_spr = id
            base_x, base_y = entity_x(id), entity_y(id)
            node_set_z_index(id, 12)
            set_tex(id, "/art/base_eagle.png")
        elseif name == "player_start" then
            player_start_x, player_start_y = entity_x(id), entity_y(id)
        elseif string.match(name, "^spawn_%d+$") then
            spawn_points[#spawn_points + 1] = { x = entity_x(id), y = entity_y(id) }
        end
    end)

    -- 基地占 2x2 单元；场景里不画 tile，由 base_eagle 精灵表达。
    local bc = math.floor((base_x - 8 - FX) / CELL) + 1
    local br = ROWS - math.floor((base_y - 8 - FY) / CELL)
    for dr = 0, 1 do
        for dc = 0, 1 do
            map[br - dr][bc + dc] = "B"
            if cell_node[br - dr][bc + dc] then
                park(cell_node[br - dr][bc + dc])
                cell_node[br - dr][bc + dc] = nil
            end
        end
    end
    table.sort(spawn_points, function(a, b)
        if a.x ~= b.x then return a.x < b.x end
        return a.y < b.y
    end)

    base_alive = true
    if base_spr >= 0 then
        set_tex(base_spr, "/art/base_eagle.png")
        node_set_active(base_spr, 1)
    end
    if stage_root >= 0 then node_set_active(stage_root, 1) end
    log("tank: stage scene ready stage=" .. pending_stage .. " tiles=" .. tile_count ..
        " spawns=" .. #spawn_points .. " base=(" .. math.floor(base_x) .. "," ..
        math.floor(base_y) .. ") frame=" .. frame)
    return base_spr >= 0 and #spawn_points > 0
end

function reset_actor_pools()
    for i = 1, #enemies do
        enemies[i].active = false
        enemies[i].spawning = false
        enemies[i].dead = true
        park(enemies[i].id)
    end
    for i = 1, #bullets do
        bullets[i].active = false
        park(bullets[i].id)
    end
    for i = 1, #explosions do
        explosions[i].active = false
        park(explosions[i].id)
    end
    for i = 1, #spawn_fx do
        spawn_fx[i].active = false
        park(spawn_fx[i].id)
    end
    if powerup.active then park(powerup.id) end
    powerup.active = false
    park(muzzle)
    freeze_timer = 0
    shovel_timer = 0
    shovel_ring = {}
end

function activate_stage(st)
    stage = st
    pending_stage = st
    if not collect_stage() then
        log("tank: stage load failed stage=" .. st .. " frame=" .. frame)
        return false
    end
    reset_actor_pools()
    spawn_queue = make_enemy_queue(st)
    spawn_timer = 0
    freeze_timer = 0
    shovel_timer = 0
    player.level = player_level
    player.shield = 0
    player.alive = false
    player.respawn = 0
    spawn_player()
    intro_timer = 1.6
    state = S_INTRO
    start_bgm()
    log("tank: stage " .. stage .. " start enemies=" .. #spawn_queue ..
        " lives=" .. lives .. " level=" .. player.level .. " frame=" .. frame)
    return true
end

function request_stage(st)
    stage = st
    pending_stage = st
    want_stage = true
    stage_instantiate_tried = false
    state = S_LOADING
    spawn_queue = {}
    reset_actor_pools()
    if player then
        player.alive = false
        park(player.id)
        park(player.shield_id)
    end
    if stage_root >= 0 then node_set_active(stage_root, 0) end
    if stage_inst >= 0 then
        scene_unload(stage_inst)
        stage_inst = -1
        stage_root = -1
    end
    new_grid()
    base_spr = -1
    if field_bg >= 0 then node_set_active(field_bg, 1) end
    if field_frame >= 0 then node_set_active(field_frame, 1) end
    start_bgm()
    log("tank: stage " .. st .. " loading scene=" .. stage_path(st) .. " frame=" .. frame)
end

function update_loading()
    if not want_stage then return end
    if stage_host < 0 then stage_host = entity_find("StageHost") end
    if stage_host < 0 then return end
    local kids = node_child_count(stage_host)
    if stage_inst < 0 then
        if kids > 0 then return end                  -- 旧关卡还在等帧边界卸载
        if stage_instantiate_tried then return end
        stage_instantiate_tried = true
        stage_inst = scene_instantiate(stage_host, stage_path(pending_stage), "{}")
        log("tank: stage instance request handle=" .. stage_inst .. " frame=" .. frame)
        if stage_inst < 0 then
            log("tank: stage instance failed path=" .. stage_path(pending_stage))
            trigger_game_over("stage_load")
        end
        return
    end
    if kids > 0 then
        if activate_stage(pending_stage) then
            -- state 已切 INTRO；下一帧开始演出。
        else
            trigger_game_over("stage_load")
        end
    end
end

function stage_path(st)
    local idx = (st - 1) % STAGE_COUNT + 1
    return "stages/stage" .. idx .. ".scn"
end

-- ============================ 特效 ============================

function spawn_explosion(x, y, scale)
    for i = 1, #explosions do
        local e = explosions[i]
        if not e.active then
            e.active = true
            e.x, e.y = x, y
            e.t = 0
            e.scale = scale or 1.0
            place(e.id, x, y)
            node_set_scale(e.id, e.scale)
            node_set_opacity(e.id, 1.0)
            unpark(e.id)
            set_tex(e.id, "/art/explosion0.png")
            return
        end
    end
end

function update_explosions(dt)
    for i = 1, #explosions do
        local e = explosions[i]
        if e.active then
            e.t = e.t + dt
            local f = math.floor(e.t / 0.055)
            if f >= 5 then
                e.active = false
                park(e.id)
            else
                set_tex(e.id, "/art/explosion" .. f .. ".png")
                node_set_scale(e.id, e.scale * (1.0 + f * 0.16))
            end
        end
    end
end

function show_spawn_fx(i, x, y)
    local s = spawn_fx[i]
    if not s then return end
    s.active = true
    s.t = 0
    place(s.id, x, y)
    node_set_opacity(s.id, 1.0)
    unpark(s.id)
    set_tex(s.id, "/art/spawn0.png")
end

function update_spawn_fx(dt)
    for i = 1, #spawn_fx do
        local s = spawn_fx[i]
        if s.active then
            s.t = s.t + dt
            local f = math.floor(s.t / 0.10)
            if f >= 4 then
                s.active = false
                park(s.id)
            else
                set_tex(s.id, "/art/spawn" .. f .. ".png")
            end
        end
    end
end

-- ============================ 基地 ============================

function base_center()
    return base_x, base_y
end

function destroy_base(x, y)
    if not base_alive then return end
    base_alive = false
    if base_spr >= 0 then set_tex(base_spr, "/art/base_dead.png") end
    spawn_explosion(base_x, base_y, 1.7)
    spawn_explosion(base_x - 12, base_y + 8, 1.0)
    sfx("explode", 0.85)
    for r = 1, ROWS do
        for c = 1, COLS do
            if map[r][c] == "B" then
                map[r][c] = "."
            end
        end
    end
    log("tank: base destroyed x=" .. math.floor(x) .. " y=" .. math.floor(y) ..
        " frame=" .. frame)
    trigger_game_over("base")
end

-- ============================ 子弹 ============================

function bullet_kill(b, spark)
    if not b.active then return end
    b.active = false
    park(b.id)
    if spark then spawn_explosion(b.x, b.y, 0.45) end
end

function bullet_spawn(team, owner, x, y, dir, speed, power)
    local v = DIRS[dir]
    local b = nil
    for i = 1, #bullets do
        if not bullets[i].active then b = bullets[i]; break end
    end
    if not b then return nil end
    b.active = true
    b.team = team
    b.owner = owner
    b.x, b.y = x, y
    b.vx, b.vy = v.dx, v.dy
    b.speed = speed
    b.power = power or false
    b.life = 0.0
    place(b.id, x, y)
    set_tex(b.id, "/art/bullet.png")
    unpark(b.id)
    if team == 1 then sfx("fire", 0.25) end
    return b
end

function bullet_hit_tank(b)
    if b.team == 0 then
        for i = 1, #enemies do
            local e = enemies[i]
            if e.active and not e.spawning and e.invuln <= 0 then
                if rect_hits(b.x, b.y, e.x, e.y, TANK_HALF + 5) then
                    damage_enemy(e, 1, b.x, b.y)
                    bullet_kill(b, true)
                    return true
                end
            end
        end
    else
        if player and player.alive and rect_hits(b.x, b.y, player.x, player.y, TANK_HALF + 5) then
            if player.shield > 0 or env_god then
                spawn_explosion(b.x, b.y, 0.5)
                bullet_kill(b, false)
            else
                damage_player()
                bullet_kill(b, true)
            end
            return true
        end
    end
    return false
end

function bullet_check_cell(b)
    if b.x < FX or b.x > FIELD_RIGHT or b.y < FY or b.y > FIELD_TOP then
        bullet_kill(b, true)
        return true
    end
    local c, r = cell_of_world(b.x, b.y)
    local ch = map[r][c]
    if ch == "#" then
        set_cell(c, r, ".")
        sfx("brick", 0.45)
        spawn_explosion(b.x, b.y, 0.55)
        bullet_kill(b, false)
        log("tank: brick broke c=" .. c .. " r=" .. r .. " frame=" .. frame)
        return true
    elseif ch == "@" then
        if b.power then
            set_cell(c, r, ".")
            sfx("brick", 0.55)
            spawn_explosion(b.x, b.y, 0.75)
            bullet_kill(b, false)
            log("tank: steel broke c=" .. c .. " r=" .. r .. " frame=" .. frame)
        else
            sfx("steel", 0.32)
            spawn_explosion(b.x, b.y, 0.35)
            bullet_kill(b, false)
            log("tank: steel hit c=" .. c .. " r=" .. r .. " frame=" .. frame)
        end
        return true
    elseif ch == "B" then
        bullet_kill(b, false)
        destroy_base(b.x, b.y)
        return true
    end
    return false
end

function update_bullets(dt)
    for i = 1, #bullets do
        local b = bullets[i]
        if b.active then
            local travel = b.speed * dt
            local steps = math.ceil(travel / 5.0)
            if steps < 1 then steps = 1 end
            local dead = false
            for _ = 1, steps do
                b.x = b.x + b.vx * travel / steps
                b.y = b.y + b.vy * travel / steps
                if bullet_check_cell(b) then dead = true; break end
                if bullet_hit_tank(b) then dead = true; break end
                if b.team == 0 then
                    for j = 1, #bullets do
                        local o = bullets[j]
                        if o.active and o.team == 1 and
                           math.abs(o.x - b.x) < 7.0 and math.abs(o.y - b.y) < 7.0 then
                            bullet_kill(o, true)
                            bullet_kill(b, false)
                            dead = true
                            break
                        end
                    end
                end
                if dead then break end
            end
            if not dead then
                place(b.id, b.x, b.y)
                b.life = b.life + dt
                if b.life > 3.0 then bullet_kill(b, false) end
            end
        end
    end
end

-- ============================ 坦克 ============================

function player_texture()
    return "/art/tank_player_l" .. clamp(player.level, 0, 3) .. "_" .. player.dir .. ".png"
end

function update_player_sprite()
    if not player or not player.alive then return end
    set_tex(player.id, player_texture())
    place(player.id, player.x, player.y)
    if player.shield > 0 then
        unpark(player.shield_id)
        place(player.shield_id, player.x, player.y)
        set_tex(player.shield_id, "/art/shield" .. (math.floor(frame / 8) % 2) .. ".png")
    else
        park(player.shield_id)
    end
    if player.muzzle_t > 0 then
        local v = DIRS[player.dir]
        unpark(muzzle)
        place(muzzle, player.x + v.dx * 22, player.y + v.dy * 22)
        node_set_opacity(muzzle, clamp(player.muzzle_t / 0.06, 0.0, 1.0))
    else
        park(muzzle)
    end
end

function enemy_texture(e)
    if e.kind == "armor" then
        return "/art/tank_armor_hp" .. clamp(e.hp, 1, 4) .. "_" .. e.dir .. ".png"
    end
    return "/art/tank_" .. e.kind .. "_" .. e.dir .. ".png"
end

function update_enemy_sprite(e)
    if not e.active or e.spawning then return end
    set_tex(e.id, enemy_texture(e))
    place(e.id, e.x, e.y)
    if e.flash > 0 then
        set_rgb(e.id, 2.6, 2.6, 2.6)
        node_set_opacity(e.id, 1.0)
        return
    end
    if freeze_timer > 0 then
        set_rgb(e.id, 0.55, 0.78, 1.25)
        node_set_opacity(e.id, 0.70)
        return
    end
    local k = 1.0
    if e.carrier and (math.floor(frame / 8) % 2 == 0) then k = 1.9 end
    set_rgb(e.id, k, k, k)
    node_set_opacity(e.id, 1.0)
end

function player_can_move(x, y)
    if tank_map_collides(x, y) then return false end
    for i = 1, #enemies do
        local e = enemies[i]
        if e.active and not e.spawning and tanks_overlap(x, y, e.x, e.y) then return false end
    end
    return true
end

function enemy_can_move(e, x, y)
    if tank_map_collides(x, y) then return false end
    if player and player.alive and tanks_overlap(x, y, player.x, player.y) then return false end
    for i = 1, #enemies do
        local o = enemies[i]
        if o ~= e and o.active and not o.spawning and tanks_overlap(x, y, o.x, o.y) then
            return false
        end
    end
    return true
end

function set_player_dir(dir)
    if player.dir == dir then return end
    local ox, oy = player.x, player.y
    if dir == "u" or dir == "d" then
        player.x = snap(player.x, FX + CELL, CELL)
    else
        player.y = snap(player.y, FY + CELL, CELL)
    end
    if not player_can_move(player.x, player.y) then
        player.x, player.y = ox, oy
    end
    player.dir = dir
    update_player_sprite()
end

function player_on_ice()
    local c, r = cell_of_world(player.x, player.y)
    return map[r] and map[r][c] == "I"
end

function move_player(dir, dt)
    if player.dir ~= dir then set_player_dir(dir) end
    local v = DIRS[player.dir]
    local speed = TANK_SPEED
    local on_ice = player_on_ice()
    if on_ice then speed = speed * 1.28 end
    local nx = player.x + v.dx * speed * dt
    local ny = player.y + v.dy * speed * dt
    if player_can_move(nx, ny) then
        player.x, player.y = nx, ny
        player.moved = true
        if on_ice then player.slide = 0.12 end
    else
        player.moved = false
    end
    update_player_sprite()
end

function fire_player()
    if not player or not player.alive or state ~= S_PLAY then return false end
    if player.fire_cd > 0 then return false end
    local max_b = player.level >= 2 and 2 or 1
    if player_bullet_count() >= max_b then return false end
    local v = DIRS[player.dir]
    local speed = PLAYER_BULLET_SPEED + (player.level >= 1 and 45.0 or 0.0)
    local b = bullet_spawn(0, player, player.x + v.dx * 20.0, player.y + v.dy * 20.0,
                           player.dir, speed, player.level >= 3)
    if not b then return false end
    player.fire_cd = PLAYER_FIRE_CD - (player.level >= 1 and 0.06 or 0.0)
    player.muzzle_t = 0.06
    sfx("fire", 0.38)
    log("tank: player fire dir=" .. player.dir .. " level=" .. player.level ..
        " frame=" .. frame)
    return true
end

function spawn_player()
    player.alive = true
    player.x, player.y = player_start_x, player_start_y
    player.dir = "u"
    player.level = player_level
    player.shield = PLAYER_SHIELD_TIME
    player.fire_cd = 0
    player.muzzle_t = 0
    player.respawn = 0
    player.slide = 0
    player.flash = 0
    unpark(player.id)
    update_player_sprite()
    log("tank: player spawn x=" .. math.floor(player.x) .. " y=" .. math.floor(player.y) ..
        " level=" .. player.level .. " shield=" .. player.shield .. " frame=" .. frame)
end

function damage_player()
    if not player or not player.alive or env_god then return end
    player.alive = false
    park(player.id)
    park(player.shield_id)
    park(muzzle)
    spawn_explosion(player.x, player.y, 1.35)
    sfx("player_hit", 0.85)
    if lives > 0 then
        lives = lives - 1
        player.respawn = RESPAWN_TIME
        log("tank: player dead hp=0 lives=" .. lives .. " frame=" .. frame)
    else
        lives = 0
        log("tank: player dead hp=0 lives=0 frame=" .. frame)
        trigger_game_over("player")
    end
end

function update_player(dt)
    if not player then return end
    if not player.alive then
        if player.respawn > 0 then
            player.respawn = player.respawn - dt
            if player.respawn <= 0 then
                player.respawn = 0
                player_level = 0
                spawn_player()
            end
        end
        return
    end

    if player.fire_cd > 0 then player.fire_cd = player.fire_cd - dt end
    if player.muzzle_t > 0 then player.muzzle_t = player.muzzle_t - dt end
    if player.shield > 0 then player.shield = player.shield - dt end

    local dir = nil
    if env_autoplay then
        dir = bot_dir()
    else
        if input_key(8) ~= 0 or input_key(87) ~= 0 or input_key(119) ~= 0 then dir = "u"
        elseif input_key(9) ~= 0 or input_key(83) ~= 0 or input_key(115) ~= 0 then dir = "d"
        elseif input_key(6) ~= 0 or input_key(65) ~= 0 or input_key(97) ~= 0 then dir = "l"
        elseif input_key(7) ~= 0 or input_key(68) ~= 0 or input_key(100) ~= 0 then dir = "r"
        end
    end

    if dir then
        move_player(dir, dt)
        if env_autoplay and bot_want_fire() then fire_player() end
    elseif player_on_ice() and player.slide > 0 then
        player.slide = player.slide - dt
        local v = DIRS[player.dir]
        local nx = player.x + v.dx * TANK_SPEED * 0.72 * dt
        local ny = player.y + v.dy * TANK_SPEED * 0.72 * dt
        if player_can_move(nx, ny) then
            player.x, player.y = nx, ny
        end
    else
        player.slide = 0
    end

    if not env_autoplay then
        local firing = input_key(2) ~= 0 or input_key(32) ~= 0 or
                       input_key(74) ~= 0 or input_key(106) ~= 0 or
                       input_key(90) ~= 0 or input_key(122) ~= 0 or
                       input_down() ~= 0
        if firing then fire_player() end
    end

    update_player_sprite()
end

-- ============================ 敌人 AI ============================

function aligned_shot(x1, y1, x2, y2, dir, max_dist)
    if dir == "u" then
        return math.abs(x1 - x2) < 12 and y2 > y1 and (y2 - y1) < max_dist
    elseif dir == "d" then
        return math.abs(x1 - x2) < 12 and y2 < y1 and (y1 - y2) < max_dist
    elseif dir == "r" then
        return math.abs(y1 - y2) < 12 and x2 > x1 and (x2 - x1) < max_dist
    elseif dir == "l" then
        return math.abs(y1 - y2) < 12 and x2 < x1 and (x1 - x2) < max_dist
    end
    return false
end

function enemy_fire(e)
    if any_enemy_bullet(e) then return end
    local v = DIRS[e.dir]
    local b = bullet_spawn(1, e, e.x + v.dx * 20.0, e.y + v.dy * 20.0,
                           e.dir, ENEMY_DEFS[e.kind].bullet, false)
    if b then
        e.fire_cd = ENEMY_FIRE_CD_MIN + rng() * (ENEMY_FIRE_CD_MAX - ENEMY_FIRE_CD_MIN)
    end
end

function enemy_dir_valid(e, dir)
    local v = DIRS[dir]
    local ox, oy = e.x, e.y
    if dir == "u" or dir == "d" then
        e.x = snap(e.x, FX + CELL, CELL)
    else
        e.y = snap(e.y, FY + CELL, CELL)
    end
    local ok = enemy_can_move(e, e.x + v.dx * 4.0, e.y + v.dy * 4.0)
    e.x, e.y = ox, oy
    return ok
end

function set_enemy_dir(e, dir)
    if e.dir == dir then return end
    local ox, oy = e.x, e.y
    if dir == "u" or dir == "d" then
        e.x = snap(e.x, FX + CELL, CELL)
    else
        e.y = snap(e.y, FY + CELL, CELL)
    end
    if not enemy_can_move(e, e.x, e.y) then
        e.x, e.y = ox, oy
    end
    e.dir = dir
    update_enemy_sprite(e)
end

function choose_enemy_dir(e)
    local candidates = {}
    for i = 1, 4 do
        local d = DIR_ORDER[i]
        if enemy_dir_valid(e, d) then candidates[#candidates + 1] = d end
    end
    if #candidates == 0 then return end

    if player and player.alive then
        local dx, dy = player.x - e.x, player.y - e.y
        if math.abs(dx) < 10 then
            local want = dy > 0 and "u" or "d"
            if enemy_dir_valid(e, want) then set_enemy_dir(e, want); return end
        elseif math.abs(dy) < 10 then
            local want = dx > 0 and "r" or "l"
            if enemy_dir_valid(e, want) then set_enemy_dir(e, want); return end
        end
    end

    local bx, by = base_center()
    local best, best_w = candidates[1], -1e9
    for i = 1, #candidates do
        local d = candidates[i]
        local w = rng() * 1.5
        if d == "d" and e.y > by then w = w + 3.0 end
        if player and player.alive then
            local pdx, pdy = player.x - e.x, player.y - e.y
            if math.abs(pdx) > math.abs(pdy) then
                if d == (pdx > 0 and "r" or "l") then w = w + 1.6 end
            else
                if d == (pdy > 0 and "u" or "d") then w = w + 1.6 end
            end
        end
        if d == e.dir then w = w + 1.1 end
        local back = e.dir == "u" and "d" or e.dir == "d" and "u" or
                     e.dir == "l" and "r" or "l"
        if d == back then w = w - 1.3 end
        if w > best_w then best_w, best = w, d end
    end
    set_enemy_dir(e, best)
end

function spawn_enemy(spec)
    local e = nil
    for i = 1, #enemies do
        if not enemies[i].active then e = enemies[i]; break end
    end
    if not e then return false end

    local chosen = nil
    local start = (e.idx + stage) % math.max(1, #spawn_points)
    for k = 0, #spawn_points - 1 do
        local p = spawn_points[((start + k) % #spawn_points) + 1]
        local occupied = false
        if player and player.alive and tanks_overlap(p.x, p.y, player.x, player.y) then
            occupied = true
        end
        for i = 1, #enemies do
            local o = enemies[i]
            if o.active and tanks_overlap(p.x, p.y, o.x, o.y) then occupied = true end
        end
        if not occupied then chosen = p; break end
    end
    if not chosen then return false end

    local def = ENEMY_DEFS[spec.kind]
    e.active = true
    e.spawning = true
    e.spawn_t = 0.80
    e.invuln = 0.80
    e.x, e.y = chosen.x, chosen.y
    e.dir = "d"
    e.kind = spec.kind
    e.hp = def.hp
    e.max_hp = def.hp
    e.speed = def.speed * (1.0 + math.max(0, stage - 4) * 0.03)
    e.carrier = spec.carrier
    e.power_kind = spec.power_kind
    e.fire_cd = 0.45 + rng() * 0.55
    e.think = 0.15
    e.flash = 0
    e.invuln = 0.80
    e.stuck = 0
    place(e.id, e.x, e.y)
    set_tex(e.id, enemy_texture(e))
    node_set_opacity(e.id, 1.0)
    set_rgb(e.id, 1, 1, 1)
    park(e.id)
    show_spawn_fx(e.idx, e.x, e.y)
    log("tank: enemy spawn idx=" .. e.idx .. " kind=" .. e.kind ..
        " carrier=" .. (e.carrier and 1 or 0) ..
        " x=" .. math.floor(e.x) .. " y=" .. math.floor(e.y) ..
        " remain=" .. (#spawn_queue - 1 + count_active_enemies() + 1) ..
        " frame=" .. frame)
    return true
end

function update_enemies(dt)
    for i = 1, #enemies do
        local e = enemies[i]
        if e.active then
            if e.spawning then
                e.spawn_t = e.spawn_t - dt
                if e.spawn_t <= 0 then
                    e.spawning = false
                    e.spawn_t = 0
                    e.invuln = 1.0
                    unpark(e.id)
                    update_enemy_sprite(e)
                    log("tank: enemy enter idx=" .. e.idx .. " frame=" .. frame)
                end
            else
                if e.invuln > 0 then e.invuln = e.invuln - dt end
                if e.flash > 0 then e.flash = e.flash - dt end
                if e.fire_cd > 0 then e.fire_cd = e.fire_cd - dt end

                if freeze_timer <= 0 then
                    e.think = e.think - dt
                    if e.think <= 0 then
                        choose_enemy_dir(e)
                        e.think = 0.30 + rng() * 0.75
                    end
                    local v = DIRS[e.dir]
                    local nx = e.x + v.dx * e.speed * dt
                    local ny = e.y + v.dy * e.speed * dt
                    if enemy_can_move(e, nx, ny) then
                        e.x, e.y = nx, ny
                        e.stuck = 0
                    else
                        e.stuck = (e.stuck or 0) + dt
                        if e.stuck > 0.05 then
                            choose_enemy_dir(e)
                            e.think = 0.20 + rng() * 0.35
                            e.stuck = 0
                        end
                    end
                    if e.fire_cd <= 0 and not any_enemy_bullet(e) then
                        local want_fire = false
                        if player and player.alive and
                           aligned_shot(e.x, e.y, player.x, player.y, e.dir, 290) then
                            want_fire = true
                        end
                        local bx, by = base_center()
                        if aligned_shot(e.x, e.y, bx, by, e.dir, 420) then want_fire = true end
                        if rng() < 0.18 then want_fire = true end
                        if want_fire then enemy_fire(e) end
                    end
                end
                update_enemy_sprite(e)
            end
        end
    end
end

function damage_enemy(e, dmg, hx, hy)
    if not e.active or e.spawning or e.invuln > 0 then return end
    e.hp = e.hp - dmg
    e.flash = 0.12
    spawn_explosion(hx or e.x, hy or e.y, 0.55)
    sfx("hit", 0.4)
    log("tank: hit enemy=" .. e.idx .. " kind=" .. e.kind ..
        " hp=" .. e.hp .. " frame=" .. frame)
    if e.hp <= 0 then
        kill_enemy(e)
    else
        update_enemy_sprite(e)
    end
end

function kill_enemy(e, silent)
    if not e.active then return end
    e.active = false
    e.dead = true
    park(e.id)
    spawn_explosion(e.x, e.y, 1.25)
    sfx("explode", 0.62)
    local def = ENEMY_DEFS[e.kind]
    add_score(def.score)
    log("tank: kill idx=" .. e.idx .. " kind=" .. e.kind .. " score=" .. def.score ..
        " total_score=" .. score .. " remaining=" .. total_remaining_enemies() ..
        " frame=" .. frame)
    if e.carrier and not silent then
        spawn_powerup(e.power_kind or "star", e.x, e.y)
    end
    if #spawn_queue == 0 and count_active_enemies() == 0 then
        start_stage_clear()
    end
end

-- ============================ 道具 ============================

function remove_powerup()
    if powerup.active then
        powerup.active = false
        park(powerup.id)
    end
end

function spawn_powerup(kind, x, y)
    if powerup.active then remove_powerup() end
    powerup.active = true
    powerup.kind = kind
    powerup.x, powerup.y = x, y
    powerup.t = POWERUP_TTL
    place(powerup.id, x, y)
    set_tex(powerup.id, POWER_TEX[kind] or POWER_TEX.star)
    node_set_opacity(powerup.id, 1.0)
    unpark(powerup.id)
    log("tank: powerup spawn kind=" .. kind .. " x=" .. math.floor(x) ..
        " y=" .. math.floor(y) .. " frame=" .. frame)
end

function activate_shovel()
    shovel_ring = {}
    local cells = {
        { 12, 23 }, { 13, 23 }, { 14, 23 }, { 15, 23 },
        { 12, 24 }, { 13, 24 }, { 14, 24 }, { 15, 24 },
        { 11, 25 }, { 12, 25 }, { 11, 26 }, { 12, 26 },
        { 15, 25 }, { 16, 25 }, { 15, 26 }, { 16, 26 },
    }
    for i = 1, #cells do
        local c, r = cells[i][1], cells[i][2]
        shovel_ring[#shovel_ring + 1] = { c = c, r = r }
        set_cell(c, r, "@")
    end
    shovel_timer = 12.0
    log("tank: powerup shovel base walls steel frame=" .. frame)
end

function update_shovel(dt)
    if shovel_timer <= 0 then return end
    shovel_timer = shovel_timer - dt
    if shovel_timer > 0 then
        local blink = shovel_timer <= 3.0 and (math.floor(frame / 8) % 2 == 0)
        for i = 1, #shovel_ring do
            local p = shovel_ring[i]
            if map[p.r][p.c] == "@" or map[p.r][p.c] == "#" then
                set_cell(p.c, p.r, blink and "#" or "@")
            end
        end
    else
        shovel_timer = 0
        for i = 1, #shovel_ring do
            local p = shovel_ring[i]
            if map[p.r][p.c] ~= "B" and map[p.r][p.c] ~= "." then
                set_cell(p.c, p.r, "#")
            end
        end
        shovel_ring = {}
        log("tank: shovel expired frame=" .. frame)
    end
end

function apply_powerup(kind)
    remove_powerup()
    sfx("powerup", 0.7)
    if kind == "star" then
        if player.level < 3 then
            player.level = player.level + 1
            player_level = player.level
            update_player_sprite()
        else
            add_score(1000)
        end
        log("tank: powerup star level=" .. player.level .. " frame=" .. frame)
    elseif kind == "helmet" then
        player.shield = math.max(player.shield, 10.0)
        log("tank: powerup helmet shield=10 frame=" .. frame)
    elseif kind == "grenade" then
        local n = 0
        for i = 1, #enemies do
            local e = enemies[i]
            if e.active then
                kill_enemy(e, true)
                add_score(200)
                n = n + 1
            end
        end
        log("tank: powerup grenade kills=" .. n .. " frame=" .. frame)
    elseif kind == "shovel" then
        activate_shovel()
    elseif kind == "tank" then
        lives = lives + 1
        log("tank: powerup tank lives=" .. lives .. " frame=" .. frame)
    elseif kind == "timer" then
        freeze_timer = 8.0
        log("tank: powerup timer freeze=8 frame=" .. frame)
    end
end

function update_powerup(dt)
    if not powerup.active then return end
    if player and player.alive and
       math.abs(player.x - powerup.x) < 24 and math.abs(player.y - powerup.y) < 24 then
        apply_powerup(powerup.kind)
        return
    end
    powerup.t = powerup.t - dt
    if powerup.t <= 0 then
        remove_powerup()
        return
    end
    node_set_opacity(powerup.id, (math.floor(frame / 10) % 2 == 0) and 1.0 or 0.45)
end

-- ============================ 关卡组成 / 流程 ============================

function make_enemy_queue(st)
    local loop = math.floor((st - 1) / STAGE_COUNT)
    local m = (st - 1) % STAGE_COUNT + 1
    local counts
    if m == 1 then counts = { basic = 14, fast = 4, power = 1, armor = 1 }
    elseif m == 2 then counts = { basic = 10, fast = 5, power = 3, armor = 2 }
    elseif m == 3 then counts = { basic = 7, fast = 5, power = 5, armor = 3 }
    else counts = { basic = 5, fast = 4, power = 6, armor = 5 }
    end
    local list = {}
    for _, kind in ipairs({ "basic", "fast", "power", "armor" }) do
        for _ = 1, counts[kind] do list[#list + 1] = kind end
    end
    for i = #list, 2, -1 do
        local j = math.floor(rng() * i) + 1
        if j < 1 then j = 1 end
        list[i], list[j] = list[j], list[i]
    end
    local carrier_at = {}
    for k = 1, 3 do
        local idx = 4 + (k - 1) * 5 + math.floor(rng() * 2)
        if idx > #list then idx = #list end
        carrier_at[idx] = true
    end
    local q = {}
    for i = 1, #list do
        local kind = list[i]
        if loop > 0 and kind == "basic" and i % 4 == 0 then kind = "armor" end
        q[i] = {
            kind = kind,
            carrier = carrier_at[i] == true,
            power_kind = POWER_KINDS[((i + loop) % #POWER_KINDS) + 1],
        }
    end
    return q
end

function spawn_initial_wave()
    local n = 0
    while n < MAX_ACTIVE_ENEMIES and #spawn_queue > 0 do
        local spec = table.remove(spawn_queue, 1)
        if spawn_enemy(spec) then
            n = n + 1
        else
            table.insert(spawn_queue, 1, spec)
            break
        end
    end
    spawn_timer = 2.6
end

function begin_play()
    state = S_PLAY
    spawn_initial_wave()
    log("tank: stage " .. stage .. " play frame=" .. frame)
end

function start_stage_clear()
    if state ~= S_PLAY then return end
    state = S_CLEAR
    stage_clear_timer = 2.4
    remove_powerup()
    for i = 1, #bullets do bullet_kill(bullets[i], false) end
    sfx("stage_clear", 0.75)
    log("tank: stage " .. stage .. " clear score=" .. score ..
        " lives=" .. lives .. " frame=" .. frame)
end

function trigger_game_over(reason)
    if state == S_OVER then return end
    if score > hi_score then hi_score = score end
    state = S_OVER
    over_timer = 0
    over_accept = false
    stop_bgm()
    remove_powerup()
    for i = 1, #bullets do bullet_kill(bullets[i], false) end
    sfx("game_over", 0.8)
    log("tank: game over reason=" .. reason .. " score=" .. score ..
        " stage=" .. stage .. " lives=" .. lives .. " frame=" .. frame)
end

function hide_actors()
    if player then park(player.id); park(player.shield_id) end
    park(muzzle)
    for i = 1, #enemies do park(enemies[i].id) end
    for i = 1, #bullets do park(bullets[i].id) end
    for i = 1, #explosions do park(explosions[i].id) end
    for i = 1, #spawn_fx do park(spawn_fx[i].id) end
    park(powerup.id)
end

function return_to_title()
    state = S_TITLE
    want_stage = false
    stage_instantiate_tried = false
    stop_bgm()
    hide_actors()
    if stage_root >= 0 then node_set_active(stage_root, 0) end
    if stage_inst >= 0 then
        scene_unload(stage_inst)
        stage_inst = -1
        stage_root = -1
    end
    if field_bg >= 0 then node_set_active(field_bg, 0) end
    if field_frame >= 0 then node_set_active(field_frame, 0) end
    stage_clear_timer = 0
    over_timer = 0
    over_accept = false
    log("tank: title frame=" .. frame)
end

function new_game()
    player_level = 0
    if stage_override > 0 then stage = stage_override else stage = 1 end
    score = 0
    lives = 2
    next_extra = 20000
    over_timer = 0
    over_accept = false
    if player then
        player.level = 0
        player.alive = false
        player.shield = 0
    end
    request_stage(stage)
    log("tank: new game stage=" .. stage .. " lives=" .. lives ..
        " autoplay=" .. (env_autoplay and 1 or 0) .. " frame=" .. frame)
end

-- ============================ 自走测试（真实操作等价） ============================

local bot_path = {}
local bot_path_age = 0

function map_open_for_tank(c, r)
    if c < 1 or c > COLS - 1 or r < 1 or r > ROWS - 1 then return false end
    for rr = r, r + 1 do
        for cc = c, c + 1 do
            if is_block_tank(map[rr][cc]) then return false end
        end
    end
    return true
end

function bfs_path(sc, sr, tc, tr)
    if not map_open_for_tank(tc, tr) then
        local best, bd = nil, 1e9
        for r = 1, ROWS - 1 do
            for c = 1, COLS - 1 do
                if map_open_for_tank(c, r) then
                    local d = math.abs(c - tc) + math.abs(r - tr)
                    if d < bd then bd, best = d, { c, r } end
                end
            end
        end
        if not best then return {} end
        tc, tr = best[1], best[2]
    end
    if not map_open_for_tank(sc, sr) then
        local best, bd = nil, 1e9
        for i = 1, 4 do
            local d = DIRS[DIR_ORDER[i]]
            local c = sc + (d.dx > 0 and 1 or 0)
            local r = sr + (d.dy < 0 and 1 or 0)
            if map_open_for_tank(c, r) then
                local dd = math.abs(c - tc) + math.abs(r - tr)
                if dd < bd then bd, best = dd, { c, r } end
            end
        end
        if not best then return {} end
        sc, sr = best[1], best[2]
    end
    local N = 25
    local function key(c, r) return (r - 1) * N + c end
    local prev, seen, q = {}, {}, { { sc, sr } }
    seen[key(sc, sr)] = true
    local head, found = 1, false
    while head <= #q do
        local cur = q[head]; head = head + 1
        if cur[1] == tc and cur[2] == tr then found = true; break end
        for i = 1, 4 do
            local d = DIRS[DIR_ORDER[i]]
            local c, r = cur[1] + d.dx, cur[2] - d.dy
            if map_open_for_tank(c, r) and not seen[key(c, r)] then
                seen[key(c, r)] = true
                prev[key(c, r)] = cur
                q[#q + 1] = { c, r }
            end
        end
    end
    if not found then return {} end
    local path = {}
    local c, r = tc, tr
    while not (c == sc and r == sr) do
        table.insert(path, 1, { c, r })
        local p = prev[key(c, r)]
        if not p then return {} end
        c, r = p[1], p[2]
    end
    return path
end

function bot_pick_target()
    if powerup.active then
        local c, r = tank_cell(powerup.x, powerup.y)
        return c, r
    end
    local best, bd = nil, 1e18
    for i = 1, #enemies do
        local e = enemies[i]
        if e.active and not e.spawning then
            local d = dist2(e.x, e.y, player.x, player.y)
            if d < bd then bd, best = d, e end
        end
    end
    if best then return tank_cell(best.x, best.y) end
end

function bot_repath()
    if not player or not player.alive then bot_path = {}; return end
    local sc, sr = tank_cell(player.x, player.y)
    local tc, tr = bot_pick_target()
    if not tc then bot_path = {}; return end
    bot_path = bfs_path(sc, sr, tc, tr)
    bot_path_age = 0
end

function bot_fire_blocked_by_base()
    local v = DIRS[player.dir]
    local bx, by = player.x, player.y
    for _ = 1, 22 do
        bx = bx + v.dx * 16
        by = by + v.dy * 16
        local c, r = cell_of_world(bx, by)
        local ch = map[r][c]
        if ch == "B" then return true end
        if is_block_bullet(ch) then return false end
    end
    return false
end

function bot_has_target_ahead()
    for i = 1, #enemies do
        local e = enemies[i]
        if e.active and not e.spawning and
           aligned_shot(player.x, player.y, e.x, e.y, player.dir, 300) then
            local v = DIRS[player.dir]
            local c, r = cell_of_world(player.x + v.dx * 20, player.y + v.dy * 20)
            if not is_block_tank(map[r][c]) then return true end
        end
    end
    return false
end

function bot_want_fire()
    if not player or not player.alive then return false end
    if bot_has_target_ahead() then return true end
    local v = DIRS[player.dir]
    local c, r = cell_of_world(player.x + v.dx * 28, player.y + v.dy * 28)
    if map[r][c] == "#" and not bot_fire_blocked_by_base() then return true end
    return false
end

function bot_dir()
    if not player or not player.alive then return nil end
    bot_path_age = bot_path_age + 1
    if bot_path_age > 20 or #bot_path == 0 then bot_repath() end
    local sc, sr = tank_cell(player.x, player.y)
    while #bot_path > 0 do
        local p = bot_path[1]
        if p[1] == sc and p[2] == sr then table.remove(bot_path, 1)
        else break end
    end
    if #bot_path > 0 then
        local p = bot_path[1]
        if p[1] > sc then return "r" elseif p[1] < sc then return "l"
        elseif p[2] < sr then return "u" elseif p[2] > sr then return "d" end
    end
    -- 没路径时沿“危险最小”的方向挪一步，避免长期站桩。
    local bx, by = base_center()
    if math.abs(player.x - bx) > 24 then return player.x < bx and "r" or "l" end
    if math.abs(player.y - by) > 24 then return player.y < by and "u" or "d" end
    return nil
end

-- ============================ HUD 发布（global_* 共享通道） ============================

function set_global_num(key, v)
    if hud_num_cache[key] ~= v then
        global_set(key, v)
        hud_num_cache[key] = v
    end
end

function set_global_text(key, v)
    if hud_text_cache[key] ~= v then
        global_set_text(key, v)
        hud_text_cache[key] = v
    end
end

function overlay_text()
    if state == S_LOADING then return "STAGE " .. pad_num(pending_stage, 2) end
    if state == S_INTRO then return "STAGE " .. pad_num(stage, 2) end
    if state == S_PAUSE then return "PAUSED" end
    if state == S_CLEAR then return "STAGE " .. pad_num(stage, 2) .. " CLEAR" end
    if state == S_OVER then return "GAME OVER" end
    return ""
end

function publish_hud()
    local mode = state == S_TITLE and "title" or "battle"
    set_global_text("hud.mode", mode)
    set_global_num("hud.score", score)
    set_global_num("hud.hi", math.max(hi_score, score))
    set_global_num("hud.stage", state == S_LOADING and pending_stage or stage)
    set_global_num("hud.lives", lives)
    local remaining = 0
    if state == S_LOADING then
        remaining = ENEMIES_PER_STAGE
    elseif state ~= S_TITLE then
        remaining = total_remaining_enemies()
    end
    set_global_num("hud.enemy", remaining)
    set_global_text("hud.overlay", state == S_TITLE and "" or overlay_text())
    set_global_num("hud.prompt", over_accept and 1 or 0)
end

-- ============================ 测试钩子 ============================

function hook_brick()
    if not player or not player.alive then return end
    player.x, player.y = FX + 9 * CELL, FY + 8 * CELL
    player.dir = "u"
    for r = 10, 16 do
        for c = 8, 11 do
            if map[r][c] == "#" or map[r][c] == "@" then set_cell(c, r, ".") end
        end
    end
    set_cell(9, 5, "#")
    set_cell(10, 5, "#")
    player.level = math.max(player.level, 1)
    player.fire_cd = 0
    player.shield = 99
    update_player_sprite()
    fire_player()
    log("tank: drill brick armed x=" .. math.floor(player.x) .. " y=" .. math.floor(player.y) ..
        " frame=" .. frame)
end

function hook_base()
    if not player then return end
    -- 站在基地正上方 72px，朝下开火；中间 4 行清空，让子弹走真实路径进入 B 格。
    player.x, player.y = base_center()
    player.y = base_y + 72.0
    player.dir = "d"
    player.shield = 99
    for r = 21, 25 do
        for c = 12, 15 do
            if map[r][c] == "#" or map[r][c] == "@" then set_cell(c, r, ".") end
        end
    end
    update_player_sprite()
    player.fire_cd = 0
    player.alive = true
    fire_player()
    log("tank: drill base armed x=" .. math.floor(player.x) .. " y=" .. math.floor(player.y) ..
        " frame=" .. frame)
end

function hook_power()
    if not player then return end
    player.shield = 99
    spawn_powerup(hook_power_kind, player.x, player.y)
    log("tank: drill power kind=" .. hook_power_kind .. " frame=" .. frame)
end

function hook_clear()
    spawn_queue = {}
    local n = 0
    for i = 1, #enemies do
        local e = enemies[i]
        if e.active then
            kill_enemy(e, true)
            n = n + 1
        end
    end
    log("tank: drill clear kills=" .. n .. " frame=" .. frame)
    if state == S_PLAY then start_stage_clear() end
end

-- ============================ 更新 ============================

function update_spawns(dt)
    if state ~= S_PLAY then return end
    if #spawn_queue == 0 or count_active_enemies() >= MAX_ACTIVE_ENEMIES then return end
    spawn_timer = spawn_timer - dt
    if spawn_timer > 0 then return end
    local spec = spawn_queue[1]
    if spawn_enemy(spec) then
        table.remove(spawn_queue, 1)
        spawn_timer = 2.2
    else
        spawn_timer = 0.5
    end
end

function update_play(dt)
    update_player(dt)
    update_enemies(dt)
    update_bullets(dt)
    update_powerup(dt)
    update_explosions(dt)
    update_spawn_fx(dt)
    update_shovel(dt)
    update_spawns(dt)
    if freeze_timer > 0 then freeze_timer = freeze_timer - dt end
    update_water(dt)
end

function update(dt)
    frame = frame + 1

    if env_trace and frame % 60 == 0 then
        log("tank: trace frame=" .. frame .. " state=" .. state ..
            " score=" .. score .. " lives=" .. lives .. " stage=" .. stage ..
            " px=" .. math.floor(player and player.x or 0) ..
            " py=" .. math.floor(player and player.y or 0) ..
            " enemies=" .. count_active_enemies() .. " queue=" .. #spawn_queue ..
            " freeze=" .. string.format("%.1f", freeze_timer))
    end

    if state == S_TITLE then
        if env_autoplay and frame > 20 then new_game() end
    elseif state == S_LOADING then
        update_loading()
    elseif state == S_INTRO then
        intro_timer = intro_timer - dt
        update_explosions(dt)
        update_spawn_fx(dt)
        if player then update_player_sprite() end
        if intro_timer <= 0 then begin_play() end
    elseif state == S_PLAY then
        if hook_brick_at > 0 and frame == hook_brick_at then hook_brick(); hook_brick_at = 0 end
        if hook_base_at > 0 and frame == hook_base_at then hook_base(); hook_base_at = 0 end
        if hook_power_at > 0 and frame == hook_power_at then hook_power(); hook_power_at = 0 end
        if hook_clear_at > 0 and frame == hook_clear_at then hook_clear(); hook_clear_at = 0 end
        if state == S_PLAY then update_play(dt) end
    elseif state == S_PAUSE then
        -- UI 在 hud.lua 中继续闪烁；世界冻结。
    elseif state == S_CLEAR then
        stage_clear_timer = stage_clear_timer - dt
        update_explosions(dt)
        update_spawn_fx(dt)
        update_bullets(dt)
        if stage_clear_timer <= 0 then
            stage = stage + 1
            request_stage(stage)
        end
    elseif state == S_OVER then
        over_timer = over_timer + dt
        update_explosions(dt)
        update_spawn_fx(dt)
        update_bullets(dt)
        if over_timer > 1.2 then over_accept = true end
        if env_autoplay and over_timer > 2.6 then new_game() end
    end

    publish_hud()
end

-- ============================ 输入 ============================

function key_is_start(key)
    return key == 3 or key == 13 or key == 2 or key == 32 or
           key == 74 or key == 106
end

function on_key_down(target, key)
    if state == S_TITLE then
        if key_is_start(key) then new_game() end
        return
    elseif state == S_OVER then
        if over_accept and key_is_start(key) then return_to_title() end
        return
    elseif state == S_PAUSE then
        if key == 80 or key == 112 or key == 3 or key == 13 or key == 1 or key == 27 then
            state = S_PLAY
            if audio_on then audio_bus_volume("music", 0.17) end
            log("tank: resume frame=" .. frame)
        end
        return
    elseif state == S_PLAY then
        if key == 80 or key == 112 then
            state = S_PAUSE
            if audio_on then audio_bus_volume("music", 0.0) end
            sfx("pause", 0.4)
            log("tank: pause frame=" .. frame)
            return
        elseif key == 1 or key == 27 then
            return_to_title()
            return
        end
        if key == 2 or key == 32 or key == 74 or key == 106 or key == 90 or key == 122 then
            fire_player()
        end
    end
end

function on_pointer_down(target, x, y)
    if state == S_TITLE then
        new_game()
    elseif state == S_OVER then
        if over_accept then return_to_title() end
    elseif state == S_PAUSE then
        state = S_PLAY
        if audio_on then audio_bus_volume("music", 0.17) end
    elseif state == S_PLAY then
        fire_player()
    end
end

-- ============================ 初始化 ============================

function actor_name(prefix, suffix)
    return prefix .. (suffix or "body")
end

function init_actors()
    local pid = entity_find(actor_name("p_"))
    local sid = entity_find(actor_name("shield_"))
    if pid < 0 or sid < 0 then
        log("tank: missing player prefab actors")
        quit()
        return false
    end
    park(pid)
    park(sid)
    player = {
        id = pid, shield_id = sid, x = 0, y = 0, dir = "u", level = 0,
        alive = false, shield = 0, fire_cd = 0, respawn = 0, slide = 0,
        muzzle_t = 0, moved = false, flash = 0,
    }
    node_set_z_index(pid, 20)
    node_set_z_index(sid, 55)
    set_tex(pid, "/art/tank_player_l0_u.png")
    set_tex(sid, "/art/shield0.png")

    for i = 1, MAX_ACTIVE_ENEMIES do
        local id = entity_find("e" .. i .. "_body")
        if id < 0 then log("tank: missing enemy prefab " .. i); quit(); return false end
        park(id)
        node_set_z_index(id, 20)
        enemies[i] = {
            idx = i, id = id, active = false, spawning = false, dead = true,
            x = 0, y = 0, dir = "d", kind = "basic", hp = 1, max_hp = 1,
            speed = 36, carrier = false, power_kind = "star",
            fire_cd = 1, think = 0, flash = 0, invuln = 0, spawn_t = 0,
            stuck = 0,
        }
    end

    for i = 1, 20 do
        local nm = "b" .. string.format("%02d", i) .. "_body"
        local id = entity_find(nm)
        if id < 0 then log("tank: missing bullet prefab " .. i); quit(); return false end
        park(id)
        node_set_z_index(id, 30)
        bullets[i] = { id = id, active = false, team = 0, owner = nil,
                       x = 0, y = 0, vx = 0, vy = 0, speed = 0, power = false, life = 0 }
    end

    for i = 1, 14 do
        local nm = "x" .. string.format("%02d", i) .. "_body"
        local id = entity_find(nm)
        if id < 0 then log("tank: missing explosion prefab " .. i); quit(); return false end
        park(id)
        node_set_z_index(id, 50)
        explosions[i] = { id = id, active = false, x = 0, y = 0, t = 0, scale = 1.0 }
    end

    for i = 1, MAX_ACTIVE_ENEMIES do
        local id = entity_find("s" .. i .. "_body")
        if id < 0 then log("tank: missing spawn prefab " .. i); quit(); return false end
        park(id)
        node_set_z_index(id, 56)
        spawn_fx[i] = { id = id, active = false, t = 0 }
    end

    local pow = entity_find("power_body")
    local muz = entity_find("muzzle_body")
    if pow < 0 or muz < 0 then log("tank: missing powerup/muzzle prefab"); quit(); return false end
    powerup.id = pow
    park(pow)
    node_set_z_index(pow, 25)
    muzzle = muz
    park(muzzle)
    node_set_z_index(muzzle, 35)
    set_tex(muzzle, "/art/explosion0.png")
    return true
end

function on_start()
    audio_on = audio_enabled() ~= 0 and
        env("MAQUE_AUDIO") ~= "" and env("MAQUE_AUDIO") ~= "off" and
        env("MAQUE_AUDIO") ~= "null"
    bgm_on = env("MAQUE_TANK_BGM") ~= "0"

    env_autoplay = env("MAQUE_TANK_AUTOPLAY") == "1"
    env_god = env("MAQUE_TANK_GOD") == "1"
    env_trace = env("MAQUE_TANK_TRACE") == "1"
    env_skip_title = env("MAQUE_TANK_SKIP_TITLE") == "1"
    stage_override = tonumber(env("MAQUE_TANK_STAGE") or "0") or 0
    hook_brick_at = tonumber(env("MAQUE_TANK_BRICK_AT") or "0") or 0
    hook_base_at = tonumber(env("MAQUE_TANK_BASE_AT") or "0") or 0
    hook_power_at = tonumber(env("MAQUE_TANK_POWER_AT") or "0") or 0
    hook_power_kind = env("MAQUE_TANK_POWER_KIND") or "star"
    if hook_power_kind == "" then hook_power_kind = "star" end
    hook_clear_at = tonumber(env("MAQUE_TANK_CLEAR_AT") or "0") or 0

    bg = entity_find("bg")
    field_bg = entity_find("field_bg")
    field_frame = entity_find("field_frame")
    stage_host = entity_find("StageHost")
    if bg >= 0 then node_set_z_index(bg, -100) end
    if field_bg >= 0 then node_set_z_index(field_bg, 0); park(field_bg) end
    if field_frame >= 0 then node_set_z_index(field_frame, 2); park(field_frame) end

    new_grid()
    if not init_actors() then return end

    state = S_TITLE
    hide_actors()
    publish_hud()
    log("tank: ready 640x480 stages=" .. STAGE_COUNT .. " enemies=" .. ENEMIES_PER_STAGE ..
        " lua=1 autoplay=" .. (env_autoplay and 1 or 0))
    if env_skip_title or env_autoplay or hook_brick_at > 0 or
       hook_base_at > 0 or hook_power_at > 0 then
        new_game()
    end
end

function on_destroy()
end
