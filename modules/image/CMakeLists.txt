# maque::image —— 图像解码（Wuffs 单文件）
add_library(maque_image STATIC src/image.c)
add_library(maque::image ALIAS maque_image)
target_include_directories(maque_image PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/third_party/wuffs")
set_target_properties(maque_image PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

if(MAQUE_BUILD_TESTS)
    add_executable(maque_image_test tests/image_test.cpp)
    target_link_libraries(maque_image_test PRIVATE maque::image)
    add_test(NAME image COMMAND maque_image_test)
endif()
target_include_directories(maque_image PUBLIC "${CMAKE_BINARY_DIR}/modules/core/data/generated")   # DSL 生成的 ABI 头（docs/21）
target_include_directories(maque_image_test PUBLIC "${CMAKE_BINARY_DIR}/modules/core/data/generated")   # DSL 生成的 ABI 头（docs/21）
