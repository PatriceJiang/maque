/* 测试用接口（宿主与测试模块共享）。 */
#pragma once
#include "core/module/mq_module.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mq_test_api_v1 {
    mq_api_header header;                 /* 必须首字段 */
    int (*add)(int a, int b);
    const char* (*greet)(void);
} mq_test_api_v1;

#define MQ_TEST_API_NAME "maque.test"

#ifdef __cplusplus
}
#endif
