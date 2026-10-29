# maque::unibreak —— vendored libunibreak（UAX #14 换行 + UAX #29 字素边界；zlib 许可）
# 只编译 linebreak / graphemebreak 及其数据表（不含 tools/测试）。
add_library(maque_unibreak STATIC
    linebreak.c
    linebreakdata.c
    linebreakdef.c
    unibreakbase.c
    unibreakdef.c
    eastasianwidthdata.c
    eastasianwidthdef.c
    graphemebreak.c
    graphemebreakdata.c
    emojidef.c
    emojidata.c
    indicconjunctbreakdata.c)
add_library(maque::unibreak ALIAS maque_unibreak)
target_include_directories(maque_unibreak PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(maque_unibreak PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON)
