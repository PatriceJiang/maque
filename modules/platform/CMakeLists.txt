# maque::platform —— 平台层（SDL2）：窗口 + 输入
find_path(SDL2_INCLUDE_DIR SDL.h PATH_SUFFIXES SDL2)
find_library(SDL2_LIBRARY NAMES SDL2)

if(SDL2_INCLUDE_DIR AND SDL2_LIBRARY)
    add_library(maque_platform STATIC src/platform.cpp)
    add_library(maque::platform ALIAS maque_platform)
    target_include_directories(maque_platform PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include" "${SDL2_INCLUDE_DIR}")
    target_link_libraries(maque_platform PUBLIC "${SDL2_LIBRARY}")
    set_target_properties(maque_platform PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)

    if(MAQUE_BUILD_TESTS)
        add_executable(maque_platform_test tests/platform_test.cpp)
        target_link_libraries(maque_platform_test PRIVATE maque::platform)
        add_test(NAME platform COMMAND maque_platform_test)
    endif()
else()
    message(WARNING "maque: SDL2 not found; platform 模块关闭")
endif()
