/* 平台抽象（docs/05）：窗口/指针/键盘/文本事件 + 帧泵
 * ⚠️ 声明**由 DSL 生成**（docs/21 §1「引擎的一切都在 schema.yaml 声明一次」）：
 *   单一真相 = modules/core/data/schema.yaml 的 `module_abi` 段；
 *   生成物   = build/modules/core/data/generated/abi/platform.h。
 * 本文件保留为稳定包含路径（消费者无需改动）。改接口请改 schema.yaml。 */
#pragma once
#include "abi/platform.h"
