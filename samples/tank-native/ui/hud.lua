-- HUD 子场景（ui/hud.scn）的行为。
--
-- 设计要点（这是"场景嵌套"的示范）：
--   · HUD 是一个**独立 SceneInstance**：自己的 VM、自己的实体池、自己的 update。
--   · 它不读取 game.lua 的 Lua 变量，也不跨场景抓根场景的节点；只读共享的
--     `global_*` 渠道（docs/32 §16.3）。游戏逻辑只负责写：
--         hud.mode / hud.score / hud.hi / hud.stage / hud.lives / hud.enemy
--         hud.overlay / hud.prompt
--   · 纯视图没有输入/碰撞；标题画面的闪烁、坦克巡逻和位图字体排版都在这里，
--     根脚本只决定"现在是什么模式/显示什么消息"。
--   · 位图字体仍是 8x8 PNG（老游戏观感），HUD 内部按字形池复用实体。

local W = 640.0
local H = 480.0
local HUD_X = 456.0
local TEXT_SLOTS = 360

local text_slots = {}
local tex_of = {}
local text_cursor = 1
local text_prev_max = 0

local panel = -1
local logo = -1
local dialog = -1
local title_a = -1
local title_b = -1
local enemy_icons = {}
local life_icons = {}

local function clamp(v, lo, hi)
    if v < lo then return lo end
    if v > hi then return hi end
    return v
end

local function pad_num(n, width)
    local s = tostring(math.floor(n + 0.5))
    while #s < width do s = "0" .. s end
    return s
end

local function set_tex(id, path)
    if id < 0 then return end
    if tex_of[id] ~= path then
        node_set_string(id, "scn.Sprite", "texture", path)
        tex_of[id] = path
    end
end

local function set_rgb(id, r, g, b)
    node_set_number(id, "scn.Sprite", "r", r)
    node_set_number(id, "scn.Sprite", "g", g)
    node_set_number(id, "scn.Sprite", "b", b)
end

local function sprite(x, y, w, h, z, tex)
    local id = entity_spawn()
    node_add_component(id, "scn.Transform2D")
    node_add_component(id, "scn.Sprite")
    node_set_position(id, x, y)
    node_set_number(id, "scn.Sprite", "width", w)
    node_set_number(id, "scn.Sprite", "height", h)
    set_rgb(id, 1, 1, 1)
    node_set_z_index(id, z)
    if tex then set_tex(id, tex) end
    return id
end

local function glyph_path(ch, scale)
    if ch == " " then return nil end
    local cp = string.byte(ch, 1)
    if cp == nil then return nil end
    if cp >= 97 and cp <= 122 then cp = cp - 32 end
    local allowed = (cp >= 65 and cp <= 90) or (cp >= 48 and cp <= 57) or
        cp == 46 or cp == 44 or cp == 58 or cp == 45 or cp == 47 or cp == 33 or
        cp == 63 or cp == 39 or cp == 40 or cp == 41 or cp == 43 or cp == 60 or
        cp == 62 or cp == 61
    if not allowed then cp = 63 end
    return "/art/glyph" .. (scale >= 2 and "16" or "8") .. "/" .. cp .. ".png"
end

local function text_begin()
    text_cursor = 1
end

local function text_end()
    for i = text_cursor, text_prev_max do
        node_set_active(text_slots[i].id, 0)
    end
    if text_cursor - 1 > text_prev_max then text_prev_max = text_cursor - 1 end
end

local function draw_text(left, top, str, scale, r, g, b, visible)
    scale = scale or 1
    local chw = 8 * scale
    local adv = 6 * scale
    for i = 1, #str do
        local slot = text_slots[text_cursor]
        if not slot then break end
        text_cursor = text_cursor + 1
        local ch = string.sub(str, i, i)
        local path = glyph_path(ch, scale)
        if not path or visible == false then
            node_set_active(slot.id, 0)
        else
            node_set_active(slot.id, 1)
            node_set_position(slot.id, left + (i - 1) * adv + chw * 0.5, H - top - chw * 0.5)
            node_set_number(slot.id, "scn.Sprite", "width", chw)
            node_set_number(slot.id, "scn.Sprite", "height", chw)
            set_rgb(slot.id, r or 1.0, g or 1.0, b or 1.0)
            set_tex(slot.id, path)
        end
    end
