/* 宿主 API · 全局消息总线（msg_subscribe / msg_unsubscribe / msg_publish）
 *
 * 与 node 事件互补（docs/14 §多 VM、docs/19 §3.1）：
 *   · node 事件绑定在节点上，沿父链冒泡；消息只按 (bus, subject, event) 匹配，
 *     与节点树/场景实例无关 → 跨节点、跨子场景、跨 VM 的全局通信；
 *   · 订阅记录注册时的脚本上下文（根/子实例、global、overlay），发布时在该上下文里
 *     调用 handler；顺序 = 订阅顺序，错误隔离沿用脚本总线；
 *   · 载荷 = 最多 3 个数值（与事件总线一致）；同步派发，确定性。
 *
 * callback 的两种形式：
 *   · JS/Lua 函数：VM 创建后安装的 shim 把它挂到该 VM 的全局唯一名字上
 *     （`__mq_msg_cb_<vm>_<n>`），C 侧只保存名称；卸载时 shim 负责删除；
 *   · 全局函数名字符串：与 node_on/schedule 的名字约定一致（native 只能用这种）。
 */
#include "../runtime_engine.h"

#include <algorithm>
#include <cstring>
#include <print>
#include <string>
#include <vector>

using namespace maque::rt;

namespace {

const char* const kMsgShimJs = R"JS(
(function () {
  var rawSub = globalThis.msg_subscribe;
  var rawPub = globalThis.msg_publish;
  if (typeof rawSub !== 'function' || globalThis.__mq_msg_shimmed) return;
  globalThis.__mq_msg_shimmed = 1;
  var names = globalThis.__mq_msg_names = globalThis.__mq_msg_names || Object.create(null);
  var seq = 0;
  var vmId = globalThis.__mq_vm_id || 0;
  var sub = function (bus, subject, event, cb) {
    if (typeof cb === 'function') {
      var name = '__mq_msg_cb_' + vmId + '_' + (++seq);
      globalThis[name] = cb;
      names[name] = 1;
      return rawSub(bus, subject, event, name);
    }
    return rawSub(bus, subject, event, cb);
  };
  globalThis.msg_subscribe = sub;
  globalThis.msg_publish = function (bus, subject, event) {
    return rawPub(bus, subject, event,
      arguments.length > 3 ? arguments[3] : 0,
      arguments.length > 4 ? arguments[4] : 0,
      arguments.length > 5 ? arguments[5] : 0);
  };
  globalThis.__mq_msg_release = function (a, b, name) {
    if (typeof name === 'undefined') name = a;
    if (names[name]) { delete names[name]; try { delete globalThis[name]; } catch (e) {} }
  };
})();
)JS";

const char* const kMsgShimLua = R"LUA(
local raw_sub = msg_subscribe
local raw_pub = msg_publish
if type(raw_sub) == 'function' and not _G.__mq_msg_shimmed then
  _G.__mq_msg_shimmed = true
  local names = {}
  _G.__mq_msg_names = names
  local seq = 0
  local vm_id = _G.__mq_vm_id or 0
  function msg_subscribe(bus, subject, event, cb)
    if type(cb) == 'function' then
      seq = seq + 1
      local name = '__mq_msg_cb_' .. tostring(vm_id) .. '_' .. tostring(seq)
      _G[name] = cb
      names[name] = true
      return raw_sub(bus, subject, event, name)
    end
    return raw_sub(bus, subject, event, cb)
  end
  function msg_publish(bus, subject, event, a, b, c)
    return raw_pub(bus, subject, event, a or 0, b or 0, c or 0)
  end
  function __mq_msg_release(a, b, name)
    if name == nil then name = a end
    if names[name] then names[name] = nil; _G[name] = nil end
  end
end
)LUA";

bool msg_match(const std::string& pattern, const std::string& value) {
    return pattern == "*" || pattern == value;
}

}  // namespace

