# maque::text —— 文本渲染基础设施（FreeType 字体 → 图集 → 排版 + 富文本标记）
add_library(maque_text STATIC src/text.cpp)
add_library(maque::text ALIAS maque_text)
target_include_directories(maque_text PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
set_target_properties(maque_text PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)

# 可选依赖：系统 FreeType（缺失时回退到内置 5x7 位图字体）
find_package(Freetype QUIET)
if(FREETYPE_FOUND)
    target_link_libraries(maque_text PUBLIC Freetype::Freetype)
    target_compile_definitions(maque_text PUBLIC MAQUE_HAVE_FREETYPE=1)
    message(STATUS "maque::text: FreeType ${FREETYPE_VERSION_STRING} 已启用")
else()
    message(STATUS "maque::text: 未找到 FreeType，使用内置 5x7 位图字体")
endif()

if(MAQUE_BUILD_TESTS)
    add_executable(maque_text_test tests/text_test.cpp)
maque_deploy_asan(maque_text_test)   # Windows：ASan 运行时 DLL 放到 exe 旁
    target_link_libraries(maque_text_test PRIVATE maque::text)
    add_test(NAME text COMMAND maque_text_test)
endif()
