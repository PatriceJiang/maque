# samples/tank-native —— 《坦克大战》native 迁移切片（plans/26-cpp-scripts.md）
#
# 与 samples/tank 共用同一套 .scn / prefab / 美术 / 音频；根脚本改为 C++ guest，
# HUD 仍是独立 Lua 场景。guest 只链接生成头 + SDK，不链接引擎库。
#
# 文件名按请求保留为 tank-nativel.scn。

maque_add_native_script(tank_native_game SOURCE game_native.cpp)

# 静态 guest：编进一个复用 samples/player/main.cpp 的宿主，不产生/加载 .so。
# （Android/iOS 的生产宿主同样把 $<TARGET_OBJECTS:tank_native_static> 加进去即可。）
if(MAQUE_ENABLE_NATIVE AND TARGET maque_player AND TARGET maque::module)
    maque_add_native_script_static(tank_native_static
        SOURCE game_native.cpp
        KEY tank_native_game
        ID tank_native_game)
    add_executable(tank_native_static_player
        "${CMAKE_SOURCE_DIR}/samples/player/main.cpp"
        $<TARGET_OBJECTS:tank_native_static>)
    target_link_libraries(tank_native_static_player PRIVATE maque::runtime)
    if(COMMAND maque_deploy_plugin_deps)
        maque_deploy_plugin_deps(tank_native_static_player)
    endif()
endif()

if(MAQUE_BUILD_TESTS AND TARGET maque_player AND MAQUE_ENABLE_NATIVE AND TARGET maque::module)
    set(_tank_native_manifest "${CMAKE_CURRENT_BINARY_DIR}/manifest.json")
    file(GENERATE OUTPUT "${_tank_native_manifest}" CONTENT
"{
  \"api\": 1,
  \"target\": \"${MAQUE_TARGET_KEY}\",
  \"scripts\": {
    \"game_native.cpp\": {
      \"artifact\": \"$<TARGET_FILE_NAME:tank_native_game>\",
      \"kind\": \"shared\"
    }
  }
}
")
    add_test(NAME tank_native_smoke COMMAND "${CMAKE_COMMAND}" -E env
        "MAQUE_NATIVE_MANIFEST=${_tank_native_manifest}"
        "MAQUE_TANK_SKIP_TITLE=1"
        "MAQUE_WINDOW=0x0"
        "MAQUE_PRESENT=off"
        "MAQUE_MAX_FRAMES=240"
        $<TARGET_FILE:maque_player> "${CMAKE_CURRENT_SOURCE_DIR}/tank-nativel.scn")
    set_tests_properties(tank_native_smoke PROPERTIES
        PASS_REGULAR_EXPRESSION "player spawn x=" TIMEOUT 120)

    # 标题 → 点击 → 动态 stage → intro → play（与 Lua 版 tank_smoke 同一条路径）
    add_test(NAME tank_native_title COMMAND "${CMAKE_COMMAND}" -E env
        "MAQUE_NATIVE_MANIFEST=${_tank_native_manifest}"
        "MAQUE_SIM_CLICK=20:320:240"
        "MAQUE_WINDOW=0x0"
        "MAQUE_PRESENT=off"
        "MAQUE_MAX_FRAMES=360"
        $<TARGET_FILE:maque_player> "${CMAKE_CURRENT_SOURCE_DIR}/tank-nativel.scn")
    set_tests_properties(tank_native_title PROPERTIES
        PASS_REGULAR_EXPRESSION "stage 1 play" TIMEOUT 120)

    # 输入路径：W 移动 + 空格开火（输入模拟器与真实设备事件同一条路径）
    add_test(NAME tank_native_input COMMAND "${CMAKE_COMMAND}" -E env
        "MAQUE_NATIVE_MANIFEST=${_tank_native_manifest}"
        "MAQUE_TANK_SKIP_TITLE=1"
        "MAQUE_SIM=${CMAKE_CURRENT_SOURCE_DIR}/tank_native_input.sim.json"
        "MAQUE_WINDOW=0x0"
        "MAQUE_PRESENT=off"
        "MAQUE_MAX_FRAMES=260"
        $<TARGET_FILE:maque_player> "${CMAKE_CURRENT_SOURCE_DIR}/tank-nativel.scn")
    set_tests_properties(tank_native_input PROPERTIES
        PASS_REGULAR_EXPRESSION "player fire dir=u" TIMEOUT 120)

    # 用原 Lua 版的同一套 e2e（autoplay / rules 探针）跑 native 根脚本。
    add_test(NAME tank_native_play COMMAND "${CMAKE_COMMAND}" -E env
        "MAQUE_NATIVE_MANIFEST=${_tank_native_manifest}"
        "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/tank_e2e.sh"
        play $<TARGET_FILE:maque_player> "${CMAKE_CURRENT_SOURCE_DIR}/tank-nativel.scn")
    add_test(NAME tank_native_rules COMMAND "${CMAKE_COMMAND}" -E env
        "MAQUE_NATIVE_MANIFEST=${_tank_native_manifest}"
        "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/tank_e2e.sh"
        rules $<TARGET_FILE:maque_player> "${CMAKE_CURRENT_SOURCE_DIR}/tank-nativel.scn")
    add_test(NAME tank_native_e2e_smoke COMMAND "${CMAKE_COMMAND}" -E env
        "MAQUE_NATIVE_MANIFEST=${_tank_native_manifest}"
        "${MAQUE_BASH}" "${CMAKE_SOURCE_DIR}/tests/tank_e2e.sh"
        smoke $<TARGET_FILE:maque_player> "${CMAKE_CURRENT_SOURCE_DIR}/tank-nativel.scn")
    set_tests_properties(tank_native_play tank_native_rules tank_native_e2e_smoke PROPERTIES
        SKIP_RETURN_CODE 77 SKIP_REGULAR_EXPRESSION "SKIP" TIMEOUT 240)
