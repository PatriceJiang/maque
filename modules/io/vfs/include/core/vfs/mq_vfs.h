/* 虚拟文件系统（docs/04）：挂载点 + 多协议后端
 * ⚠️ 声明**由 DSL 生成**（docs/21 §1「引擎的一切都在 schema.yaml 声明一次」）：
 *   单一真相 = modules/core/data/schema.yaml 的 `module_abi` 段；
 *   生成物   = build/modules/core/data/generated/abi/vfs.h（与本文曾经的手写内容逐条等价）。
 * 本文件保留为稳定包含路径（消费者无需改动）。改接口请改 schema.yaml，不要改这里。 */
#pragma once
#include "abi/vfs.h"
