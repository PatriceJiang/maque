# maque::vfs —— 虚拟文件系统（挂载 + 每协议一个实现目录；docs/04）
#
# 一个协议一个目录（核心 vfs.cpp 只做挂载表 + 最长前缀匹配 + 分发）：
#   src/vfs.cpp              核心 + C ABI 入口（abi/vfs.h）
#   src/vfs_internal.h       模块内部接口（协议后端签名 / 共享类型）
#   src/util.cpp             共享工具（归一化 / scheme / 路径安全 / 缓冲交付）
#   src/file/backend.cpp     file://      本地目录
#   src/mem/backend.cpp      mem://       内存命名空间
#   src/zip/backend.cpp      zip://       归档（stored）
#   src/url/backend.cpp      http(s)://   libcurl 抓取 + 会话缓存
#   src/builtin/backend.cpp  builtin:     引擎内建资源
add_library(maque_vfs STATIC
    src/vfs.cpp
    src/util.cpp
    src/file/backend.cpp
    src/mem/backend.cpp
    src/zip/backend.cpp
    src/url/backend.cpp
    src/builtin/backend.cpp)
add_library(maque::vfs ALIAS maque_vfs)
target_include_directories(maque_vfs PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(maque_vfs PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
set_target_properties(maque_vfs PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
target_link_libraries(maque_vfs PUBLIC maque::tracy)   # Tracy 埋点（docs/29；MAQUE_TRACY=OFF 时是空接口）

# libcurl：URL(http/https/file) 抓取 + http(s) 目录挂载（docs/04 §4）
find_package(CURL QUIET)
if(CURL_FOUND)
    target_link_libraries(maque_vfs PUBLIC CURL::libcurl)
    target_compile_definitions(maque_vfs PRIVATE MQ_HAVE_CURL)
    message(STATUS "maque: vfs curl enabled (${CURL_VERSION_STRING})")
else()
    message(WARNING "maque: libcurl not found; vfs URL fetch disabled")
endif()

if(MAQUE_BUILD_TESTS)
    add_executable(maque_vfs_test tests/vfs_test.cpp)
maque_deploy_asan(maque_vfs_test)   # Windows：ASan 运行时 DLL 放到 exe 旁
    target_link_libraries(maque_vfs_test PRIVATE maque::vfs)
    if(CURL_FOUND)
        target_compile_definitions(maque_vfs_test PRIVATE MQ_HAVE_CURL)
    endif()
    if(UNIX)
        # 单测内建极简 HTTP 服务器（mount http:// 用例不依赖外部网络）
        target_compile_definitions(maque_vfs_test PRIVATE MQ_TEST_HTTP)
    endif()
    add_test(NAME vfs COMMAND maque_vfs_test)
endif()

