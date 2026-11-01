# 类型树与 SoA 自动布局

从 C 结构体自动生成 SoA 列布局，支持任意深度嵌套，并能 gather 回原结构体。

## 1. 目标与非目标

**目标**

- 结构体定义是唯一事实来源，布局描述符由工具生成，不手写
- 嵌套结构体递归展开，叶子成为 SoA 的列
- 提供 gather / scatter 在 AoS 与 SoA 之间往返
- 按 size / kind 建立索引，支持"找出所有 4 字节字段"这类查询

**非目标**

- 不做完整反射（不暴露运行时构造类型的能力）
- 不做序列化（那是另一个模块，可以复用本层的类型树，但不在本层实现）
- 不做跨动态库的 ABI 协商

## 2. 模型

```c
typedef enum cxf_kind {
    CXF_K_I8,  CXF_K_U8,  CXF_K_I16, CXF_K_U16,
    CXF_K_I32, CXF_K_U32, CXF_K_F32,
    CXF_K_I64, CXF_K_U64, CXF_K_F64,
    CXF_K_PTR,      /* 指针：递归的终止符 */
    CXF_K_STRUCT,   /* 内部节点 */
} cxf_kind;

typedef struct cxf_type cxf_type;

typedef struct cxf_field {
    const char        *name;
    uint32_t           offset;   /* offsetof(宿主, 字段) */
    const cxf_type    *type;
} cxf_field;

struct cxf_type {
    const char     *name;
    uint32_t        size;
    uint32_t        align;
    cxf_kind        kind;
    uint32_t        field_count;
    const cxf_field *fields;   /* kind == STRUCT 有效 */
    const cxf_type  *target;   /* kind == PTR 有效，void* 时为 NULL */
};
```

`cxf_type` 是树的节点，`cxf_field` 是边。叶子是原子类型或指针。

**为什么 size/align 存在 type 里而不是 field 里**：嵌套时子结构体的 size 与 align
只与其类型有关，与它在父结构体中的位置无关。放 type 里让展平逻辑少一次间接查询。

## 3. 终止性

递归展开必定终止，这是可证的：

C 的 struct 不能包含自身实例（那是不完整类型），自引用只能通过指针实现。
因此类型图是有限深度的有向无环图，递归必然落到 `kind != CXF_K_STRUCT` 的节点。

```c
static void flatten(const cxf_type *t, uint32_t off, cxf_leaf *out, uint32_t *n) {
    if (t->kind != CXF_K_STRUCT) { out[(*n)++] = (cxf_leaf){ off, t }; return; }
    for (uint32_t i = 0; i < t->field_count; i++)
        flatten(t->fields[i].type, off + t->fields[i].offset, out, n);
}
```

函数短得不像话，正是因为终止性有保证，不需要深度上限或 visited 集合。

## 4. 展平产物

展平每个类型一次，得到叶子数组，装配期完成：

```c
typedef struct cxf_leaf {
    uint32_t        offset;   /* 在原结构体内的绝对字节偏移 */
    const cxf_type *type;     /* 叶子类型，决定列宽与对齐 */
    uint32_t        col;      /* 对应 SoA 的列号 */
    const char     *path;     /* "pos.x" 形式，供调试与按名查找 */
} cxf_leaf;
```

`path` 用点号连接嵌套路径。它不是给热路径用的，是给索引查询和诊断用的。

## 5. SoA 表

**复用 `cxf_table`，不另造存储。** table 已经具备 SoA 多列、稳定 handle、
世代失效，缺的只是"从结构体自动生成列定义"这一环。

构建：展平得到 N 个叶子 → `cxf_table_init(t, alloc, sizes, N, cap)`，
其中 `sizes[i] = leaves[i].type->size`。

```c
void *cxf_soa_col(cxf_soa *s, uint32_t leaf);              /* 热路径 */
void  cxf_soa_gather(cxf_soa *s, uint32_t row, void *out);  /* 冷路径 */
void  cxf_soa_scatter(cxf_soa *s, uint32_t row, const void *in);
```

gather 就是按 leaf 的 offset 逐字段 memcpy 回宿主结构体。
它是便利设施，不是主访问路径 —— 热路径应当直接取列指针。

## 6. 代码生成

**输入**：头文件，结构体用 `CXF_TYPE(...)` 标记。
**输出**：`xxx.gen.c` + `xxx.gen.h`。生成物进版本库，`make gen` 手动触发。

### 分工：生成器不算数值

生成的 C 代码里写的是 `sizeof` / `_Alignof` / `offsetof`，不是数字。
生成器只决定"有哪些类型、哪些字段、每个字段是什么种类"，数值由编译器填。

```
生成器  = 名字 + 结构 + 种类
编译器  = size / align / offset
```

换来三个性质：

1. **交叉编译正确** —— 数值永远由目标编译器给出，不依赖生成机
2. **生成物可读** —— 它就是普通 C，能直接读、能断点调试
3. **宏、typedef、匿名结构、条件编译都交给 clang**，生成器不碰，也就算不错

