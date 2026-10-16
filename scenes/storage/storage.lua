-- storage.lua —— 与 storage.js **同一逻辑**（验证 JS/Lua 共用同一套 DSL 生成的绑定，docs/14/21）。
-- 计数跨进程持久（默认 file://<场景目录>/.maque），键空间 <uid>/<session>/（默认 default/main）。
_KEY = 'demo/count'

local function refresh_texts(n)
  local info = entity_find('Info')
  if info >= 0 then
    node_set_string(info, 'scn.Label', 'text', 'storage ' .. storage.info() .. '  count=' .. n)
  end
  local keys = entity_find('Keys')
  if keys >= 0 then node_set_string(keys, 'scn.Label', 'text', 'keys: ' .. storage.keys()) end
end

function on_start()
  local prev = storage.get(_KEY)
  local n = 0
  if prev ~= '' then n = tonumber(prev) or 0 end
  n = n + 1
  storage.set(_KEY, tostring(n))
  storage.set('demo/last_seen', tostring(n))
  refresh_texts(n)
  log('storage_demo count=' .. n .. ' keys=' .. storage.keys()
      .. ' has=' .. storage.exists(_KEY) .. ' mode=' .. storage.info())
  if input_key(1) > 0 then quit() end
end

function update() end
