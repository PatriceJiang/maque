# 01 · math 模块计划

> Owner agent：`agent-math` ｜ 上游：`docs/00`（根）、`docs/06`（反射/布局）、`docs/07`（内存/数据模型）
> 交付顺序：**阶段 A 地基第一块**（`math → core → module host → platform → executor`）。

## 1. 模块概述

- **职责**：提供全引擎共享的**纯值类型数学库**：向量、矩阵、四元数、变换、标量函数、插值、几何辅助。它是 `scene / animation / physics / renderer / particle` 的公共数值语言。
- **所在层**：**基础层**（最底层，无任何模块依赖）。
- **上游文档**：`docs/00 §5`（packed POD + 固定布局）、`docs/06 §4`（schema → C++ POD 头 + `static_assert(sizeof==size)`）、`docs/07 §4/§9`（数据布局、确定性：禁地址依赖）。
- **一句话**：**无状态、无分配、可直接内联的确定性 POD 数学层**，其布局即 ABI。

### 与 schema 的关系（唯一真相）

`modules/core/data/schema.yaml` 的 `namespace: math` 是**布局唯一真相**：
`Vec2(8) / Vec3(12) / Vec4(16) / Mat3(36) / Mat4(64) / Transform(48)`。
生成的 C++ POD 头（`parser.js` 产出）由本模块 `#include`，本模块**只做加法**（补 `Quat`、几何类型与全部操作函数），**不改字段/尺寸**。
> `Quat`、`Aabb`、`Ray`、`Plane` 等需回写 `schema.yaml`（新增条目），由 `codegen` 生成布局；本模块负责其操作面。任何布局变更 → 同步 bump `MQ_MATH_ABI_VERSION`。

---

## 2. 接口层设计（完备，先冻结）

> **与其余模块的差异说明**：math **无实例、无状态、无回调**，因此不需要 vtable 与函数指针表（ADR-0013 的 vtable 用在有生命周期的模块）。
> math 的跨 `.so` 接口面 = **POD 布局（C ABI）+ 内联自由函数**。布局由 `schema.yaml` + `static_assert` 冻结并版本化；操作函数是 `inline noexcept` 头文件实现，跨 `.so` 编译期各模块各得一份，**无 ABI 失配风险**（与 GLM/Eigen 头库同理）。

### 2.1 公共头与版本

```cpp
/* modules/math/include/math/mq_math.h —— 纯 C++23，无 std 于 ABI 面 */
#pragma once
#include <cstdint>
#include "math_types.h"   // codegen 产物：Vec2/Vec3/Vec4/Mat3/Mat4/Transform/Quat 等 POD

#define MQ_MATH_ABI_VERSION 1u   // POD 布局版本；布局变更必须 bump

namespace math {

/* 布局契约：与 schema.yaml 逐字段一致（sizeof 断言） */
static_assert(sizeof(Vec2) == 8);
static_assert(sizeof(Vec3) == 12);
static_assert(sizeof(Vec4) == 16);
static_assert(sizeof(Mat3) == 36);
static_assert(sizeof(Mat4) == 64);
static_assert(sizeof(Transform) == 48);
static_assert(sizeof(Quat) == 16);
static_assert(alignof(Mat4) == 4);          // packed：不假设 SIMD 对齐
static_assert(std::is_trivially_copyable_v<Vec3> && std::is_standard_layout_v<Vec3>);
/* Transform 字段顺序即序列化顺序，不许重排 */
static_assert(offsetof(Transform, position) == 0);
static_assert(offsetof(Transform, rotation) == 12);
static_assert(offsetof(Transform, scale)    == 24);

/* 编译期浮点确定性开关：禁止 FMA 收缩（跨平台一致的前提） */
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
} // namespace math
```

- **命名空间**：`math`（与 schema 一致）。跨模块引用形如 `math::Vec3`。
- **极简 C 视图**（供 codegen 描述符 / 非 C++ 消费者引用类型布局时使用）：
  ```c
  typedef struct mq_math_vec3 { float x, y, z; } mq_math_vec3;   /* sizeof==12 */
  typedef struct mq_math_mat4 { float m[16];    } mq_math_mat4;  /* sizeof==64，列主序 */
  ```
  仅布局别名，不带函数；`static_assert(sizeof(mq_math_vec3)==sizeof(math::Vec3))`。

