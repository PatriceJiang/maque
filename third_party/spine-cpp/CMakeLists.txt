# spine-cpp —— Spine 2D 官方 C++ 运行时（vendored）
#
# 上游：https://github.com/EsotericSoftware/spine-runtimes（分支 4.3 的 spine-cpp/）
# 许可：Spine Runtimes License Agreement（见本目录 LICENSE）——**不是** MIT/BSD。
#   集成允许，但再分发必须随附该许可与版权声明；使用者需自备 Spine Editor 许可。
#   README 与本目录同步说明这一点，勿删 LICENSE。
#
# 为什么不用上游的 CMakeLists：它 include 了上级目录的 flags.cmake（不在 vendored 范围内），
# 且会给全局 CMAKE_CXX_FLAGS 追加 -std=c++11 -fno-exceptions -fno-rtti（影响其它目标）。
# 这里改成 target 级设置，只作用于 spine-cpp 自己（与 third_party/lua 的做法一致）。
file(GLOB SPINE_CPP_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/spine/*.cpp")

add_library(spine-cpp STATIC ${SPINE_CPP_SOURCES})
add_library(Spine::spine-cpp ALIAS spine-cpp)

target_include_directories(spine-cpp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

# 上游要求：无异常、无 RTTI（其代码在异常被禁用时才启用 -fno-exceptions 路径）。
# 我们用 target 级编译选项实现，避免污染其它目标。
set_target_properties(spine-cpp PROPERTIES
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

if(MSVC)
    # clang-cl / MSVC：关异常与 RTTI；CRT 安全警告（上游 flags.cmake 同样处理）
    target_compile_options(spine-cpp PRIVATE /EHs-c- /GR- /D_CRT_SECURE_NO_WARNINGS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(spine-cpp PRIVATE -fno-exceptions -fno-rtti)
    endif()
    # 第三方代码：不把它的警告当我们的问题（但保留错误）
    target_compile_options(spine-cpp PRIVATE /W0)
else()
    target_compile_options(spine-cpp PRIVATE -fno-exceptions -fno-rtti -w)
endif()

# 第三方代码不开 ASan 之外的项目级告警策略；ASan 由全局 MAQUE_SANITIZE 决定，无需额外处理
