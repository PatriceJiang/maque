# maque::render —— 渲染器接口（后端无关，header-only）
add_library(maque_render INTERFACE)
add_library(maque::render ALIAS maque_render)
target_include_directories(maque_render INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(maque_render INTERFACE cxx_std_23)