### 2.2 公开类型（POD，全部 `trivially_copyable` / `standard_layout`）

| 类型 | 尺寸 | 字段（顺序 = 序列化顺序） | 约定 |
|------|------|--------------------------|------|
| `Vec2` | 8 | `x, y` | — |
| `Vec3` | 12 | `x, y, z` | 点或方向由用途决定；**无 4 字节填充** |
| `Vec4` | 16 | `x, y, z, w` | 齐次坐标；`w=0` 方向，`w=1` 点 |
| `Mat3` | 36 | `m[9]` | **列主序**，`m[col*3+row]` |
| `Mat4` | 64 | `m[16]` | **列主序**，`m[col*4+row]`，仿射变换 |
| `Quat` | 16 | `x, y, z, w` | 单位四元数；`w` 为实部；同 Vec4 布局但**独立类型**（防语义混用） |
| `Transform` | 48 | `position:Vec3, rotation:Vec3, scale:Vec3` | `rotation` 为 **Euler 弧度，内旋 Z→Y→X**（见 §2.4） |
| `Aabb` | 24 | `min:Vec3, max:Vec3` | 轴对齐包围盒（**需回写 schema**） |
| `Ray` | 24 | `origin:Vec3, dir:Vec3` | `dir` 约定**已归一化**（调用方保证）（**需回写 schema**） |
| `Plane` | 16 | `normal:Vec3, d:float` | `dot(n,p)+d=0`（**需回写 schema**） |

> `Quat` 与 `Transform` 的 Euler 约定是**跨模块契约**，animation/physics/renderer 一律遵守。

### 2.3 操作面（全部 `[[nodiscard]] inline noexcept`，纯函数，无副作用）

符号：`vN`=向量, `mN`=矩阵, `q`=四元数, `t`=Transform, `s`=标量。`EPS`=`1e-6f`（可配）。

**A. 标量 / 通用**
| 函数 | 语义 |
|------|------|
| `float clamp(float v, float lo, float hi)` | 夹取；`lo>hi` 断言 |
| `float saturate(float v)` | `clamp(v,0,1)` |
| `float lerp(float a, float b, float t)` | `a+(b-a)*t` |
| `float inv_lerp(float a, float b, float v)` | `(v-a)/(b-a)`，`a==b` 返回 0 |
| `float remap(float v, float inLo, float inHi, float outLo, float outHi)` | 区间映射 |
| `float sign(float v)` | -1/0/+1 |
| `float smoothstep(float e0,float e1,float v)` | 平滑插值 |
| `float move_towards(float cur,float target,float maxDelta)` | 限速趋近 |
| `float degrees(float rad)` / `float radians(float deg)` | 角度换算（编译期常量） |
| `bool  roughly_equal(float a,float b,float eps=EPS)` | 近似相等（相对+绝对） |
| `bool  is_finite(float v)` | 非 NaN/Inf 校验 |

**B. 向量（Vec2/Vec3/Vec4 各一套；下面以 Vec3 为例，其余同名重载）**
| 函数 | 语义 |
|------|------|
| `Vec3 add(a,b)` `sub(a,b)` `mul(a,b)` `mul(a,s)` `div(a,s)` `negate(a)` | 逐分量；`mul(a,b)` 为逐分量积 |
| `float dot(a,b)` | 点积 |
| `Vec3 cross(a,b)` | 叉积（Vec3） |
| `float length_sq(v)` `float length(v)` | 长度平方 / 长度（`sqrt`） |
| `Vec3 normalize(v)` | 单位化；`length==0` 调试断言、发布返回 `{0,0,0}`（见 `safe_normalize`） |
| `Vec3 safe_normalize(v, Vec3 fallback)` | 零向量返回 fallback（确定、不 UB） |
| `float distance(a,b)` `float distance_sq(a,b)` | 距离 |
| `Vec3 lerp(a,b,t)` | 线性插值 |
| `Vec3 min(a,b)` `Vec3 max(a,b)` `Vec3 abs(v)` | 逐分量 |
| `Vec3 project(a, onto)` | 投影到 `onto`（onto 须归一化） |
| `Vec3 reflect(v, n)` | 镜面反射（n 归一化） |
| `float angle_between(a,b)` | 夹角（弧度，内部自动归一化） |
| `Vec3 perp(v)` | 任一垂直向量（确定性择轴，见 §确定性） |

