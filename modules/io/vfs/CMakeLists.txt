# maque::vfs —— 虚拟文件系统（挂载 + file:// 后端）
add_library(maque_vfs STATIC src/vfs.cpp)
add_library(maque::vfs ALIAS maque_vfs)
target_include_directories(maque_vfs PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
set_target_properties(maque_vfs PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)

if(MAQUE_BUILD_TESTS)
    add_executable(maque_vfs_test tests/vfs_test.cpp)
    target_link_libraries(maque_vfs_test PRIVATE maque::vfs)
    add_test(NAME vfs COMMAND maque_vfs_test)
endif()
