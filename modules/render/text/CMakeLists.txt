# maque::text —— 文本渲染基础设施（FreeType 字体 → 图集 → 排版 + 富文本标记）
add_library(maque_text STATIC src/text.cpp)
add_library(maque::text ALIAS maque_text)
target_include_directories(maque_text PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
set_target_properties(maque_text PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
target_link_libraries(maque_text PUBLIC maque::tracy)   # Tracy 埋点（docs/29；MAQUE_TRACY=OFF 时是空接口）

# 可选依赖：系统 FreeType（缺失时回退到内置 5x7 位图字体）
find_package(Freetype QUIET)
if(FREETYPE_FOUND)
    target_link_libraries(maque_text PUBLIC Freetype::Freetype)
    target_compile_definitions(maque_text PUBLIC MAQUE_HAVE_FREETYPE=1)
    message(STATUS "maque::text: FreeType ${FREETYPE_VERSION_STRING} 已启用")
else()
    message(STATUS "maque::text: 未找到 FreeType，使用内置 5x7 位图字体")
endif()

# 可选依赖：HarfBuzz（GPOS kerning / 连字 / 复杂脚本 shaping；缺失时回退逐码点布局）
option(MAQUE_HARFBUZZ "启用 HarfBuzz 文本 shaping（缺少时自动回退逐码点布局）" ON)
if(MAQUE_HARFBUZZ AND FREETYPE_FOUND)
    # 不走 harfbuzzConfig.cmake：Windows 下它把 prefix 多算了几层，INTERFACE_INCLUDE_DIRECTORIES
    # 会变成 "/harfbuzz"。这里直接找库/头，.deps/dist 由顶层 CMAKE_PREFIX_PATH 覆盖。
    find_path(MAQUE_HARFBUZZ_INCLUDE_DIR NAMES harfbuzz/hb.h PATH_SUFFIXES include)
    find_library(MAQUE_HARFBUZZ_LIBRARY NAMES harfbuzz harfbuzz.lib PATH_SUFFIXES lib)
    if(MAQUE_HARFBUZZ_INCLUDE_DIR AND MAQUE_HARFBUZZ_LIBRARY)
        if(NOT TARGET harfbuzz::harfbuzz)
            add_library(harfbuzz::harfbuzz UNKNOWN IMPORTED)
            set_target_properties(harfbuzz::harfbuzz PROPERTIES
                IMPORTED_LOCATION "${MAQUE_HARFBUZZ_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${MAQUE_HARFBUZZ_INCLUDE_DIR}")
        endif()
        set(harfbuzz_FOUND TRUE)
    endif()
endif()
if(MAQUE_HARFBUZZ AND FREETYPE_FOUND AND harfbuzz_FOUND)
    target_link_libraries(maque_text PUBLIC harfbuzz::harfbuzz)
    target_compile_definitions(maque_text PUBLIC MAQUE_HAVE_HARFBUZZ=1)
    message(STATUS "maque::text: HarfBuzz 已启用（shaping；MAQUE_TEXT_SHAPING=0 可运行时关闭）")
elseif(MAQUE_HARFBUZZ AND FREETYPE_FOUND)
    message(STATUS "maque::text: 未找到 HarfBuzz，回退逐码点布局（可用 -DMAQUE_HARFBUZZ=OFF 静默）")
endif()

if(MAQUE_BUILD_TESTS)
    add_executable(maque_text_test tests/text_test.cpp)
maque_deploy_asan(maque_text_test)   # Windows：ASan 运行时 DLL 放到 exe 旁
    target_link_libraries(maque_text_test PRIVATE maque::text)
    add_test(NAME text COMMAND maque_text_test)
endif()