**C. 矩阵**
| 函数 | 语义 |
|------|------|
| `Mat4 identity4()` / `Mat3 identity3()` | 单位阵 |
| `Mat4 mul(Mat4,Mat4)` / `Mat3 mul(Mat3,Mat3)` / `Mat3 mul(Mat3,Mat4上调)` | 矩阵乘（列主序） |
| `Vec4 mul(Mat4, Vec4)` | 齐次乘 |
| `Vec3 transform_point(Mat4, Vec3)` | 点变换（补 w=1，透视除） |
| `Vec3 transform_dir(Mat4, Vec3)` | 方向变换（w=0，忽略平移） |
| `Mat4 transpose(Mat4)` / `Mat3 transpose(Mat3)` | 转置 |
| `float determinant(Mat3)` / `float determinant(Mat4)` | 行列式 |
| `Mat4 inverse(Mat4)` | 通用逆；奇异时调试断言、发布返回单位阵 |
| `bool try_inverse(Mat4, Mat4& out)` | 奇异返回 `false`（错误安全路径，见 §2.5） |
| `Mat4 affine_inverse(Mat4)` | 仿射快路径（旋转/平移/缩放） |
| `Mat4 translate(Vec3)` `rotate_x/y/z(float)` `rotate_q(Quat)` `scale(Vec3)` | 构造器 |
| `Mat4 compose_trs(Vec3 t, Quat r, Vec3 s)` | TRS → 矩阵（**推荐路径**） |
| `Mat4 compose_trs(Vec3 t, Vec3 euler, Vec3 s)` | Euler 版（内部转 Quat） |
| `bool decompose(Mat4, Vec3& t, Quat& r, Vec3& s)` | 逆运算；含负缩放/退化检测 |
| `Mat4 perspective(fovY, aspect, near, far)` | 透视投影（右手系，`[0,1]` 深度） |
| `Mat4 ortho(l,r,b,t,n,f)` | 正交投影 |
| `Mat4 look_at(eye, target, up)` | 视图矩阵 |

**D. 四元数**
| 函数 | 语义 |
|------|------|
| `Quat identity_q()` | `{0,0,0,1}` |
| `Quat from_axis_angle(Vec3 axis, float rad)` | axis 须归一化 |
| `Quat from_euler(Vec3 euler)` / `Vec3 to_euler(Quat)` | 与 Transform 的 ZYX 约定互转 |
| `Quat from_mat3(Mat3)` / `Mat3 to_mat3(Quat)` / `Mat4 to_mat4(Quat)` | 互转（数值稳定分支处理） |
| `Quat mul(Quat,Quat)` | 复合 |
| `Quat conjugate(Quat)` / `Quat inverse(Quat)` | 共轭 / 逆（单位四元数逆=共轭，仍做归一化保护） |
| `Quat normalize(Quat)` | 归一化；零四元数返回单位 |
| `float dot(Quat,Quat)` | 点积 |
| `Vec3 rotate(Quat, Vec3)` | 旋转向量（等价 `q v q*`，用高效形式） |
| `Quat slerp(Quat a, Quat b, float t)` | 球面插值；**最短路径**（dot<0 取反）；`t∈[0,1]` 外插仍定义 |
| `Quat nlerp(Quat a, Quat b, float t)` | 归一化线性插值（更快，路径略偏） |
| `Quat look_rotation(Vec3 fwd, Vec3 up)` | 朝向四元数 |
| `Quat from_to_rotation(Vec3 from, Vec3 to)` | 两方向间最短旋转（含近 180° 分支） |

**E. 插值**
| 函数 | 语义 |
|------|------|
| `Vec3 lerp/slerp`（slERP 仅对方向向量：先归一化，按夹角插值，退化线性） | 向量插值 |
| `Quat slerp/nlerp` | 见 D |
| `Transform lerp(Transform a, Transform b, float t)` | 位置/缩放线性、旋转 slerp |
| `Transform move_towards(Transform cur, Transform tgt, float maxDelta)` | 限速 |
| `float damp(float cur,float target,float lambda,float dt)` | 帧率无关阻尼 `lerp(cur,target,1-exp(-lambda*dt))`（用 `det_exp`） |

