/* maque 网络（libwebsockets，docs/11）：WebSocket 客户端底座（供控制平面/远程）。
 * 最小切片：context 生命周期；后续加 connect/send/service。 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mq_net mq_net;

mq_net* mq_net_create(void);
void    mq_net_destroy(mq_net* net);

#ifdef __cplusplus
}
#endif
