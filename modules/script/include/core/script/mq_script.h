/* 脚本 VM（docs/14，quickjs）：求值/回调/注册原语
 * ⚠️ 声明**由 DSL 生成**（docs/21 §1「引擎的一切都在 schema/ 声明一次」）：
 *   单一真相 = modules/core/data/schema/ 的 `module_abi` 段；
 *   生成物   = build/modules/core/data/generated/abi/script.h。
 * 本文件保留为稳定包含路径（消费者无需改动）。改接口请改 schema/。 */
#pragma once
#include "abi/script.h"