namespace maque::rt {

int Engine::install_message_shim(mq_script_vm* script_vm, const char* lang) {
    if (!script_vm || !lang) return 1;
    const int id = next_vm_id++;
    std::string code;
    if (std::strcmp(lang, "js") == 0) {
        code = "globalThis.__mq_vm_id = " + std::to_string(id) + ";\n" + kMsgShimJs;
    } else if (std::strcmp(lang, "lua") == 0) {
        code = "_G.__mq_vm_id = " + std::to_string(id) + "\n" + kMsgShimLua;
    } else {
        return 0;   // native：只有 handler 名字，没有函数 shim
    }
    const int rc = mq_script_eval(script_vm, code.c_str(), "@msg_shim");
    if (rc != 0) {
        char err[512] = { 0 };
        mq_script_last_error(script_vm, err, (int)sizeof err);
        std::print(stderr, "runtime: global message shim failed ({}): {}\n", lang, err);
    }
    return rc;
}

void Engine::clear_msg_listeners(int instance) {
    std::erase_if(msg_listeners, [&](const MsgListener& l) { return l.instance == instance; });
}

void Engine::clear_msg_listeners_slot(int instance, int slot) {
    if (slot < 0) return;
    std::erase_if(msg_listeners, [&](const MsgListener& l) {
        return l.instance == instance && l.slot == slot;
    });
}

int Engine::msg_publish_internal(const std::string& bus, const std::string& subject,
                                 const std::string& event, double a, double b, double c) {
    if (!msg_listeners.empty() && msg_depth >= 16) {
        std::print(stderr, "runtime: msg_publish nesting too deep (dropped)\n");
        return 0;
    }
    std::vector<int> ids;
    ids.reserve(msg_listeners.size());
    for (const MsgListener& l : msg_listeners) {
        if (msg_match(l.bus, bus) && msg_match(l.subject, subject) && msg_match(l.event, event))
            ids.push_back(l.id);
    }
    if (ids.empty()) return 0;

    ++msg_depth;
    int hits = 0;
    for (int id : ids) {
        auto it = std::find_if(msg_listeners.begin(), msg_listeners.end(),
                               [&](const MsgListener& l) { return l.id == id; });
        if (it == msg_listeners.end()) continue;      // 回调里被 unsubscribe → 跳过
        const MsgListener l = *it;

        if (l.module && l.module_vm) {
            SceneInstanceRuntime* inst = l.instance >= 0 ? instance_at(l.instance) : nullptr;
            if (l.instance >= 0 && !inst) continue;
            SceneInstanceRuntime* prev_inst = active_instance;
            mq_script_instance* prev_si = active_script_instance;
            const bool prev_g = in_global;
            const bool prev_ov = in_overlay;
            active_instance = inst;
            active_script_instance = l.module_inst;
            in_global = l.instance == -3;
            in_overlay = l.instance == -2;
            int rc = 0;
            if (l.module_inst && mq_script_instance_has(l.module_inst, l.handler.c_str()))
                rc = call_module_event(l.module_inst, l.handler.c_str(), a, b, c, nullptr) ? 0 : 1;
            else
                rc = mq_script_call3(l.module_vm, l.handler.c_str(), a, b, c, nullptr);
            active_instance = prev_inst;
            active_script_instance = prev_si;
            in_global = prev_g;
            in_overlay = prev_ov;
            if (rc == 0) ++hits;
            else if (rc == 2) ++module_errors;
        } else if (l.instance == -3) {
            hits += call_global_event(l.handler.c_str(), a, b, c, nullptr);
        } else if (l.instance == -2) {
            hits += call_overlay_event(l.handler.c_str(), a, b, c, nullptr);
        } else if (SceneInstanceRuntime* inst = instance_at(l.instance)) {
            hits += call_instance_event(*inst, l.handler.c_str(), a, b, c, nullptr);
        }
    }
    --msg_depth;
    return hits;
}

}  // namespace maque::rt

namespace host {

float msg_subscribe(void* ctx, const char* bus, const char* subject, const char* event,
                    const char* callback) {
    auto* eng = (Engine*)ctx;
    if (!eng || !bus || !*bus || !subject || !*subject || !event || !*event ||
        !callback || !*callback)
        return -1;
    Engine::MsgListener l;
    l.bus = bus;
    l.subject = subject;
    l.event = event;
    l.handler = callback;
    if (eng->in_global) l.instance = -3;
    else if (eng->in_overlay) l.instance = -2;
    else l.instance = eng->active_instance ? eng->active_instance->id : 0;
    if (eng->active_script_instance) {
        l.module = true;
        l.module_inst = eng->active_script_instance;
        l.module_vm = eng->module_vm;
    } else {
        l.slot = eng->script_slot_for_handler(callback);
    }
    l.id = eng->msg_next_id++;
    eng->msg_listeners.push_back(std::move(l));
    return (float)eng->msg_listeners.back().id;
}

float msg_unsubscribe(void* ctx, float listener) {
    auto* eng = (Engine*)ctx;
    if (!eng) return 0;
    const int id = (int)listener;
    auto it = std::find_if(eng->msg_listeners.begin(), eng->msg_listeners.end(),
                           [&](const Engine::MsgListener& l) { return l.id == id; });
    if (it == eng->msg_listeners.end()) return 0;
    const Engine::MsgListener l = *it;
    eng->msg_listeners.erase(it);
    /* 删除 shim 生成的全局函数；用户传的普通 handler 名字由 shim 的 names 表判断后不动。 */
    if (l.module && l.module_vm) {
        mq_script_call2s(l.module_vm, "__mq_msg_release", 0.0, 0.0, l.handler.c_str(), nullptr);
    } else {
        mq_script_bus* b = nullptr;
        if (l.instance == -3) b = eng->global_bus;
        else if (l.instance == -2) b = eng->overlay_bus;
        else if (SceneInstanceRuntime* inst = eng->instance_at(l.instance)) b = inst->bus;
        if (b) mq_script_bus_call(b, "__mq_msg_release", 0.0, 0.0, l.handler.c_str());
    }
    return 1;
}

float msg_publish(void* ctx, const char* bus, const char* subject, const char* event,
                  float a, float b, float c) {
    auto* eng = (Engine*)ctx;
    if (!eng || !bus || !*bus || !subject || !*subject || !event || !*event) return 0;
    return (float)eng->msg_publish_internal(bus, subject, event, (double)a, (double)b, (double)c);
}

}  // namespace host
