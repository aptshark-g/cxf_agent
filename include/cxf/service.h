#ifndef CXF_SERVICE_H
#define CXF_SERVICE_H

#include "base.h"
#include "module.h"

/*
 * 服务表：DI 的装配产物。
 *
 * 三阶段纪律（这是本文件存在的全部理由）：
 *   装配期  允许字符串查找、版本校验、DAG 排序。慢无所谓，低频。
 *   固化    模块在自己的 init 里 find 一次，把结果存进 state。
 *   调用期  直接用 state 里已固化的指针，一次跳转直达，零查找。
 *
 * 违反这条纪律的代价不是"慢一点"：一旦调用期出现查找，
 * "谁调用谁"就从编译期事实退化成运行时数据 —— grep 失效、内联失效、
 * 编译器无法跨函数优化。那是白盒性的根本损失，不是性能问题。
 *
 * 与能力位（module.h 的 cxf_caps）的分工：
 *   能力位     32 种上限，O(1) 匹配，用于核心能力
 *   服务表     无数量上限，字符串查找，用于插件式扩展服务
 * 两者都在装配期完成匹配，调用期都不再查找。
 *
 * 不变式：
 *   S1 同一 name 不会注册两次
 *   S2 find 的 version 必须完全相等才匹配 —— 不做"向上兼容"，
 *      ABI 兼容由注册方显式声明多个版本来提供
 *   S3 整组注册要先整体校验再提交 —— 服务表同样只增不减，
 *      失败时留下半张表会让调用方无从判断
 */
typedef struct cxf_service_def {
    const char *name;
    uint32_t    api_version;
    const void *api;
} cxf_service_def;

/*
 * 声明一个服务。展开成一个外部链接的常量，所以它必须写在实现文件里，
 * 不能写在被多个 TU 包含的头文件里 —— 那样会重复定义符号。
 *
 * SYM 是给符号取名用的标识符，NAME 是查找时用的字符串。两者分开，
 * 因此 NAME 可以带点号与斜杠（"particle.sim" 这种命名空间写法）。
 *
 * 好处：API 表本身可以保持 static。对外可见的只有这个薄薄的一层注册项，
 * 调用方拿不到也不该拿到 API 结构体的符号。
 */
#define CXF_SERVICE(SYM, NAME, VER, API) \
    const cxf_service_def cxf_svc_##SYM = { (NAME), (VER), (API) }

/*
 * 成组注册项，由生成器产出。
 *
 * 与类型表的差别：这里是**指针**数组，那里是值数组。原因是注册项本身
 * 已经是由 CXF_SERVICE 定义的外部常量，放在一起再拷一遍没有意义，
 * 而且指针形态让 API 表得以保持 static。
 */
typedef struct cxf_service_bundle {
    const cxf_service_def *const *entries;
    uint32_t                      count;
    const char                   *source;   /* 只用于诊断 */
} cxf_service_bundle;

/*
 * 注册服务。api 指向的函数指针表必须在 world 生命周期内保持有效，
 * 通常是编译期静态常量。
 */
cxf_result cxf_service_register(cxf_world *w, const char *name,
                                uint32_t api_version, const void *api);

/*
 * 按名字与版本查找。找不到返回 NULL。
 * 只在装配期调用（模块 init、模块加载后）—— 见顶部三阶段纪律。
 */
const void *cxf_service_find(const cxf_world *w, const char *name,
                             uint32_t api_version);

/* 整组注册，见 S3。重名返回 CXF_ERR_RANGE。 */
cxf_result cxf_service_register_bundle(cxf_world *w,
                                       const cxf_service_bundle *b);

#endif
