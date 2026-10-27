# maque player —— 通用宿主（无游戏逻辑）
add_executable(maque_player main.cpp)
target_link_libraries(maque_player PRIVATE maque::runtime)
maque_deploy_plugin_deps(maque_player)   # Windows：ASan 运行时 + wgpu_native.dll 放到 exe 旁

if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME player_run
        COMMAND maque_player "${CMAKE_SOURCE_DIR}/scenes/ball.scn"
                             "${CMAKE_CURRENT_BINARY_DIR}/frames")
endif()

# 确定性：同场景跑两次，world_trail 必须一致（docs/10）
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME determinism COMMAND "${MAQUE_BASH}" -c
      "P=$<TARGET_FILE:maque_player>; S=${CMAKE_SOURCE_DIR}/scenes/ball.scn; \
       A=$($<TARGET_FILE:maque_player> ${CMAKE_SOURCE_DIR}/scenes/ball.scn 2>/dev/null | grep -oE 'world_trail=0x[0-9a-f]+'); \
       B=$($<TARGET_FILE:maque_player> ${CMAKE_SOURCE_DIR}/scenes/ball.scn 2>/dev/null | grep -oE 'world_trail=0x[0-9a-f]+'); \
       test -n \"$A\" && test \"$A\" = \"$B\"")
endif()

# 控制平面（headless 驱动）：AI 可查询指标/场景/校验和
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME control_plane COMMAND "${MAQUE_BASH}" -c
      "MAQUE_CONTROL='{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"Perf.get\"}' $<TARGET_FILE:maque_player> ${CMAKE_SOURCE_DIR}/scenes/ball.scn 2>/dev/null | grep -q '\"metrics\"'")
endif()

# 截图端到端：单张 PNG/PPM + 确定性 + **批量截图**（--shot-seq / --shot-frames /
# --shot-every / --shot-count）+ `--frames` 退出码语义（到上限 = 正常结束，返回 0）。
# 注：tests/shot_e2e.sh 一直存在但**此前从未注册成测试** —— 截图路径等于没有回归，这轮补上。
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME shot_e2e COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/shot_e2e.sh"
             $<TARGET_FILE:maque_player>
             "${CMAKE_SOURCE_DIR}/scenes"
             "${CMAKE_CURRENT_BINARY_DIR}/shot_e2e")
endif()

# Tracy 性能分析端到端（docs/29）：默认不启动客户端；MAQUE_TRACY=1 时 tracy-capture 能抓到
# 帧/zone；F3 启动客户端并拉起 profiler 进程（CI 用 capture 二进制当假 profiler）。
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME tracy_e2e COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/tracy_e2e.sh"
             $<TARGET_FILE:maque_player>
             "${CMAKE_SOURCE_DIR}/samples"
             "${CMAKE_CURRENT_BINARY_DIR}/tracy_e2e")
    set_tests_properties(tracy_e2e PROPERTIES SKIP_RETURN_CODE 77)
endif()

# 音频端到端（docs/30）：脚本 audio_* → VFS 解码 → 混音器逻辑时钟/状态机；
# null 后端确定性断言 + SDL dummy 驱动设备路径。
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME audio_e2e COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/audio_e2e.sh"
             $<TARGET_FILE:maque_player>
             "${CMAKE_SOURCE_DIR}/tests/data/audio/audio_e2e.scn"
             "${CMAKE_CURRENT_BINARY_DIR}/audio_e2e")
    set_tests_properties(audio_e2e PROPERTIES SKIP_RETURN_CODE 77)

    add_test(NAME audio_control_e2e COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/audio_control_e2e.sh"
             $<TARGET_FILE:maque_player>
             "${CMAKE_SOURCE_DIR}/tests/data/audio/audio_control.scn")
    set_tests_properties(audio_control_e2e PROPERTIES SKIP_RETURN_CODE 77)
endif()

