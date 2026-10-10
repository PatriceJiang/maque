# maque::vfs —— 虚拟文件系统（挂载 + file:// 后端）
add_library(maque_vfs STATIC src/vfs.cpp)
add_library(maque::vfs ALIAS maque_vfs)
target_include_directories(maque_vfs PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
set_target_properties(maque_vfs PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)

# libcurl：URL(http/https/file) 抓取
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
    target_link_libraries(maque_vfs_test PRIVATE maque::vfs)
    if(CURL_FOUND)
        target_compile_definitions(maque_vfs_test PRIVATE MQ_HAVE_CURL)
    endif()
    add_test(NAME vfs COMMAND maque_vfs_test)
endif()
