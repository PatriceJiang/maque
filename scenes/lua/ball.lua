-- ball.lua —— 与 scenes/ball.js **逐行等价**的 Lua 版弹跳球（点击生成球）。
-- 演示：同一套宿主 API（DSL 声明 → 生成绑定）在 Lua 后端可用；同一份语义 → 同一世界演化。
-- 引擎侧：脚本语言由扩展名选择（.lua → lua 后端，docs/14「统一后端 ABI」），沙箱运行、确定性 GC。
--
-- 行为：① 场景球自由落体 → 触地弹跳 → 静止；② 按下鼠标在按下处生成球（向上初速）；
--       ③ 每球静止 5 秒后移除；场上无球时退出。坐标是世界坐标（y 向上，引擎已做窗口→世界映射）。

local gravity     = 980.0   -- px/s^2
local restitution = 0.6     -- 恢复系数
local groundR     = 8.0     -- 球心最低 y（半径）
local removeDelay = 5.0     -- 静止后移除延迟（秒）
local clickSpeed  = 220.0   -- 点击生成时的向上初速

local balls = {}            -- { {id=, x=, y=, vy=, resting=, restTime=} }
local clicks = 0

local function add_ball(id, x, y, vy)
    balls[#balls + 1] = { id = id, x = x, y = y, vy = vy, resting = false, restTime = 0.0 }
end

function on_start()
    -- 引擎接口：math.clamp / math.lerp / math.saturate 由宿主注入（与 js 后端同名同义）
    log("math.clamp(5,0,3)=" .. math.clamp(5, 0, 3)
        .. " math.lerp(0,10,0.25)=" .. math.lerp(0, 10, 0.25)
        .. " math.saturate(-3)=" .. math.saturate(-3))
    if entity_count() > 0 then
        local id = 0
        local x, y = entity_x(id), entity_y(id)
        add_ball(id, x, y, 0.0)
        log("ball: use scene entity id=" .. id .. " at (" .. x .. "," .. y .. ")")
    else
        local x = screen_w() * 0.5
        local id = spawn_ball(x, 200.0, 0.0, 0.0, groundR)
        add_ball(id, x, 200.0, 0.0)
        log("ball: spawn entity id=" .. id)
    end
    log("ball: 点击窗口可在按下处生成球")
end

-- 按下鼠标（事件派发按回调表：on_pointer_down(target, x, y)）
function on_pointer_down(target, x, y)
    local id = spawn_ball(x, y, 0.0, clickSpeed, groundR)
    if id < 0 then return end
    add_ball(id, x, y, clickSpeed)
    clicks = clicks + 1
    log("ball: click #" .. clicks .. " spawn id=" .. id
        .. " at (" .. math.floor(x + 0.5) .. "," .. math.floor(y + 0.5) .. ") balls=" .. #balls)
end

function update(dt)
    local i = #balls
    while i >= 1 do
        local b = balls[i]
        if not b.resting then
            b.vy = b.vy - gravity * dt          -- 自由落体
            b.y  = b.y + b.vy * dt
            if b.y <= groundR then              -- 触地 → 弹跳
                b.y = groundR
                b.vy = -b.vy * restitution
                if math.abs(b.vy) < 20.0 then b.vy = 0.0; b.resting = true end
            end
            entity_set_pos(b.id, b.x, b.y)
        else
            b.restTime = b.restTime + dt
            if b.restTime >= removeDelay then
                entity_destroy(b.id)            -- 静止 5s 后销毁实体
                log("ball: destroy entity id=" .. b.id .. " (剩余 " .. (#balls - 1) .. ")")
                table.remove(balls, i)
            end
        end
        i = i - 1
    end
    -- 场上没有球 → demo 结束（不点击时与旧行为一致）
    if #balls == 0 then
        log("ball: 全部静止并移除，退出（clicks=" .. clicks .. "）")
        quit()
    end
end