endif()

# ---- 静态 guest 回归（同一 guest 源码，不加载 .so）----
if(MAQUE_BUILD_TESTS AND TARGET tank_native_static_player)
    set(_tank_native_static_manifest "${CMAKE_CURRENT_BINARY_DIR}/manifest-static.json")
    file(GENERATE OUTPUT "${_tank_native_static_manifest}" CONTENT
"{
  \"api\": 1,
  \"target\": \"${MAQUE_TARGET_KEY}\",
  \"scripts\": {
    \"game_native.cpp\": {
      \"kind\": \"static\",
      \"key\": \"tank_native_game\"
    }
  }
}
")
    add_test(NAME tank_native_static COMMAND "${CMAKE_COMMAND}" -E env
        "MAQUE_NATIVE_MANIFEST=${_tank_native_static_manifest}"
        "MAQUE_NATIVE_INFO=1"
        "MAQUE_TANK_SKIP_TITLE=1"
        "MAQUE_WINDOW=0x0"
        "MAQUE_PRESENT=off"
        "MAQUE_MAX_FRAMES=120"
        $<TARGET_FILE:tank_native_static_player> "${CMAKE_CURRENT_SOURCE_DIR}/tank-nativel.scn")
    set_tests_properties(tank_native_static PROPERTIES
        PASS_REGULAR_EXPRESSION "static:tank_native_game" TIMEOUT 120)

    add_test(NAME tank_native_static_smoke COMMAND "${CMAKE_COMMAND}" -E env
        "MAQUE_NATIVE_MANIFEST=${_tank_native_static_manifest}"
        "MAQUE_TANK_SKIP_TITLE=1"
        "MAQUE_WINDOW=0x0"
        "MAQUE_PRESENT=off"
        "MAQUE_MAX_FRAMES=240"
        $<TARGET_FILE:tank_native_static_player> "${CMAKE_CURRENT_SOURCE_DIR}/tank-nativel.scn")
    set_tests_properties(tank_native_static_smoke PROPERTIES
        PASS_REGULAR_EXPRESSION "player spawn x=" TIMEOUT 120)
endif()
