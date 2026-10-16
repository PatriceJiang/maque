# maque::storage —— 键值存储（浏览器 localStorage 类比；docs/25）
#
# 模式（spec 的 scheme）→ 一个后端目录：
#   src/storage.cpp        核心：spec 解析 / (uid,session) 命名空间 / 键校验 / 分发 + C ABI
#   src/storage_internal.h 模块内部接口（Request/Response、后端签名、共享工具）
#   src/util.cpp           共享：段/键校验、键 → 路径 / URL、极简 JSON、base64、UTF-8 校验
#   src/file/backend.cpp   file://    本地目录（原子写 tmp+rename；默认 <场景目录>/.maque）
#   src/mem/backend.cpp    mem://     进程内（默认模式）
#   src/http/backend.cpp   http(s):// REST：GET/PUT/DELETE <base>/<ns>/<key> + GET <base>/<ns>?keys=1
#   src/ws/backend.cpp     ws(s)://   文本帧 JSON（libwebsockets；协议名 maque.storage.v1）
add_library(maque_storage STATIC
    src/storage.cpp
    src/util.cpp
    src/file/backend.cpp
    src/mem/backend.cpp
    src/http/backend.cpp
    src/ws/backend.cpp)
add_library(maque::storage ALIAS maque_storage)
target_include_directories(maque_storage PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(maque_storage PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
set_target_properties(maque_storage PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)

# libcurl：http(s):// 后端（缺省 → 该模式返回 UNSUPPORTED，不静默降级）
find_package(CURL QUIET)
if(CURL_FOUND)
    target_link_libraries(maque_storage PUBLIC CURL::libcurl)
    target_compile_definitions(maque_storage PRIVATE MQ_HAVE_CURL)
    message(STATUS "maque: storage http(s) enabled (${CURL_VERSION_STRING})")
else()
    message(WARNING "maque: libcurl not found; storage http(s):// 后端关闭")
endif()

# libwebsockets：ws(s):// 后端
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LWS QUIET libwebsockets)
endif()
if(LWS_FOUND)
    target_include_directories(maque_storage PRIVATE ${LWS_INCLUDE_DIRS})
    target_link_directories(maque_storage PUBLIC ${LWS_LIBRARY_DIRS})
    target_link_libraries(maque_storage PUBLIC ${LWS_LIBRARIES})
    target_compile_definitions(maque_storage PRIVATE MQ_HAVE_LWS)
else()
    message(WARNING "maque: libwebsockets not found; storage ws(s):// 后端关闭")
endif()

if(MAQUE_BUILD_TESTS)
    add_executable(maque_storage_test tests/storage_test.cpp)
maque_deploy_asan(maque_storage_test)   # Windows：ASan 运行时 DLL 放到 exe 旁
    target_link_libraries(maque_storage_test PRIVATE maque::storage)
    # 测试内建 HTTP 服务端在 Windows 走 Winsock2（storage_test.cpp 的平台兼容段）。
    if(WIN32)
        target_link_libraries(maque_storage_test PRIVATE ws2_32)
    endif()
    if(LWS_FOUND)
        # 单测内起一个最小 lws 服务端做 ws(s) e2e（不需要外部服务）
        target_link_directories(maque_storage_test PRIVATE ${LWS_LIBRARY_DIRS})
        target_include_directories(maque_storage_test PRIVATE ${LWS_INCLUDE_DIRS})
        target_link_libraries(maque_storage_test PRIVATE ${LWS_LIBRARIES})
        target_compile_definitions(maque_storage_test PRIVATE MQ_HAVE_LWS)
    endif()
    add_test(NAME storage COMMAND maque_storage_test)
endif()
