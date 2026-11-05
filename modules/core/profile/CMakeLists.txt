# maque::profile —— Linux 采样剖析（perf）封装（docs/35）。
#
# 两种构建形态：
#   · MAQUE_PROFILE=ON（Linux 默认）：src/profile.cpp 编译真实实现（perf 子进程 + report 解析）；
#   · 其它平台 / OFF：同一份源文件编译为 stub（MQ_PROFILE_ENABLED=0），调用方不需要 #if，
#     `supported()==false` 且 start/stop 返回可读原因。
#
# 本目标**不链接**任何第三方库，也不引入线程/信号处理器；`perf record` 是外部子进程。
if(MAQUE_PROFILE)
    set(MAQUE_PROFILE_DEF 1)
else()
    set(MAQUE_PROFILE_DEF 0)
endif()

add_library(maque_profile STATIC src/profile.cpp)
add_library(maque::profile ALIAS maque_profile)
target_include_directories(maque_profile PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(maque_profile PUBLIC MQ_PROFILE_ENABLED=${MAQUE_PROFILE_DEF})
target_compile_features(maque_profile PUBLIC cxx_std_23)

if(MAQUE_BUILD_TESTS)
    add_executable(maque_profile_test tests/profile_test.cpp)
    maque_deploy_asan(maque_profile_test)
    target_link_libraries(maque_profile_test PRIVATE maque::profile)
    add_test(NAME profile COMMAND maque_profile_test)
endif()
