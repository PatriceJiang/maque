-- worker.lua —— Lua 子场景（旧路径：每个 SceneInstance 一个 Lua VM）。
-- 与 JS 根只有两条通道：scene_self_params() 进来的参数 + scene_emit 往返事件。
local S = { tag = '?', count = 0 }

function on_start()
  local p = scene_self_params()
  local tag = string.match(p or '', '"tag"%s*:%s*"([AB])"')
  if tag then S.tag = tag end
  log('MIX_WORKER_START inst=' .. scene_self() .. ' tag=' .. S.tag)
end

-- JS 根 scene_emit(child, 'ask', 100+i) -> 本函数（事件名 = 处理器名）
function ask(id, value)
  S.count = S.count + 1
  local v = math.floor(value)
  log('MIX_WORKER_ASK inst=' .. scene_self() .. ' tag=' .. S.tag ..
      ' value=' .. v .. ' count=' .. S.count)
  -- Lua -> JS 根（实例 0）：事件名 reply 对应 host.js 的 reply 方法
  scene_emit(0, 'reply', v + S.count)
end

function update(dt) end

function on_destroy()
  log('MIX_WORKER_DESTROY inst=' .. scene_self() .. ' tag=' .. S.tag .. ' count=' .. S.count)
end
