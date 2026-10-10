// 网络(libwebsockets) 单元测试（context 生命周期）
#include "core/net/mq_net.h"

#include <cstdio>

int main() {
    mq_net* n = mq_net_create();
    if (!n) { std::printf("net_test: FAIL create\n"); return 1; }
    mq_net_destroy(n);
    std::printf("net_test: all checks passed\n");
    return 0;
}
