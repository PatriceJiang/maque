/* 运行时入口（"浏览器"）：mq_run_scene
 * ⚠️ 声明**由 DSL 生成**（docs/21 §1「引擎的一切都在 schema/ 声明一次」）：
 *   单一真相 = modules/core/data/schema/ 的 `module_abi` 段；
 *   生成物   = build/modules/core/data/generated/abi/runtime.h。
 * 本文件保留为稳定包含路径（消费者无需改动）。改接口请改 schema/。 */
#pragma once
#include "abi/runtime.h"