**F. 几何辅助**
| 函数 | 语义 |
|------|------|
| `Aabb aabb_from_points(const Vec3*, size_t n)` | 点集包围盒 |
| `Aabb aabb_union(Aabb,Aabb)` / `bool aabb_contains(Aabb,Vec3)` / `bool aabb_overlaps(Aabb,Aabb)` | 盒运算 |
| `Vec3 aabb_center/Aabb aabb_expand` | 中心/外扩 |
| `bool ray_aabb(Ray, Aabb, float& tHit)` | 射线-AABB（slab 法，含边界） |
| `bool ray_sphere(Ray, Vec3 c, float r, float& tHit)` | 射线-球 |
| `Plane plane_from_points(Vec3 a, Vec3 b, Vec3 c)` | 三点定面（退化返回 d=NaN，配 `is_finite` 校验） |
| `Plane plane_from_normal_point(Vec3 n, Vec3 p)` | 法线+点定面 |
| `float plane_distance(Plane, Vec3)` | 带符号距离 |
| `Vec3 closest_point_on_segment(Vec3 p, Vec3 a, Vec3 b)` | 线段最近点（`t` 夹取） |
| `Vec3 closest_point_on_ray(Ray, Vec3 p)` | 射线最近点（`t>=0`） |
| `Vec3 triangle_normal(Vec3 a,b,c)` / `float triangle_area(a,b,c)` | 三角形 |
| `Vec3 barycentric(Vec3 p, Vec3 a,b,c)` | 重心坐标（退化未定义，配断言） |

**G. Transform（场景/层级核心）**
| 函数 | 语义 |
|------|------|
| `Transform identity_transform()` | 位置0/旋转0/缩放1 |
| `Mat4 to_mat4(Transform)` | 等价 `compose_trs` |
| `Transform from_mat4(Mat4)` | `decompose` + `to_euler` |
| `Transform compose(Transform parent, Transform child)` | **层级复合**（`parent * child`） |
| `Transform inverse(Transform)` | 逆变换（缩放按分量取倒数，含零分量保护） |
| `Vec3 transform_point(Transform, Vec3)` | 点变换 |
| `Vec3 transform_dir(Transform, Vec3)` | 方向变换（忽略平移） |
| `Transform lerp(Transform,Transform,float)` | 见 E |

### 2.4 约定（冻结）

- **列主序**：`Mat4.m[col*4+row]`；`mul(A,B)` 表示先应用 B 再 A，`transform_point(A, transform_point(B,p))==transform_point(mul(A,B),p)`。
- **Euler**：`Transform.rotation` 分量 `(x,y,z)` 为绕 X/Y/Z 的弧度，**内旋顺序 Z→Y→X**，即 `R = Rx·Ry·Rz`；与 `from_euler/to_euler` 严格互逆（在万向锁外）。
- **右手系**，深度范围 `[0,1]`（WebGPU，ADR-0025）。
- **单位**：长度任意；角度一律弧度。
- **Quat 归一化**：凡产生新四元数的操作保证接近单位（slerp/normalize 后重归一）。

### 2.5 错误 / 断言模型

- **无异常、无 `std::error_code` 于热路径**：全部 `noexcept`，`[[nodiscard]]`。遵循 `docs/00`「无异常越界」。
- **前置条件**（如 `normalize(0)`、`inverse(奇异)`、`slerp` 输入未归一化）：
  - **Debug**：`MQ_ASSERT`（引擎断言，禁裸 `assert`）——早失败、可观测（喂 A2）。
  - **Release**：**确定性回退**（`normalize→0`、`inverse→identity`、零四元数→identity），**绝不 UB**。
- **可查错路径**：对可能失败且调用方需感知的运算提供 `bool try_*`（`try_inverse`）或返回可被 `is_finite` 检测的 `NaN`（`plane_from_points` 退化）。
- **输入污染防御**：上层怀疑数据时用 `is_finite` / `roughly_equal` 主动校验（供 script/AI 边界）。

### 2.6 版本化

- `MQ_MATH_ABI_VERSION` 与 `static_assert` 绑定：任何字段增删/重排、`sizeof` 变化、Euler/列主序约定改变 → **bump 版本**。
- 与 `docs/02` 的 `abi_version` 及 `docs/06` 的 schema 版本**同步发布**；schema 变更由 codegen 产出，math 头随之更新。
- 消费方可在初始化时比较 `MQ_MATH_ABI_VERSION` 做兼容检查（宿主模块注册时）。

### 2.7 内存所有权