# ImGui 面板开关（F2）端到端：沿检测（单次/按住/两次）+ 反复开关不崩 + 面板确实画进像素缓冲。
# headless 无真实键盘 → 用 MAQUE_SIM_KEY_AT 注入按下事件（与真实键盘同一条路径）。
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME imgui_toggle_e2e COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/imgui_toggle_e2e.sh"
             $<TARGET_FILE:maque_player>
             "${CMAKE_SOURCE_DIR}/samples"
             "${CMAKE_CURRENT_BINARY_DIR}/imgui_toggle_e2e")
    set_tests_properties(imgui_toggle_e2e PROPERTIES SKIP_RETURN_CODE 77)
endif()

# ImGui 窗口呈现端到端（docs/28）：① imgui 开启时游戏画面仍上屏（统一 renderer、单次 present）
# ② stats 可拖动（合成指针走真实输入路径）；③ UI 度量固定（换窗口大小不变）；④ software 回退可见。
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME imgui_present_e2e COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/imgui_present_e2e.sh"
             $<TARGET_FILE:maque_player>
             "${CMAKE_SOURCE_DIR}/samples"
             "${CMAKE_CURRENT_BINARY_DIR}/imgui_present_e2e")
    set_tests_properties(imgui_present_e2e PROPERTIES SKIP_RETURN_CODE 77)
endif()

# 3D/PBR 直接呈现端到端（plans/18-pbr-3d.md M1）：MAQUE_PRESENT=wgpu → wgpu swapchain 直呈
# ① attach_window 成功且不建 SDL_Renderer ② MAQUE_CAPTURE_WGPU 从交换链抓帧断言画面非空
# ③ 直呈环境下 --shot 仍走离屏回读（截图/CI 路径不受影响）
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME render3d_present_e2e COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/render3d_present_e2e.sh"
             $<TARGET_FILE:maque_player>
             "${CMAKE_SOURCE_DIR}/scenes/ball.scn"
             "${CMAKE_CURRENT_BINARY_DIR}/render3d_present_e2e")
    set_tests_properties(render3d_present_e2e PROPERTIES SKIP_RETURN_CODE 77)
endif()

# 3D 渲染基础端到端（plans/18-pbr-3d.md M2）：render.* 组件 → depth + 3D pass + Lambert 透视
# ① --shot 结构断言（非背景占比/中心可见/顶部为空/明暗面颜色多样性）② 3D + wgpu 交换链直呈抓帧
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME render3d_scene_e2e COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/render3d_scene_e2e.sh"
             $<TARGET_FILE:maque_player>
             "${CMAKE_SOURCE_DIR}/scenes/pbr/cube.scn"
             "${CMAKE_CURRENT_BINARY_DIR}/render3d_scene_e2e")
    set_tests_properties(render3d_scene_e2e PROPERTIES SKIP_RETURN_CODE 77)
endif()

# 场景自定义全屏 shader 端到端（render.ShaderPass + Shadertoy Seascape）：
# WGSL 是场景资源（VFS 根 = 场景目录），非内建 shader。① --shot 日志含 shader pass 加载行 +
# "runtime: shot" + PPM 640x360 结构断言（颜色丰富/非纯色/天空亮于海面）② frame 1 vs 45 有显著
# 像素差（time 推进）③ 同帧两次逐字节一致 ④ WGSL 在 scenes/shadertoy/、场景 JSON 挂组件并引用它。
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME shadertoy_e2e COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/shadertoy_e2e.sh"
             $<TARGET_FILE:maque_player>
             "${CMAKE_SOURCE_DIR}/scenes/shadertoy/seascape.scn"
             "${CMAKE_CURRENT_BINARY_DIR}/shadertoy_e2e")
    set_tests_properties(shadertoy_e2e PROPERTIES SKIP_RETURN_CODE 77)
endif()

# PBR/迁移渲染回归（docs/31 §11 P3）：①迁移 manifest/compare 指标下限 ②Normal oracle + 镜像 front-face
# ③已提交 preview.jpg 渲染基线（重渲染 ToyCar/Duck/CompareSpecular 与提交预览比较；无 PIL 时跳过该节）。
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME pbr_regression_e2e COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/pbr_regression_e2e.sh"
             $<TARGET_FILE:maque_player>
             "${CMAKE_CURRENT_BINARY_DIR}/pbr_regression_e2e")
    set_tests_properties(pbr_regression_e2e PROPERTIES SKIP_RETURN_CODE 77)
