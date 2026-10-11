# maque::physics —— 确定性 2D 刚体/碰撞（C ABI vtable + 世代句柄，docs/15 §3）
add_library(maque_physics STATIC src/physics.cpp)
add_library(maque::physics ALIAS maque_physics)
target_include_directories(maque_physics PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
set_target_properties(maque_physics PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
# 确定性（docs/10 D8）：禁 FMA 收缩、禁 -ffast-math；求解器仅用普通 float 运算。
target_compile_options(maque_physics PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-ffp-contract=off>)

if(MAQUE_BUILD_TESTS)
    add_executable(maque_physics_test tests/physics_test.cpp)
    target_link_libraries(maque_physics_test PRIVATE maque::physics)
    add_test(NAME physics COMMAND maque_physics_test)
endif()