- **math 不分配任何内存**：无 allocator、无隐藏缓冲、无 static 缓冲。全部按值/`const&` 传入传出。天然满足 `docs/07 R5`（分配者释放）——因为**根本无分配**。
- 操作结果放入调用方提供的 `out` 参数（`try_inverse(Mat4, Mat4&)`），或按值返回 POD（`Mat4` 64B，可接受）。

### 2.8 线程 / 确定性

- **可任意跨线程**：所有函数为纯函数，无共享可变状态 ⇒ **天然线程安全**，无需锁（`docs/00 ADR-0014`：零全局可变状态）。
- **确定性（A3）硬约定**：
  1. 编译期 **禁用 FMA 收缩**（`#pragma clang fp contract(off)` / `-ffp-contract=off`），**禁用 `-ffast-math`**，避免 x87（`-mfpmath=sse`/`-msse2`）。
  2. 热路径**只用正确舍入的 IEEE 运算**（`+ - * / sqrt`）——这些跨平台 bit 一致。
  3. **`sin/cos/tan/atan2/exp/pow` 依赖 libm，跨平台不保证 bit 一致** ⇒ 模拟路径（physics/animation 积分）必须走本模块 `det_*` 确定性实现（自研多项式/查表 + 明确误差界）；仅视觉路径可用系统 `libm`。
  4. `perp(v)` 的择轴规则、`look_rotation` 的 up 退化、`slerp` 的 dot<0 分支均**确定**，不依赖地址或未初始化值。
  5. SIMD 仅在能证明**逐 bit 等于标量参考实现**时启用；否则仅用于标记为"非模拟"的视觉路径。**标量参考实现是权威真值**。

---

## 3. 依赖与边界

- **可依赖**：
  - C++23 标准库的**编译期/纯函数**子集：`<cstdint>`、`<cmath>`（仅 `sqrt/fabs` 等正确舍入项）、`<bit>`、`<type_traits>`、`<cstddef>`。
  - codegen 产物 `math_types.h`（schema 驱动，属本模块分发）。
- **禁止**（硬红线）：
  - **不得**依赖任何上层模块（core/scene/physics/renderer/…）。
  - **不得**持有全局/静态可变状态（零全局状态，ADR-0014）。
  - **不得**在 ABI/序列化面出现 `std::vector`/`std::string`/指针/vtable/平台相关字段（`docs/07 §4`）。
  - **不得**分配内存、抛异常、加锁、I/O、日志（除 debug 断言）。
  - **不得**做会改变 FP 结果的"优化"：快开方近似、`-ffast-math`、随机 SIMD 重排。
  - 热路径**不得**经虚函数（本模块全 inline，天然满足 `docs/00 §6③`）。
- **与其它模块的接缝**：
  - **core/data（02）**：math 类型是 schema 一等公民；codegen 生成其 POD。core 提供 `MQ_ASSERT` 与类型描述符。
  - **scene（11）/ animation（12）**：`Transform`、`Mat4`、`Quat` 是层级传播 / 骨骼 / 蒙皮的直接输入。
  - **physics（13）**：Rapier FFI 的向量/transform 需与 math 布局**逐字段对齐**（`static_assert` 于 FFI 边界）。
  - **renderer（15）**：`Mat4`（列主序、`[0,1]` 深度）直接喂 WebGPU uniform；`Aabb` 供剔除。
  - **particle（14）**：用 `det_*` 与向量运算做确定性发射。
  - **script（16）**：绑定由反射生成，字面量即 math POD（不手写绑定）。

---

## 4. 测试计划（从接口推导，先行）

> 测试框架：**doctest**（单头、无依赖、C++23），或引擎既有测试 harness（若无则以 doctest 起步，保持"仅依赖本模块 + 公共头"）。
> 每个测试目标**只 `#include "mq_math.h"`**，不链接任何其它模块。

### 4.1 单元测试用例清单

