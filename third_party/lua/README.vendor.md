# third_party/lua — 官方 Lua 5.4.8（vendored）

- 来源：https://www.lua.org/ftp/lua-5.4.8.tar.gz （未修改，仅移除解释器/编译器的 `main`：`lua.c` / `luac.c`）
- 用途：`modules/script/lua` 的 Lua 脚本后端（docs/14「统一后端 ABI」的可选后端）
- 许可：MIT（见 `LICENSE`、`src/lua.h` 的版权声明）
- 构建：`CMakeLists.txt` 导出静态库 `maque_lua`（C11，不含 readline/解释器）
