# maque::asset —— 资产管线（导入 / 依赖 / 烘焙 / 打包 / dump；docs/08、plans/08）
# 纯 C ABI 公共头（C11 干净）；实现为 C++23；只依赖 VFS（读源内容），JSON 自建（不依赖 persist）。
add_library(maque_asset STATIC src/asset.cpp)
add_library(maque::asset ALIAS maque_asset)
target_include_directories(maque_asset PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(maque_asset PUBLIC maque::vfs)
set_target_properties(maque_asset PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)

if(MAQUE_BUILD_TESTS)
    add_executable(maque_asset_test tests/asset_test.cpp)
maque_deploy_asan(maque_asset_test)   # Windows：ASan 运行时 DLL 放到 exe 旁
    target_link_libraries(maque_asset_test PRIVATE maque::asset)
    add_test(NAME asset COMMAND maque_asset_test)
endif()