endif()

# 材质模型端到端（plans/19-threejs-materials.md）：three.js 材质族 → asset.PbrMaterial.shading_model。
# ① material_models_e2e：9 个对象（Standard/Physical/Basic/Lambert/Phong/Toon/Normal/Matcap/
#    Depth；Shadow 无遮挡时透明，由 ② 的阴影地面覆盖）离屏截图 → 连通域聚类 + 场景 JSON 定位 →
#    逐模型结构性断言 + 对照组（Basic 平坦 vs Lambert 渐变 vs Phong 高光 / Toon 分级 /
#    Normal 彩色法线 / Matcap 冷色渐变 / Depth 灰度 / Standard+Physical 非空）。
# ② material_shadow_e2e：同场景 MAQUE_SHADOW=1 与 =0 两张 → “开时变暗像素数”显著更多 +
#    空间一致性（单连通块、集中在可见物体附近，防“整个 shadow 视锥误遮挡”复现）。
# ③ material_physical_e2e：MAQUE_ORBIT 两角度 → Physical vs Standard 差异 + iridescence 视角偏色。
# 场景由 scenes/pbr 并行产出：任一场景缺失时脚本自身 SKIP 77（不阻塞构建）。
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME material_models_e2e COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/material_models_e2e.sh"
             $<TARGET_FILE:maque_player>
             "${CMAKE_SOURCE_DIR}/scenes/pbr/material_models.scn"
             "${CMAKE_CURRENT_BINARY_DIR}/material_models_e2e")
    set_tests_properties(material_models_e2e PROPERTIES SKIP_RETURN_CODE 77)

    add_test(NAME material_shadow_e2e COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/material_shadow_e2e.sh"
             $<TARGET_FILE:maque_player>
             "${CMAKE_SOURCE_DIR}/scenes/pbr/material_shadow.scn"
             "${CMAKE_CURRENT_BINARY_DIR}/material_shadow_e2e")
    set_tests_properties(material_shadow_e2e PROPERTIES SKIP_RETURN_CODE 77)

    add_test(NAME material_physical_e2e COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/material_physical_e2e.sh"
             $<TARGET_FILE:maque_player>
             "${CMAKE_SOURCE_DIR}/scenes/pbr/material_physical.scn"
             "${CMAKE_CURRENT_BINARY_DIR}/material_physical_e2e")
    set_tests_properties(material_physical_e2e PROPERTIES SKIP_RETURN_CODE 77)
endif()

# glTF 离线导入端到端（plans/18 方向 2 / M4b）：夹具 glTF → tools/gltf_import.py →
# 结构/确定性/--name/错误路径断言（纯 Python，无需图形环境；无 python 时 SKIP 77）
if(MAQUE_BUILD_TESTS)
    add_test(NAME gltf_import_e2e COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/gltf_import_e2e.sh"
             "${CMAKE_SOURCE_DIR}/tools/gltf_import.py"
             "${CMAKE_SOURCE_DIR}/tests/data/gltf/triangle.gltf"
             "${CMAKE_CURRENT_BINARY_DIR}/gltf_import_e2e")
    set_tests_properties(gltf_import_e2e PROPERTIES SKIP_RETURN_CODE 77)
endif()

# 输入/UI/快照/回放端到端（docs/10 §3/§5、docs/19 §9）：注入 → 分发 → 生成 → 回放一致
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME ui_e2e COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/ui_e2e.sh"
             $<TARGET_FILE:maque_player>
             "${CMAKE_SOURCE_DIR}/scenes/ui.scn"
             "${CMAKE_CURRENT_BINARY_DIR}/ui_e2e")
endif()