end

local function text_width(str, scale)
    return #str * 6 * (scale or 1)
end

local function centered(top, str, scale, r, g, b, visible)
    draw_text((W - text_width(str, scale)) * 0.5, top, str, scale, r, g, b, visible)
end

-- 过场/暂停/结算面板只在战场（224 中心）里，不盖住右侧 HUD。
local function centered_field(top, str, scale, r, g, b, visible)
    draw_text(224.0 - text_width(str, scale) * 0.5, top, str, scale, r, g, b, visible)
end

local function gnum(key)
    local v = global_get(key)
    if v == nil then return 0 end
    return v
end

local function gtext(key)
    local v = global_get_text(key)
    if v == nil then return "" end
    return v
end

local function init_pools()
    panel = entity_find("hud_panel")
    logo = entity_find("logo")
    dialog = entity_find("dialog_bg")
    title_a = entity_find("titleA_body")
    title_b = entity_find("titleB_body")
    if panel >= 0 then node_set_z_index(panel, 100) end
    if logo >= 0 then node_set_z_index(logo, 500) end
    if dialog >= 0 then
        node_set_z_index(dialog, 8500)
        node_set_opacity(dialog, 0.78)
        node_set_active(dialog, 0)
    end
    if title_a >= 0 then
        node_set_z_index(title_a, 400)
        set_tex(title_a, "/art/title_player.png")
        node_set_position(title_a, 76, 244)
        node_set_active(title_a, 1)
    end
    if title_b >= 0 then
        node_set_z_index(title_b, 400)
        set_tex(title_b, "/art/title_armor.png")
        node_set_position(title_b, 564, 244)
        node_set_active(title_b, 1)
    end

    for i = 1, TEXT_SLOTS do
        local id = sprite(0, 0, 8, 8, 9000, nil)
        node_set_active(id, 0)
        text_slots[i] = { id = id }
    end

    for i = 1, 20 do
        local id = sprite(0, 0, 16, 16, 1000, "/art/hud_enemy.png")
        node_set_active(id, 0)
        enemy_icons[i] = id
    end
    for i = 1, 6 do
        local id = sprite(0, 0, 16, 16, 1000, "/art/hud_life.png")
        node_set_active(id, 0)
        life_icons[i] = id
    end
end

local function draw_title()
    if panel >= 0 then node_set_active(panel, 0) end
    if dialog >= 0 then node_set_active(dialog, 0) end
    if logo >= 0 then node_set_active(logo, 1) end
    if title_a >= 0 then
        node_set_active(title_a, 1)
        node_set_position(title_a, 76 + math.sin(director_frame() / 80.0) * 26, 244)
    end
    if title_b >= 0 then
        node_set_active(title_b, 1)
        node_set_position(title_b, 564 - math.sin(director_frame() / 80.0) * 26, 244)
    end
    for i = 1, #enemy_icons do node_set_active(enemy_icons[i], 0) end
    for i = 1, #life_icons do node_set_active(life_icons[i], 0) end

    centered(20, "FC 1990 STYLE REMAKE", 1, 0.45, 0.50, 0.58, true)
    centered(204, "1 PLAYER", 2, 1.0, 0.84, 0.30, true)
    local blink = math.floor(director_frame() / 24) % 2 == 0
    centered(248, "PUSH ENTER TO START", 2, 1.0, 1.0, 1.0, blink)
    centered(332, "ARROWS / WASD  -  MOVE", 1, 0.72, 0.76, 0.82, true)
    centered(348, "SPACE / J  -  FIRE      P  -  PAUSE", 1, 0.72, 0.76, 0.82, true)
    centered(382, "DESTROY 20 ENEMIES  -  PROTECT THE EAGLE", 1, 0.45, 0.50, 0.58, true)
    centered(430, "HI " .. pad_num(gnum("hud.hi"), 6), 1, 1.0, 0.82, 0.28, true)