| # | 用例 | 输入 | 期望 | 类型 |
|---|------|------|------|------|
| T-01 | 布局/尺寸 | `sizeof`/`offsetof` 各类型 | 与 schema 一致；`static_assert` 通过 | 正常 |
| T-02 | `clamp` 边界 | `clamp(5,0,1)`, `clamp(-1,0,1)`, `clamp(0.5,0,1)` | 1 / 0 / 0.5 | 边界 |
| T-03 | `clamp` 非法区间 | `lo>hi` | Debug 断言触发 | 错误 |
| T-04 | `lerp` 端点 | `t=0`,`t=1`,`t=0.5`,`t=2` | `a`,`b`,中点,外插 | 正常/边界 |
| T-05 | `inv_lerp` 退化 | `a==b` | 返回 0（无除零） | 边界 |
| T-06 | `roughly_equal` | 接近/远离/NaN | 真/假/假 | 正常 |
| T-07 | Vec3 `dot/cross` | 正交基 `X,Y,Z` | `dot=0`，`cross(X,Y)=Z` | 正常 |
| T-08 | `cross` 反对称 | `cross(a,b) == -cross(b,a)` | 成立 | 性质 |
| T-09 | `length` vs `length_sq` | 3-4-5、单位向量 | `25` / `5` / `1` | 正常 |
| T-10 | `normalize` 零向量 | `{0,0,0}` | Debug 断言；Release 返回 `{0,0,0}` | 错误 |
| T-11 | `safe_normalize` | 零向量 + fallback | 返回 fallback | 边界 |
| T-12 | `angle_between` | 同向/正交/反向 | 0 / π/2 / π | 边界 |
| T-13 | `reflect` | 45° 入射 | 正确镜像 | 正常 |
| T-14 | Mat4 单位乘 | `mul(I,M)==M` | 成立 | 正常 |
| T-15 | Mat4 结合律 | 随机 A,B,C | `mul(mul(A,B),C)` 近似 `mul(A,mul(B,C))` | 性质 |
| T-16 | 变换一致性 | `t: A*B`，点 p | `transform_point(A, transform_point(B,p)) == transform_point(mul(A,B),p)` | 性质 |
| T-17 | `transpose` 对合 | `transpose(transpose(M))==M` | 成立 | 性质 |
| T-18 | `determinant` | I=1；已知奇异阵=0 | 精确/近似 | 正常/边界 |
| T-19 | `inverse` | `mul(M, inverse(M))≈I` | 各 M 近似单位 | 正常 |
| T-20 | `inverse` 奇异 | 行列式 0 | Debug 断言；Release 单位阵 | 错误 |
| T-21 | `try_inverse` 奇异 | 奇异阵 | 返回 `false`，`out` 不被误用 | 错误 |
| T-22 | `compose_trs/decompose` | 随机 T,R,S | 往返 `decompose(compose(t,r,s))≈(t,r,s)` | 性质 |
| T-23 | `perspective` 深度 | near/far 点 | z 映射到 `[0,1]` | 正常 |
| T-24 | `look_at` | eye/target/up | 视图空间下 target 在 `-Z` | 正常 |
| T-25 | Quat `from/to_euler` | 任意 euler（避万向锁） | 往返一致 | 性质 |
| T-26 | Quat `from/to_mat3` | 随机旋转 | 往返一致、`to_mat3` 正交 | 性质 |
| T-27 | Quat `mul` 与旋转 | `mul(q1,q2)` 旋 v | 等 `rotate(q1, rotate(q2, v))` | 性质 |
| T-28 | Quat 旋转往返 | `rotate(q, v)` | 保长度、`rotate(inverse(q), ·)` 复原 | 性质 |
| T-29 | `slerp` 端点/中点 | `t=0,1,0.5` | `a`,`b`,单位且等角中点 | 正常/边界 |
| T-30 | `slerp` 最短路径 | `dot(a,b)<0` | 走短弧（不绕远路） | 边界 |
| T-31 | `slerp` 近平行 | `a≈b` | 退化为 lerp 不除零 | 边界 |
| T-32 | `nlerp` 单位性 | 任意 t | 结果为归一四元数 | 性质 |
| T-33 | `from_to_rotation` 180° | 反向向量 | 结果旋转 from→to | 边界 |
| T-34 | Transform `compose` | parent×child | 等价矩阵乘 | 正常 |
| T-35 | Transform `inverse` | 含零缩放分量 | 无除零/无 NaN | 边界/错误 |
| T-36 | Transform `lerp` | 位置/缩放线性、旋转 slerp | 分量符合约定 | 正常 |
| T-37 | `ray_aabb` 命中/未命中/相切 | 三类射线 | `true/false`，`tHit` 有效 | 正常/边界 |
| T-38 | `ray_sphere` | 命中/擦边/背向 | 正确 `tHit` | 正常/边界 |
| T-39 | `closest_point_on_segment` | 端点外/内 | 夹取正确 | 边界 |
| T-40 | `triangle_normal/area` | 退化三角形 | 零面积、NaN 法线+`is_finite` 检出 | 错误 |
| T-41 | `aabb_from_points` | 空点集 | 断言 / 空盒约定 | 边界 |
| T-42 | 版本兼容 | `MQ_MATH_ABI_VERSION` | 消费方检查通过 | 版本 |
| T-43 | 内存所有权 | 全 API | 无分配（测试用替换全局 `operator new` 计数=0） | 所有权 |
| T-44 | `is_finite` 污染输入 | NaN/Inf 进入 `normalize` 等 | 被检出/不 UB | 错误 |

