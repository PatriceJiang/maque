-- 3D 变换 typed API（plans/36 M1-4）：Lua 侧值类型 = table。
local done = false

local function near(a, b, eps)
    eps = eps or 1e-3
    return math.abs(a - b) <= eps
end

function update()
    if done or director_frame() < 2 then return end
    done = true
    local id = entity_find("node")
    local ok = true
    local function check(v, msg)
        if not v then log("transform_api_lua: FAIL " .. msg) end
        ok = ok and v
    end

    local p = node_local_position(id)
    check(type(p) == "table" and near(p.x, 1.0) and near(p.y, 2.0) and near(p.z, 3.0),
          "local_position table")

    node_set_local_position(id, 2.0, 3.0, 4.0)
    local q = node_local_position(id)
    check(near(q.x, 2.0) and near(q.y, 3.0) and near(q.z, 4.0), "set local position")

    -- 值参数：table 与 3 个数字两种写法
    local w1 = node_local_to_world(id, { x = 0.0, y = 0.0, z = 0.0 })
    local w2 = node_local_to_world(id, 0.0, 0.0, 0.0)
    check(near(w1.x, 2.0) and near(w1.y, 3.0) and near(w1.z, 4.0), "value arg table")
    check(near(w2.x, w1.x) and near(w2.y, w1.y) and near(w2.z, w1.z), "value arg numbers")

    local back = node_world_to_local(id, w1)
    check(near(back.x, 0.0) and near(back.y, 0.0) and near(back.z, 0.0), "world_to_local")

    node_set_local_rotation(id, 0.0, 0.0, 0.0)
    node_look_at(id, 2.0, 3.0, -1.0)   -- 世界 -Z 朝前
    local r = node_local_rotation(id)
    check(near(r.x, 0.0) and near(r.y, 0.0) and near(r.z, 0.0), "look_at")

    local sc = node_local_scale(id)
    check(near(sc.x, 1.0) and near(sc.y, 1.0) and near(sc.z, 1.0), "local_scale")

    log(ok and "transform_api_lua: PASS" or "transform_api_lua: FAIL")
end
