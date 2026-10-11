/* 控制平面（docs/11）：JSON-RPC 方法注册 + MCP 包装
 * ⚠️ 声明**由 DSL 生成**（docs/21 §1「引擎的一切都在 schema.yaml 声明一次」）：
 *   单一真相 = modules/core/data/schema.yaml 的 `module_abi` 段；
 *   生成物   = build/modules/core/data/generated/abi/control.h。
 * 本文件保留为稳定包含路径（消费者无需改动）。改接口请改 schema.yaml。 */
#pragma once
#include "abi/control.h"
