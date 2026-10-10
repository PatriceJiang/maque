# maque schema 语言规范（v1）

> `schema.yaml` 是**反射中枢的唯一真相**（docs/06）。本规范定义其语法与 codegen 三个产出的映射。
> `parser.js` 读取 `schema.yaml`，生成：① C++ 类型头 ② C ABI 描述符 ③ JSON schema。

## 1. 顶层结构

```yaml
- namespace: <name>       # 一个命名空间
  version: 1             # 可选，默认 1
  class_list:            # 该命名空间的类型定义（顺序无关）
    - <type-def>
    - <type-def>
```

多个命名空间 → 顶层是数组。

## 2. 类型定义（type-def）

### 2.1 struct / class（POD 或含容器）

```yaml
- struct: Transform3D      # `class` 是等价别名（兼容旧 schema）
  size: 48                # 可选：固定字节数（POD 时生成 static_assert(sizeof==size)）
  align: 4                # 可选：对齐断言
  fields:
    - { name: position, type: Vec3 }
    - { name: rotation, type: Vec3 }
    - { name: scale,    type: Vec3, default: "{1.0F, 1.0F, 1.0F}" }
```

- `size` 仅对 **POD 类型**（不含动态容器/string/map/optional）有效，用于 `static_assert`。
- `fields` 顺序 = **内存布局顺序 = 序列化顺序**（不得重排；docs/08 §4）。

### 2.2 enum

```yaml
- enum: LightType
  underlying: uint32_t    # 可选，默认 int32_t
  values:
    - { name: Directional, value: 0 }
    - { name: Point,       value: 1 }
    - { name: Spot,        value: 2 }
```

- 生成 `enum class <Name> : <underlying> { ... }`。
- `value` 可省略（自动递增）。

### 2.3 alias（可选）

```yaml
- alias: EntityRef = EntityHandle
```

## 3. 字段

```yaml
fields:
  - name: <identifier>
    type: <type-expr>
    default: <cpp-expr-string>     # 可选，作为字段初始化表达式
    attrs:                         # 可选，全部为元数据（供编辑器/AI/校验）
      transient: true              # 不序列化（运行时派生）
      readonly: true
      range: [0, 100]
      unit: px
      ref: Texture                 # 引用目标类型名（asset_ref/entity_ref 等）
      tooltip: "..."
      deprecated: true
```

## 4. 类型表达式（type-expr）

| 类别 | 语法 | 例 |
|------|------|----|
| 标量 | 关键字 | `bool char int8_t uint8_t int16_t uint16_t int32_t uint32_t int64_t uint64_t float double` |
| 具名类型 | `Name` 或 `ns.Name` | `Vec3`、`render.Sprite` |
| 定长数组 | `T[N]` | `float[9]` |
| 动态数组 | `T[]` | `Vec3[]` |
| 字符串 | `string` | `string` |
| 映射 | `map<K,V>` | `map<string,int32_t>` |
| 实体引用 | `entity_ref` | `entity_ref` |
| 资产引用 | `asset_ref` / `asset_ref<T>` | `asset_ref<Texture>` |
| 可选 | `optional<T>` | `optional<int32_t>` |

## 5. 类型映射（schema → C++）

| schema | C++（生成头中） |
|--------|-----------------|
| 标量 | 同名 |
| enum `E` | `ns::E` |
| struct/class `S` | `ns::S` |
| `T[N]` | `T name[N]` |
| `T[]` | `std::vector<T> name` |
| `string` | `std::string name` |
| `map<K,V>` | `std::map<K,V> name` |
| `entity_ref` | `maque::EntityRef` |
| `asset_ref` / `asset_ref<T>` | `maque::AssetRef`（`<T>` 仅作类型标注，布局一致） |
| `optional<T>` | `std::optional<T> name` |

- **POD 判定**：字段类型**不含** `[]` / `string` / `map` / `optional` 时，该类型为 POD，`size` 断言才生效。
- 容器类字段由**描述符**标记为动态，序列化按元素 visitor（docs/08）。

## 6. 描述符（C ABI 输出）

```c
typedef enum mq_type_kind {
  MQ_TK_BOOL, MQ_TK_I8, MQ_TK_U8, MQ_TK_I16, MQ_TK_U16, MQ_TK_I32, MQ_TK_U32,
  MQ_TK_I64, MQ_TK_U64, MQ_TK_F32, MQ_TK_F64, MQ_TK_ENUM, MQ_TK_STRUCT,
  MQ_TK_ARRAY_FIXED, MQ_TK_ARRAY_DYN, MQ_TK_STRING, MQ_TK_MAP,
  MQ_TK_ENTITY_REF, MQ_TK_ASSET_REF, MQ_TK_OPTIONAL
} mq_type_kind;

typedef struct mq_field_desc {
  const char* name;
  uint32_t    offset;        /* offsetof(Type, field) */
  mq_type_kind kind;
  uint32_t    type_id;       /* 具名/元素类型；非具名为 0 */
  uint32_t    count;         /* 定长数组长度；否则 0 */
  uint32_t    attrs_flags;   /* bit0 transient, bit1 readonly, bit2 deprecated */
} mq_field_desc;

typedef struct mq_type_desc {
  const char*          name;   /* 全限定名 ns.Name */
  uint32_t             type_id;
  uint32_t             size;
  uint32_t             align;
  uint32_t             field_count;
  const mq_field_desc* fields;
  uint32_t             flags;  /* bit0 is_enum, bit1 is_pod, bit2 has_dynamic */
} mq_type_desc;

/* 生成的注册表（可按 type_id/name 查找） */
extern const mq_type_desc* const mq_schema_types[];
extern const uint32_t            mq_schema_type_count;
```

- **`type_id` = FNV-1a 32bit(全限定名)**（稳定，禁用序号；docs/06 §5）。

## 7. JSON schema 输出

- 规范化后的 schema（键按固定顺序、类型按名字排序），供 AI/工具消费（docs/06 §4）。
- 含每个类型的 `name/type_id/size/fields[]`。

## 8. 输出与 CLI

- 默认输出目录：`generated/`（与 `schema.yaml` 同级）。
- 生成文件：
  - `generated/maque_types.h` —— C++ 类型（枚举 + 结构体 + 默认值 + static_assert）
  - `generated/maque_desc.h` / `generated/maque_desc.cpp` —— C ABI 描述符与注册表
    （`.cpp` 因需 `offsetof`/`sizeof` 而包含 `maque_types.h`，但所有导出符号保持
    `extern "C"`，即 C ABI 不变）
  - `generated/maque_schema.json` —— JSON schema
- CLI：`node parser.js [--schema schema.yaml] [--out generated] [--check]`
  - `--check`：只校验，不写文件；发现问题非零退出。

## 9. 校验规则（`--check` 必查）

1. 类型/字段名唯一；命名空间不重复。
2. `type-expr` 可解析；引用的具名类型**存在**。
3. POD 类型的 `size` 必须**精确等于**计算布局（`declared != computed` 即报错，并在终端
   打印 `类型名 | declared | computed` 清单）；非 POD 不得声明 `size`/`align`。
4. `default` 存在时类型大致匹配（尽力）。
5. 枚举值不重复。
