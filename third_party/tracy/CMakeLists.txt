# Tracy Profiler —— 只编译**客户端**（vendored：third_party/tracy/public）。
# 官方 profiler GUI / capture 工具不入库（见 tools/tracy/get-profiler.ps1）。
#
# 配置要点（docs/29）：
#   · TRACY_ENABLE          —— 打开 Tracy 客户端
#   · TRACY_ON_DEMAND       —— 没有 profiler 连接时 zone 直接跳过（游戏常态零开销）
#   · TRACY_DELAYED_INIT + TRACY_MANUAL_LIFETIME
#                           —— **手动启动**（F3 / MAQUE_TRACY=1 才监听 8086），
#                              普通运行/CI 不监听端口；封装层在未启动时全部 no-op。
#   · TRACY_NO_CRASH_HANDLER —— **必须**：Tracy 自带的崩溃处理会安装信号/SEH 处理器，
#                              与 ASan 冲突。实测 Debug+ASan 构建里不关它时，capture 只能收到
#                              3 帧、0 zone（客户端协议半途停摆）；关掉后 129 帧/5418 zone 正常。
#                              崩溃诊断由仓库自己的 MAQUE_CRASH_REPORT / ASan 负责。
# 这些宏是 PUBLIC 的：调用方 TU 里的 Tracy 宏必须看到同一套配置（否则行为/ABI 不一致）。
add_library(maque_tracy_client STATIC public/TracyClient.cpp)
add_library(maque::tracy_client ALIAS maque_tracy_client)

target_compile_features(maque_tracy_client PUBLIC cxx_std_11)
target_include_directories(maque_tracy_client SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/public")
target_compile_definitions(maque_tracy_client PUBLIC
    TRACY_ENABLE
    TRACY_ON_DEMAND
    TRACY_DELAYED_INIT
    TRACY_MANUAL_LIFETIME
    TRACY_NO_CRASH_HANDLER)
set_target_properties(maque_tracy_client PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON)

# Tracy 是第三方源码：静默其告警（不改上游代码）
target_compile_options(maque_tracy_client PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/w>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-w>)

find_package(Threads REQUIRED)
target_link_libraries(maque_tracy_client PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
if(WIN32)
    target_link_libraries(maque_tracy_client PUBLIC ws2_32 dbghelp)
endif()
