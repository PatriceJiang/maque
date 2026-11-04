-- zorder.lua —— 引擎 2D z_index 回归探针（docs/bugs/013）。
-- 红块 z=0、绿块 z=9000，同一位置；正确实现下中心像素必须是绿色。
local function solid(x, y, r, g, b, z)
    local id = entity_spawn()
    node_add_component(id, "scn.Transform2D")
    node_add_component(id, "scn.Sprite")
    node_set_position(id, x, y)
    node_set_number(id, "scn.Sprite", "width", 100)
    node_set_number(id, "scn.Sprite", "height", 100)
    node_set_number(id, "scn.Sprite", "r", r)
    node_set_number(id, "scn.Sprite", "g", g)
    node_set_number(id, "scn.Sprite", "b", b)
    node_set_z_index(id, z)
    log("zorder: id=" .. id .. " z=" .. tostring(node_z_index(id)))
end

function on_start()
    solid(160, 120, 1, 0, 0, 0)
    solid(160, 120, 0, 1, 0, 9000)
end

function update(dt)
end
