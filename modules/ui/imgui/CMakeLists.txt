# maque::imgui —— 引擎侧 ImGui 集成（即时模式调试/调参 UI）。
#
# 设计要点（docs/13 §pass 层）：
#   · **画在最上层**：ImGui 的网格以 layer=IMGUI_LAYER（高于引擎 UI 的 layer=1）提交，
#     走引擎既有的 render_mesh_views 路径 → 复用 GPU 管线（采样/混合由它保证），
#     且写入**同一个像素缓冲** → 截图/自证里也能看到（imgui_impl_sdlrenderer2 那种
#     "只画到窗口"的做法做不到这点）。
#   · **调用时机**：宿主在引擎渲染完（含 UI pass）、**呈现之前**调用
#     `mq_imgui_render(...)`，把绘制数据追加到同一帧的网格列表里。
#   · 引擎核心（modules/runtime）只依赖本模块的 C ABI，不直接依赖 ImGui 头。
set(_imgui "${CMAKE_SOURCE_DIR}/third_party/imgui")

add_library(maque_imgui STATIC
    src/imgui.cpp
    "${_imgui}/imgui.cpp"
    "${_imgui}/imgui_draw.cpp"
    "${_imgui}/imgui_tables.cpp"
    "${_imgui}/imgui_widgets.cpp")
add_library(maque::imgui ALIAS maque_imgui)

target_include_directories(maque_imgui
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    # 注：imgui 头对**本模块的测试**也要可见（测试要构造控件跑一帧），所以放在 PUBLIC。
    # 但对外承诺的公开面只有 include/ui/mq_imgui.h —— 别的模块应只 include 它。
    PUBLIC "${_imgui}")
set_target_properties(maque_imgui PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)

# ImGui 是第三方源码：静默其告警（不改上游代码）
target_compile_options(maque_imgui PRIVATE
    $<$<CXX_COMPILER_ID:Clang,AppleClang,GNUCXX>:-Wno-everything>
    $<$<CXX_COMPILER_ID:MSVC>:/W0>)

if(MAQUE_BUILD_TESTS)
    add_executable(maque_imgui_test tests/imgui_test.cpp)
    maque_deploy_asan(maque_imgui_test)   # Windows：ASan 运行时 DLL 放到 exe 旁
    target_link_libraries(maque_imgui_test PRIVATE maque::imgui)
    add_test(NAME imgui COMMAND maque_imgui_test)
endif()