# 热重载端到端（docs/08/14）：运行中改脚本 → 帧边界重载 → 世界演化与基线逐帧一致
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME hot_reload_e2e COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/hot_reload_e2e.sh"
             $<TARGET_FILE:maque_player>
             "${CMAKE_SOURCE_DIR}/scenes/ui.scn"
             "${CMAKE_CURRENT_BINARY_DIR}/hot_e2e")
    # Windows 上该用例主动 skip（Git Bash 的 FIFO 送不到原生进程 stdin；脚本内有详细说明）
    set_tests_properties(hot_reload_e2e PROPERTIES SKIP_RETURN_CODE 77)
endif()

# 动画/粒子端到端（docs/15 §2/§4、docs/10 §5）：关键帧采样 + 粒子发射 + 确定性 + 回滚保真
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME anim_e2e COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/anim_e2e.sh"
             $<TARGET_FILE:maque_player>
             "${CMAKE_SOURCE_DIR}/scenes/anim/anim.scn"
             "${CMAKE_CURRENT_BINARY_DIR}/anim_e2e")
endif()

# 最小编辑器端到端（M9、docs/11 §3/§4）：编辑 → 落盘 → 重载 → 往返一致
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME editor_e2e COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/editor_e2e.sh"
             $<TARGET_FILE:maque_player>
             "${CMAKE_SOURCE_DIR}/scenes/anim/anim.scn"
             "${CMAKE_CURRENT_BINARY_DIR}/editor_e2e")
endif()

# Prefab 实例化与 bake（docs/12 §5）：展开 → 落盘自包含 → bake 幂等 → 不再重复展开
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME prefab_e2e COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/prefab_e2e.sh"
             $<TARGET_FILE:maque_player>
             "${CMAKE_SOURCE_DIR}/scenes/prefab/prefab.scn"
             "${CMAKE_CURRENT_BINARY_DIR}/prefab_e2e")
endif()

# Prefab **嵌套**实例化（docs/12 §5/§7）：prefab 里再引用 prefab。
# prefab_e2e 只覆盖单层展开；嵌套是"重复对象做成独立资源"（如坦克的炮塔挂子弹）的刚需。
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME nested_prefab_e2e COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/nested_prefab_e2e.sh"
             $<TARGET_FILE:maque_player>
             "${CMAKE_SOURCE_DIR}/scenes/prefab/nested.scn"
             "${CMAKE_CURRENT_BINARY_DIR}/nested_prefab_e2e")
endif()

# 场景实例（docs/32）：`.scn` 自带行为 → 静态嵌套 + 每实例 VM/生命周期/事件作用域 + 动态实例化/卸载。
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME scene_instance_e2e COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/scene_instance_e2e.sh"
             $<TARGET_FILE:maque_player>
             "${CMAKE_SOURCE_DIR}/scenes/nested/parent.scn"
             "${CMAKE_CURRENT_BINARY_DIR}/scene_instance_e2e")
endif()

# Prefab **平级展开**（实体级 `source` 简写）：纯展开指令、不套壳 —— 用于"平级重复对象"
# （坦克的 20 个履带印/5 个残骸就是这种形态；用 PrefabInstance 组件会改层级并白增空壳实体）。
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME inline_prefab_e2e COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/inline_prefab_e2e.sh"
             $<TARGET_FILE:maque_player>
             "${CMAKE_SOURCE_DIR}/scenes/prefab/inline.scn"
             "${CMAKE_CURRENT_BINARY_DIR}/inline_prefab_e2e")
endif()

# 崩溃/异常上下文（docs/09 §5、docs/10 §7）：脚本异常 → 结构化报告（seed/帧号/检查点/重放）
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME crash_context_e2e COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/crash_context_e2e.sh"
             $<TARGET_FILE:maque_player>
             "${CMAKE_SOURCE_DIR}/scenes/anim"
             "${CMAKE_CURRENT_BINARY_DIR}/crash_e2e")
endif()

# 导航端到端（docs/15 导航）：A* 路径 → 逐帧追踪 → 到达目标；两跑确定性
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME nav_e2e COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/nav_e2e.sh"
             $<TARGET_FILE:maque_player>
             "${CMAKE_SOURCE_DIR}/scenes/nav/nav.scn"
             "${CMAKE_CURRENT_BINARY_DIR}/nav_e2e")