### 标记为什么展开成真 typedef

```c
#define CXF_TYPE(T) typedef T T##_cxf_type
```

`CXF_TYPE(Particle)` 展开成 `typedef Particle Particle_cxf_type`，不是空宏。
名字写错时编译期就报错，生成器不会静默跳过一个类型。

**曾经试过用 `__attribute__((annotate("cxf")))`，放弃了**，两个原因：

- 写在 `struct` 关键字前面会被 clang 直接忽略（`-Wignored-attributes`），
  只有写在右花括号之后才生效 —— 语法位置很别扭
- 换到 `typedef struct { ... } Vec3 CXF_TYPE;` 时，属性会挂到 TypedefDecl 上，
  而不是 RecordDecl，检测逻辑要分两路

标记只提供名字，结构全部交给 AST。两边对不上就是编译错误，
不需要生成器自己去校验一致性。

### 工具链：`clang -Xclang -ast-dump=json`，不是 libclang

零依赖（clang 本身已有），输出 JSON，标准库即可解析。
libclang 的 Python binding 需要额外安装且版本易错配。

### 实测踩到的三个坑

1. **AST JSON 里没有 size / align / offset。** 只有名字与类型拼写。
   这正是"生成器不算数值"的由来 —— 不是选择，是 AST 没给。
2. **AST 里判断 decl 属于哪个文件不可靠。** clang 的 JSON dumper 会省略
   与上一个 decl 相同的 `file`、`line` 字段（有状态），拿 `loc.file` 过滤
   系统头文件会在边界处漏判。改用名字解析：一个翻译单元内 struct 名字唯一，
   重名即重定义错误，所以按名字查没有歧义。
3. **匿名 typedef 结构体（`typedef struct { ... } Vec3;`）的定义节点与
   typedef 是兄弟关系**，typedef 子树里只有引用。引用节点带
   `ElaboratedType.ownedTagDecl.id`，按 id 反查才是定义。按名字猜会失败。

### 标记还是全量

标记式。全量扫描会把第三方头文件的结构体也拉进来，噪声太大。
被依赖的类型会自动带出来并打印提示，不需要逐个标记。

## 7. 拒绝清单

以下类型在生成期报错，不静默跳过：

| 类型 | 原因 |
|---|---|
| union | 大小可知，活跃成员不可知 |
| 位域 | `offsetof` 对位域未定义 |
| 数组 | `cxf_kind` 里没有数组这一档。加它要先决定元素是叶子还是继续展开，现阶段拒绝而不是猜 |
| 柔性数组 | 大小不可知（走数组的拒绝路径） |
| 函数指针 | 是代码地址，不是数据，没有可展平的叶子 |
| `char` | 符号性由平台决定（ARM AAPCS 为 unsigned，其余多数为 signed）。请写 `signed char` / `unsigned char` |
| `long double` | 没有对应的 `cxf_kind` |
| 空结构体 | 没有可展平的叶子 |

指针**不拒绝**：它是合法的叶子，只是不追踪指向的内容。

**宽度假设用编译期断言兜住**：生成物里对每个用到的内置类型写
`_Static_assert(sizeof(T) == N, ...)`。平台与假设不符就编译失败，
不会静默错标 —— 例如 `long` 在 LP64 是 8 字节、在 Windows 是 4 字节。

## 8. 与其它层的关系

- 建立在 `allocator` / `table` 之上，不绕开它们，因此统计不断裂
- 类型注册表与服务表是同一模式：装配期注册与解析，调用期用固化结果
- 两者共用 `world` 的注册基建，但各自独立 —— 类型查找与服务查找不是一回事

注册期本身单列一份文档：**docs/registry.md**（三阶段纪律、接线、不变式 K1-K3、
以及"调用期零查找"是怎么被观测验证的）。

## 9. 实现顺序

1. `include/cxf/type.h` + `src/type.c`：类型描述符、展平、索引 —— 已完成
2. `include/cxf/soa.h` + `src/soa.c`：基于 table 的 SoA 构建与 gather/scatter —— 已完成
3. `tools/gen_registry.py`：clang AST → 描述符 —— 已完成
4. `tests/test_soa.c` + `tests/soa_types.h`：嵌套展开、列布局、gather 往返、拒绝路径 —— 已完成

顺序是按"核心语义先验证"定的：生成器只是自动化，先做它的话语义错了要两处返工。

第 3 步落地后，`test_soa.c` 里手写的描述符已全部删掉，改由
`tests/soa_types.gen.c` 提供。测试断言一行没改 —— 这本身就是
"生成物与手写等价"的证明。

## 10. 验证标准

- 三层嵌套结构体展平出预期叶子数与偏移
- 列宽与对齐与 `sizeof` / `_Alignof` 一致
- `scatter` 写入后 `gather` 出来与原结构体逐字节相同
- 含 union / 位域的结构体在生成期被拒绝并给出明确报错
- 顺序扫描单列的性能与 `cxf_table` 持平（本层不引入额外间接）
