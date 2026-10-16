# maque::image —— 图像解码（Wuffs 单文件）
add_library(maque_image STATIC src/image.c src/png_write.c)
add_library(maque::image ALIAS maque_image)
target_include_directories(maque_image PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/third_party/wuffs")
set_target_properties(maque_image PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

# zlib：PNG 写出（截图/预览）真压缩；缺失时自动退化为 stored deflate（仍是合法 PNG）
find_package(ZLIB QUIET)
if(ZLIB_FOUND)
    target_link_libraries(maque_image PUBLIC ZLIB::ZLIB)
    target_compile_definitions(maque_image PRIVATE MQ_HAVE_ZLIB)
    message(STATUS "maque: image png-write zlib enabled (${ZLIB_VERSION_STRING})")
else()
    message(WARNING "maque: zlib not found; PNG 写出退化为 stored（不压缩）")
endif()

if(MAQUE_BUILD_TESTS)
    add_executable(maque_image_test tests/image_test.cpp)
maque_deploy_asan(maque_image_test)   # Windows：ASan 运行时 DLL 放到 exe 旁
    target_link_libraries(maque_image_test PRIVATE maque::image)
    add_test(NAME image COMMAND maque_image_test)
endif()
target_include_directories(maque_image PUBLIC "${MAQUE_GEN_DIR}")   # DSL 生成的 ABI 头（docs/21）
target_include_directories(maque_image_test PUBLIC "${MAQUE_GEN_DIR}")   # DSL 生成的 ABI 头（docs/21）
