# spine-cpp —— Spine 2D 官方 C++ 运行时（vendored）
#
# 上游：https://github.com/EsotericSoftware/spine-runtimes（**4.2 分支**的 spine-cpp/）
# 许可：Spine Runtimes License Agreement（见本目录 LICENSE）——**不是** MIT/BSD。
#   集成允许，但再分发必须随附该许可与版权声明；使用者需自备 Spine Editor 许可。勿删 LICENSE。
#
# 为什么 4.2 而不是 4.3：4.3 把附件内部结构收成 friend-only（RegionAttachment 不再有
#   getRegion()/getUVs()、computeWorldVertices 要外部传 vertexOffsets），姿势数值搬到 *Pose 类，
#   约束收进统一 getConstraints()；4.2 的公开面与 docs/assets/06 §2 一致，胶水最少。
#
# 编译要求（本项目决定，见 docs/27）：
#   · **打开 C++ RTTI**（不传 -fno-rtti / /GR-）：封装层用 dynamic_cast 判别附件类型。
#   · 仍关闭异常（-fno-exceptions）：spine 用 getError() 返回错误，不靠异常。
#   · 不采用上游的 CMakeLists：它 include 上级目录的 flags.cmake（不在 vendored 范围内），
#     并会给全局 CMAKE_CXX_FLAGS 追加 -std=c++11 -fno-exceptions -fno-rtti（污染其它目标）。
file(GLOB SPINE_CPP_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/spine/*.cpp")

add_library(spine-cpp STATIC ${SPINE_CPP_SOURCES})
add_library(Spine::spine-cpp ALIAS spine-cpp)
target_include_directories(spine-cpp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

set_target_properties(spine-cpp PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

if(MSVC)
    target_compile_options(spine-cpp PRIVATE /EHs-c- /D_CRT_SECURE_NO_WARNINGS /W0)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(spine-cpp PRIVATE -fno-exceptions)
    endif()
else()
    target_compile_options(spine-cpp PRIVATE -fno-exceptions -w)
endif()