# maque::math —— 纯函数数学库（header-only，操作 codegen 生成的 POD 类型）
add_library(maque_math INTERFACE)
add_library(maque::math ALIAS maque_math)
target_include_directories(maque_math INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${MAQUE_GEN_DIR}")   # abi/math.h（DSL 生成，docs/21）
target_link_libraries(maque_math INTERFACE maque::schema)
target_compile_features(maque_math INTERFACE cxx_std_23)

if(MAQUE_BUILD_TESTS)
    add_executable(maque_math_test tests/math_test.cpp)
    target_link_libraries(maque_math_test PRIVATE maque::math)
    add_test(NAME math COMMAND maque_math_test)
endif()