endif()

# AI 标准工作流（docs/11 §5）：自省 → 逐帧驱动 → 快照/回滚 → 采集 → MCP 工具调用
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME ai_workflow_e2e COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/ai_workflow_e2e.sh"
             $<TARGET_FILE:maque_player>
             "${CMAKE_SOURCE_DIR}/scenes/anim/anim.scn"
             "${CMAKE_CURRENT_BINARY_DIR}/ai_workflow_e2e")
endif()

# 节点/组件 API 端到端（docs/18）：层级/节点属性/组件/事件/UI 控件/物理组件/Director
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME nodes_e2e COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/nodes_e2e.sh"
             $<TARGET_FILE:maque_player>
             "${CMAKE_SOURCE_DIR}/scenes/nodes/nodes.scn"
             "${CMAKE_CURRENT_BINARY_DIR}/nodes_e2e")
endif()

# 文本端到端（docs/17/18）：Label/TextLabel/RichText 排版 + 富文本 run + 像素级验证
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME text_e2e COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/text_e2e.sh"
             $<TARGET_FILE:maque_player>
             "${CMAKE_SOURCE_DIR}/scenes/text/text.scn"
             "${CMAKE_CURRENT_BINARY_DIR}/text_e2e")
endif()

# 窗口呈现端到端（docs/05）：真实窗口 + 呈现路径 + 缩放（无显示环境自动跳过）
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME window_present_e2e COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/window_present_e2e.sh"
             $<TARGET_FILE:maque_player>
             "${CMAKE_SOURCE_DIR}/scenes/ball.scn"
             "${CMAKE_CURRENT_BINARY_DIR}/window_present_e2e")
    set_tests_properties(window_present_e2e PROPERTIES SKIP_RETURN_CODE 77)
endif()

# 窗口 resize / 最大化端到端（docs/05）：① 运行期 resize（MAQUE_WINDOW_EVENT_AT，走真实 SDL
# 事件路径）→ 自动倍率下渲染目标（FBO）重建、抓帧尺寸跟随窗口 ② 启动即最大化（MAQUE_WINDOW_MAXIMIZED）
# ③ 显式 MAQUE_RENDER_SCALE 时目标固定（窗口变化只缩放呈现）④ 连续两次 resize 不崩/无 ASan。
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME window_resize_e2e COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/window_resize_e2e.sh"
             $<TARGET_FILE:maque_player>
             "${CMAKE_SOURCE_DIR}/scenes/ball.scn"
             "${CMAKE_CURRENT_BINARY_DIR}/window_resize_e2e")
    set_tests_properties(window_resize_e2e PROPERTIES SKIP_RETURN_CODE 77)
endif()

# 真实输入通路端到端（docs/19）：SDL 事件队列注入 → 平台翻译 → 脚本回调 + 坐标映射
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME input_click_e2e COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/input_click_e2e.sh"
             $<TARGET_FILE:maque_player>
             "${CMAKE_SOURCE_DIR}/scenes/ball.scn"
             "${CMAKE_CURRENT_BINARY_DIR}/input_click_e2e")
    set_tests_properties(input_click_e2e PROPERTIES SKIP_RETURN_CODE 77)
endif()

# 输入模拟器端到端（docs/19）：场景文件回放 + 确定性 + 录制/回放等价 + 控制面 Input.sim_*
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME sim_input_e2e COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/sim_input_e2e.sh"
             $<TARGET_FILE:maque_player>
             "${CMAKE_SOURCE_DIR}/scenes/ball.scn"
             "${CMAKE_CURRENT_BINARY_DIR}/sim_input_e2e")
    set_tests_properties(sim_input_e2e PROPERTIES SKIP_RETURN_CODE 77)
endif()

# Lua 脚本后端端到端（docs/14）：语言选择 / 宿主 API / 事件回调 / 跨语言一致性 / 确定性
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME lua_e2e COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/lua_e2e.sh"
             $<TARGET_FILE:maque_player>
             "${CMAKE_CURRENT_BINARY_DIR}/lua_e2e")
