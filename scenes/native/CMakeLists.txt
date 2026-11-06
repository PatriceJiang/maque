# native 脚本样例：只编 guest；manifest 与 e2e 在 samples/player 里接线。
# guest 不链接引擎库；API 表/guest vtable 由 codegen 生成。
maque_add_native_script(maque_native_hello SOURCE scripts/hello.cpp)
