/* 声明式引擎接口的脚本绑定入口（docs/21 §3）。
 * ⚠️ 声明**由 DSL 生成**：单一真相 = modules/core/data/schema.yaml 的 `api_list`；
 *   生成物 = build/modules/core/data/generated/maque_bind.h。
 * 本文件保留为稳定包含路径（消费者无需改动）。 */
#pragma once
#include "maque_bind.h"
