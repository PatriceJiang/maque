#include "abi/control.h"

struct mq_control_server {};

extern "C" {

mq_control_server* mq_control_server_create(mq_control*, const char*, int) { return nullptr; }
int mq_control_server_poll(mq_control_server*) { return 0; }
uint32_t mq_control_server_clients(mq_control_server*) { return 0; }
const char* mq_control_server_url(mq_control_server*) { return ""; }
uint32_t mq_control_server_rejected(mq_control_server*) { return 0; }
void mq_control_server_destroy(mq_control_server*) {}

} // extern "C"
