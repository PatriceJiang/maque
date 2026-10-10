#include "core/net/mq_net.h"

#include <libwebsockets.h>
#include <cstring>

struct mq_net {
    struct lws_context* ctx = nullptr;
};

static int maque_cb(struct lws* wsi, enum lws_callback_reasons reason,
                    void* user, void* in, size_t len) {
    (void)wsi; (void)reason; (void)user; (void)in; (void)len;
    return 0;
}

static const struct lws_protocols k_protocols[] = {
    { "maque", maque_cb, 0, 0, 0, nullptr, 0 },
    LWS_PROTOCOL_LIST_TERM
};

extern "C" {

mq_net* mq_net_create(void) {
    struct lws_context_creation_info info;
    std::memset(&info, 0, sizeof(info));
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = k_protocols;
    auto* n = new mq_net();
    n->ctx = lws_create_context(&info);
    return n;
}

void mq_net_destroy(mq_net* n) {
    if (!n) return;
    if (n->ctx) lws_context_destroy(n->ctx);
    delete n;
}

} // extern "C"
