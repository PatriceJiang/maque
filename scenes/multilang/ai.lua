-- ai.lua —— AI/关卡逻辑（Lua）。与 game.js **只通过事件总线**协作。
-- 处理器名由 game.js 的事件触发（总线按名字在任意 VM 查找 → 跨语言无需额外协议）。
local S = { ticks = 0, count = 0 }

function on_start()
  -- 注册本语言要处理的事件（由 JS 侧 node_emit_number 触发）
  local status = entity_find("Status")
  node_on(entity_find("Title"), "ai_init", "on_ai_init")
  node_on(status, "ai_tick", "on_ai_tick")
  log("ml_lua_ready")
end

-- JS → Lua：初始化（value = 实体数量）
function on_ai_init(target, count)
  S.count = math.floor(count)
  log("ml_lua_init count=" .. S.count)
  node_emit_number(target, "ai_ready", S.count)      -- Lua → JS
end

-- JS → Lua：每 30 帧一次决策请求（value = 帧号）
function on_ai_tick(target, frame)
  S.ticks = S.ticks + 1
  local f = math.floor(frame)
  local y = 90 + (f * 7) % 60                        -- 确定性“AI 决策”
  node_emit_number(target, "ai_target", y)           -- Lua → JS
  if S.ticks % 10 == 0 then log("ml_lua_ticks=" .. S.ticks) end
end

function update(dt)
  -- 两语言各自有 update：顺序 = scripts 声明顺序（先 JS 后 Lua）
end
