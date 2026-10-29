# maque_web -- ES module + WASM + preloaded P0 scene bundle.
add_executable(maque_web main.cpp)
target_link_libraries(maque_web PRIVATE maque::runtime)
set_target_properties(maque_web PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    SUFFIX ".mjs")

set(MAQUE_WEB_EXPORTS "['_mq_host_scene_create','_mq_host_scene_destroy','_mq_host_step','_mq_host_tick','_mq_host_size','_mq_host_push_pointer','_mq_host_push_key','_mq_host_frame','_mq_host_checksum','_mq_host_should_quit','_mq_host_control','_malloc','_free']")
set(MAQUE_WEB_RUNTIME_METHODS "['FS','UTF8ToString','stringToUTF8','HEAPU8']")

target_link_options(maque_web PRIVATE
    "-sMODULARIZE=1"
    "-sEXPORT_ES6=1"
    "-sEXPORT_NAME=createMaque"
    "-sENVIRONMENT=web,node"
    "-sNO_EXIT_RUNTIME=1"
    "-sALLOW_MEMORY_GROWTH=1"
    "-sWASM_BIGINT=1"
    "-sFORCE_FILESYSTEM=1"
    "-sSTACK_SIZE=1048576"
    "-sEXPORTED_FUNCTIONS=${MAQUE_WEB_EXPORTS}"
    "-sEXPORTED_RUNTIME_METHODS=${MAQUE_WEB_RUNTIME_METHODS}"
    "--preload-file"
    "${CMAKE_SOURCE_DIR}/scenes/anim@/scenes/anim")

file(GLOB MAQUE_WEB_P0_ASSETS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/scenes/anim/*")
set_property(TARGET maque_web APPEND PROPERTY LINK_DEPENDS ${MAQUE_WEB_P0_ASSETS})

add_custom_command(TARGET maque_web POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/index.html"
            "$<TARGET_FILE_DIR:maque_web>/index.html"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/bootstrap.mjs"
            "$<TARGET_FILE_DIR:maque_web>/bootstrap.mjs"
    VERBATIM)