### 4.2 游戏场景用例（真实玩法驱动）

| # | 场景 | 步骤 | 期望 |
|---|------|------|------|
| S-01 | **角色移动积分** | `pos += vel*dt`，60 步固定 dt=1/60 | 与解析解误差 < ε；步数可复现 |
| S-02 | **相机变换** | `view=look_at(eye,target,up)`，`proj=perspective(...)`，投影世界点 | 屏幕坐标落在视锥内/外正确 |
| S-03 | **骨骼层级** | 肩→肘→手，各自 local Transform；`compose` 链 | 末端世界位置正确；改肩旋转，手跟随 |
| S-04 | **射击方向** | `dir=normalize(target-muzzle)`，`ray_from(origin,dir)` | 命中 hitbox 时 `ray_aabb` 为真 |
| S-05 | **弹道轨迹** | 逐帧 `v += g*dt; p += v*dt`，记录 checksum | 同 dt/同初值 → 同轨迹（可回放） |
| S-06 | **朝向平滑** | `q= slerp(q0, look_rotation(...), t)` | 转身无翻转、走短弧 |
| S-07 | **地面高度采样** | `barycentric` + `triangle_normal` 插值 | 高度/法线与解析一致 |
| S-08 | **AABB 剔除** | 相机 frustum 简化为 AABB 与物件盒求交 | 视锥内物件被保留 |
| S-09 | **帧率无关阻尼** | `damp(cur,tgt,λ,dt)`，dt=1/30 与 1/60 | 相同时长后结果近似相等 |
| S-10 | **父节点缩放传播** | 非均匀缩放父 + 旋转子 | `compose` 后子网格形变符合预期 |

### 4.3 确定性用例（A3）

| # | 用例 | 方法 | 期望 |
|---|------|------|------|
| D-01 | 同输入同输出 | 固定字面量跑 N 次核心 API，哈希结果 bit 序列 | 每次 hashCode 完全一致 |
| D-02 | 无 FMA 漂移 | 编译 `-ffp-contract=off`，跨优化级别 `-O0/-O2` | 结果 bit 一致 |
| D-03 | 跨平台一致性（CI 矩阵） | 原生 x86_64 / aarch64 / WASM，跑同一向量（**用 `det_*`**） | checksum 一致 |
| D-04 | 回放一致 | S-05 弹道 60 帧，两次回放 | 每帧 position checksum 一致 |
| D-05 | 禁地址依赖 | 同一逻辑不同分配/对齐 | 结果不变 |
| D-06 | `det_sin/det_cos/det_atan2` 误差界 | 与 libm 对比全域采样 | 绝对误差 < 声明界（如 1e-6）；**bit 稳定** |

> **跨平台一致性仅对 `+ - * / sqrt` 与 `det_*` 要求 bit 一致**；系统 `sin/cos/exp` 不纳入确定性断言（文档化区别）。

### 4.4 测试替身（fake/stub）

- **无需真替身**：math 零依赖，测试直接被 `mq_math.h` 包含，无上游可 mock。
- **需提供的桩**：
  - `mq_test_assert.h`：把 `MQ_ASSERT` 重定向为测试可捕获的记录器，使 T-03/T-10/T-20 可断言"触发了断言"而不中断进程。
  - `counting_allocator`（仅测试）：替换全局 `operator new/delete` 计数，验证 T-43「零分配」。
  - `det_*` 参考实现桩：D-06 里作为被测目标（非替身）。

### 4.5 独立单测保障