endif()

# 画布/分辨率策略端到端（docs/17）：多窗口尺寸适配 + 输入映射 + 安全区 + fit 切换
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME canvas_e2e COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/canvas_e2e.sh"
             $<TARGET_FILE:maque_player>
             "${CMAKE_SOURCE_DIR}/scenes/canvas/canvas.scn"
             "${CMAKE_CURRENT_BINARY_DIR}/canvas_e2e")
    set_tests_properties(canvas_e2e PROPERTIES SKIP_RETURN_CODE 77)
endif()

# 推箱子 demo（第二 demo，JS）：规则/阻挡/撤销/关卡/确定性（按键经输入模拟器，无需 DISPLAY）
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME sokoban_e2e COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/sokoban_e2e.sh"
             $<TARGET_FILE:maque_player>
             "${CMAKE_SOURCE_DIR}/scenes/sokoban/sokoban.scn"
             "${CMAKE_CURRENT_BINARY_DIR}/sokoban_e2e")
endif()

# 远程加载端到端（docs/04）：VFS 挂 http 目录 —— 场景/脚本经 HTTP 抓取 + 与本地轨迹一致
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    find_package(CURL QUIET)
    if(CURL_FOUND)
        add_test(NAME remote_e2e COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/remote_e2e.sh"
                 $<TARGET_FILE:maque_player>
                 "${CMAKE_SOURCE_DIR}/scenes"
                 "${CMAKE_CURRENT_BINARY_DIR}/remote_e2e")
        set_tests_properties(remote_e2e PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

# 多语言脚本协作（docs/14 §多 VM）：跨语言事件 + 确定性 + 宿主 emit
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME multilang_e2e COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/multilang_e2e.sh"
             $<TARGET_FILE:maque_player> $<TARGET_FILE:maque_inspector>
             "${CMAKE_SOURCE_DIR}/scenes/multilang/multilang.scn"
             "${CMAKE_CURRENT_BINARY_DIR}/multilang_e2e")
endif()

# 多脚本热重载（docs/14 §多 VM）：增量生效 + 其它脚本状态保留 + 坏脚本原子回退
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME hot_reload_multi_e2e COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/hot_reload_multi_e2e.sh"
             $<TARGET_FILE:maque_player> "${CMAKE_SOURCE_DIR}/scenes/multilang"
             "${CMAKE_CURRENT_BINARY_DIR}/hot_reload_multi_e2e")
    # Windows 上该用例主动 skip（无 stdbuf，无法在进程运行中读日志；脚本内有详细说明）
    set_tests_properties(hot_reload_multi_e2e PROPERTIES SKIP_RETURN_CODE 77)
endif()

# Khronos 画廊端到端（scenes/khronos/index.scn + gallery.js，overlay_script 常驻覆盖层）：
# ① 入口 149 个模型标题的分页 list（gallery_title/item_N/page_label/page_prev/page_next）
# ② 点 item_0 → demo 场景（title=MODELS[0]，demo_title/demo_index 文本一致）
# ③ demo 上一个/下一个在排序列表内循环（首项 prev→末项、末项 next→首项）
# ④ demo_back → 入口标题 + 列表重建；⑤ page_next/page_prev 改页码文本并还原
# ⑥ 入口/demo 各抓一帧（Capture.screen 分块 base64 → PPM/PNG）做像素断言（1280x720、
#    两图显著不同、demo 中央有模型像素、入口有文字像素）。内容缺失时脚本自身 SKIP 77。
if(MAQUE_BUILD_TESTS AND TARGET maque_gfx)
    add_test(NAME khronos_gallery_e2e COMMAND "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/khronos_gallery_e2e.sh"
             $<TARGET_FILE:maque_player>
             "${CMAKE_SOURCE_DIR}"
             "${CMAKE_CURRENT_BINARY_DIR}/khronos_gallery_e2e")
    set_tests_properties(khronos_gallery_e2e PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
endif()