end

local function draw_hud()
    if panel >= 0 then node_set_active(panel, 1) end
    if logo >= 0 then node_set_active(logo, 0) end
    if title_a >= 0 then node_set_active(title_a, 0) end
    if title_b >= 0 then node_set_active(title_b, 0) end

    local overlay = gtext("hud.overlay")
    if dialog >= 0 then node_set_active(dialog, overlay ~= "" and 1 or 0) end

    draw_text(HUD_X, 22, "HI", 1, 0.72, 0.76, 0.82, true)
    draw_text(HUD_X, 34, pad_num(gnum("hud.hi"), 6), 1, 1, 1, 1, true)
    draw_text(HUD_X, 62, "SCORE", 1, 0.72, 0.76, 0.82, true)
    draw_text(HUD_X, 74, pad_num(gnum("hud.score"), 6), 1, 1, 1, 1, true)
    draw_text(HUD_X, 102, "STAGE", 1, 0.72, 0.76, 0.82, true)
    draw_text(HUD_X, 114, pad_num(gnum("hud.stage"), 2), 1, 1, 1, 1, true)

    local remain = math.floor(gnum("hud.enemy") + 0.5)
    for i = 1, #enemy_icons do
        local col = math.floor((i - 1) / 10)
        local row = (i - 1) % 10
        node_set_position(enemy_icons[i], HUD_X + 14 + col * 20, H - (146 + row * 17) - 8)
        node_set_active(enemy_icons[i], i <= remain and 1 or 0)
    end

    draw_text(HUD_X, 326, "IP", 1, 0.72, 0.76, 0.82, true)
    local lives = math.floor(gnum("hud.lives") + 0.5)
    for i = 1, #life_icons do
        node_set_position(life_icons[i], HUD_X + 14 + (i - 1) * 18, H - 342 - 8)
        node_set_active(life_icons[i], i <= lives and 1 or 0)
    end
    draw_text(HUD_X, 388, "ARROWS/WASD", 1, 0.42, 0.46, 0.53, true)
    draw_text(HUD_X, 400, "SPACE/J FIRE", 1, 0.42, 0.46, 0.53, true)
    draw_text(HUD_X, 412, "P PAUSE", 1, 0.42, 0.46, 0.53, true)

    local blink = math.floor(director_frame() / 18) % 2 == 0
    if overlay ~= "" then
        if string.sub(overlay, 1, 5) == "STAGE" and string.find(overlay, "CLEAR") then
            centered_field(198, overlay, 2, 1, 1, 1, true)
            centered_field(246, "NEXT STAGE", 1, 1.0, 0.82, 0.28, blink)
        elseif string.sub(overlay, 1, 5) == "STAGE" then
            centered_field(198, overlay, 2, 1, 1, 1, true)
            centered_field(258, "GET READY", 1, 1.0, 0.82, 0.28, blink)
        elseif overlay == "PAUSED" then
            centered_field(210, "PAUSE", 2, 1, 1, 1, true)
            centered_field(260, "P / ENTER TO RESUME", 1, 1.0, 1.0, 1.0, true)
        elseif overlay == "GAME OVER" then
            centered_field(186, "GAME OVER", 2, 1.0, 0.42, 0.32, true)
            centered_field(232, "SCORE " .. pad_num(gnum("hud.score"), 6), 1, 1, 1, 1, true)
            centered_field(256, "HI " .. pad_num(gnum("hud.hi"), 6), 1, 1.0, 0.82, 0.28, true)
            centered_field(300, "PUSH ENTER", 2, 1, 1, 1,
                     gnum("hud.prompt") > 0 and blink)
        end
    end
end

function on_start()
    init_pools()
end

function update(dt)
    local mode = gtext("hud.mode")
    if mode == "" then mode = "title" end
    text_begin()
    if mode == "title" then
        draw_title()
    else
        draw_hud()
    end
    text_end()
end