- 单测目标**只链接本模块**的 interface target（`maque::math`），`target_link_libraries` 不含 core/scene 等。
- 每个 `#include` 白名单化（CI 检查仅允许 `<cmath>/<cstdint>/<bit>/<type_traits>` + `mq_math.h`）。
- 布局断言就地验证，不依赖 codegen 运行时——防止"靠 core 才通过"。

---

## 5. 开发计划（分阶段 TDD）

1. **阶段 0 · 冻结接口**：确定 `mq_math.h` 全部类型与操作签名；把 `Quat/Aabb/Ray/Plane` 回写 `schema.yaml`；codegen 产出 `math_types.h`；`static_assert` 全通过；`MQ_MATH_ABI_VERSION=1`。**接口不完备不得进入阶段 1。**
2. **阶段 1 · 测试骨架（红）**：落地 §4.1 全部用例（含 S/D 场景），此时实现为空壳 → 全红。建立 doctest + 断言桩 + 计数分配器。
3. **阶段 2 · 最小实现到绿**：按接口分组实现——标量→向量→矩阵→四元数→Interp→Transform→几何；每实现一组，跑对应测试转绿。优先保证布局与纯函数性质。
4. **阶段 3 · 补边界/错误/确定性**：打磨零向量/奇异/退化分支、`try_*` 路径、`det_*` 确定性实现与误差界；打通 D-01..D-06；重构去重、收紧内联。
5. **阶段 4 · 上游 fake 集成 + 预算**：写 fake 消费者（假 scene/animation）验证接口可用；跑 CI 跨平台矩阵（x86_64/aarch64/WASM）一致性；测性能/体积，确认无分配、无全局状态。

---

## 6. 验收标准（DoD）

- **接口冻结**：`mq_math.h` 签名与 §2 一致，`static_assert` 全过；被上游 fake（scene/animation）成功依赖并编译。
- **单测覆盖率 ≥ 90%**（纯头库、函数密集，建议高目标）；行/分支覆盖达标。
- **确定性用例通过**：D-01..D-05 全绿；D-06 误差在声明界内且 bit 稳定；CI 跨平台 checksum 一致。
- **性能/内存预算**：`Vec3/Vec4/Mat4` 运算内联后无函数调用开销（汇编抽检）；**零堆分配**（T-43）；头文件编译体积可控（无模板膨胀）。
- **红线**：无跨模块实现依赖；零全局可变状态；无异常/分配/锁；ABI 面不含 `std::`/指针/vtable。
- **可观测**：debug 断言可被 A2 捕获；`is_finite` 可用于数据校验。

## 7. 风险

| 风险 | 处理 |
|------|------|
| libm 三角函数跨平台不 bit 一致，破坏回放（A3） | 模拟路径强制走 `det_*`；系统函数仅限视觉路径；D-06 设误差界 |
| FMA 收缩/`-ffast-math` 导致结果漂移 | 编译期 `fp contract(off)` + 禁 fast-math；D-02 守护；CI 固定编译选项 |
| SIMD 与标量结果不一致 | 标量参考实现为权威；SIMD 仅在能 bit 复现时启用，否则仅视觉路径 |
| schema 与 math 头漂移（`Quat` 等手写） | 一切布局经 `schema.yaml`+codegen；`static_assert` 守门；禁手工改生成物 |
| 列主序 / Euler 约定被误用（跨模块） | 写死在 `mq_math.h` 头注释 + 单测 T-22/T-24/T-25；renderer/physics 接入时对拍 |
| 奇异矩阵/零向量在 release 产生 UB | `try_*` + 确定性回退 + `is_finite`；T-11/T-21/T-35 覆盖 |
| 头文件膨胀拖累嵌入式编译 | 小函数强制 `inline`；大函数（`inverse/slerp`）单实现点；按需 include |
| Rapier FFI 布局不匹配 | 在 physics 边界加 `static_assert(sizeof==N)` 对拍（跨模块联合测试） |

## 8. Owner agent

- **角色标识**：`agent-math`
- **职责边界**：仅负责 `math` 模块的**接口冻结、测试与实现**（`modules/math/` 与 `mq_math.h`、`math_types.h` 生成配置）；并按需将 `Quat/Aabb/Ray/Plane` **回写** `schema.yaml`（新增条目，不改既有字段/尺寸）。**不修改**其它模块的实现；跨模块布局问题通过接口断言暴露给对应 owner。
