#ifndef AGENT_ACTION_H
#define AGENT_ACTION_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
 * agent.action 的线格式（docs/agent.md §4.8）—— 能力的**第二种返回语义**。
 *
 *   agent.action := kind │ cap │ args
 *       kind = CALL  → 派发：cap 名 + args 字节 → 运行时派生新 WorkItem
 *       kind = DONE  → 终值：payload 即最终结果，回路终止
 *
 * 为什么不是文本：`|` 分隔的文本格式必须为 args 里的 `|` 定义转义，而转义规则
 * 一旦存在，读的一方就得实现一个与写的一方**对称**的解码器 —— 两份逻辑迟早
 * 不同步，而不同步的表现是"某些参数被静默截断"。定长头 + 显式长度前缀没有
 * 任何转义，args 可以是任意字节（含 `\0`、`|`、`\n`）。
 *
 * 为什么判别字段在 payload 内部，而不是拆成两个接口类型：`out_iface` 是
 * **静态声明**的，而 LLM 能力的输出天然是动态的（有时决定调工具、有时给出
 * 终值）。⇒ 端口类型恒为 agent.action，动态性收在 payload 内部。
 * 接口类型约束的是**端口**，payload 内部允许有自己的判别字段。
 *
 * 字节布局（显式小端，不依赖主机序 —— 同构节点之间可以直接搬）：
 *
 *     [0]        u8   kind
 *     [1..5)     u32  cap_len
 *     [5..9)     u32  args_len
 *     [9..9+n)   cap 名（cap_len 字节，无 NUL）
 *     [..)       args（args_len 字节）
 *
 * 不变量：
 *   A1 长度自洽：len >= AGENT_ACT_HDR，且 AGENT_ACT_HDR + cap_len + args_len <= len
 *   A2 kind 只有两个值；CALL 必须有非空 cap 名，DONE 必须没有
 *   A3 除 A1/A2 外不解释任何字节 —— args 是载荷，不是格式
 *   A4 一份产出可以含 **N 块**（v1.13 起）。消费方必须遍历到 `off == len`；
 *      停在第一块不是"读到了一条"，是**静默丢了 N-1 条**
 *
 * ★ **这份格式的假设只有一句：每块的结束位置由它自己的头决定。**
 *   v1.13 之前 args 是"头之后的其余全部" —— 在那个假设下 decoder 只取第一块
 *   是**正确行为**（根本没有"第二块的起点"这个概念）。所以"一轮只执行一个工具"
 *   从来不是设计决定，是**一个没写下来的假设**。修法也不是改 decoder 的逻辑，
 *   是让头自定界（5 → 9 字节）。
 *   ⇒ 判据：任何"只有一条"的格式，必须在注释里写下这个假设 ——
 *     否则它会在需要第二条时以**静默丢数据**的形式暴露（不报错、不入账、只是少了）。
 *
 * 前三条都在**解码时**校验，而不是"相信产出方"。原因：产出方可能是一个模型，
 * 它的输出不是我们写的那份代码生成的；在这里接受一个越界的 cap_len，
 * 等价于允许远端指定读多远。**A4 是消费方的义务** —— 解码器看不见"后面还有没有"，
 * 它只能说"我这一块读得完"；"有没有剩"只能由遍历者判。
 *
 * ★ v1.13 加了显式的 `args_len`，原来它是"头之后的其余全部"。改动只有一个理由：
 *   **自定界**。轮内 fan-out 要求一次执行能吐出 N 条动作首尾相接（§4.17.9），
 *   而"args 到哪结束"在拼接里是没有答案的 —— 第二条的字节会被算进第一条的 args。
 *   加了 args_len，一条动作的长度就完全由它自己的头决定；A1 也从 `<=` 变成
 *   "单条时取等、作为前缀时取小于"。
 *   代价是头从 5 字节变 9 字节（每条动作 4 字节）。**不选"外面套一个总长前缀"**：
 *   那会让长度信息出现两处，多一条"前缀与内容必须一致"的不变式要守。
 */

#define AGENT_ACT_CALL  0u
#define AGENT_ACT_DONE  1u
#define AGENT_ACT_HDR   9u

/*
 * 文本 → 二进制就地转换时要留的**前置余量**（见 `agent_llm_actions_from_response`）。
 *
 * 为什么需要：二进制块比文本形态长 4 字节（头 5→9），所以文本必须落在缓冲区
 * 靠后的位置才能原地长大。16 只是"够用且对齐"的数（>= HDR 且留了 7 字节余量），
 * 不是算出来的最优值 —— 它只影响一次 malloc 之后的指针偏移。
 */
#define AGENT_ACT_HEADROOM 16u

typedef struct AgentAct {
    uint8_t     kind;
    const char *cap;       /* 指向 payload 内部，不拥有 */
    uint32_t    cap_len;
    const char *args;      /* 指向 payload 内部，不拥有 */
    uint32_t    args_len;
} AgentAct;

/*
 * 编码。返回**需要的字节数**（snprintf 语义）：out_cap 不够时只报告不写，
 * 调用方据此返回 CXF_ERR_FULL 让 blob 翻倍重试。返回 0 表示入参本身非法
 * （kind 越界 / CALL 没有能力名）—— 那是调用方的错，不是容量问题。
 */
static inline uint32_t agent_act_encode(uint8_t kind,
                                        const char *cap, uint32_t cap_len,
                                        const char *args, uint32_t args_len,
                                        char *out, uint32_t out_cap) {
    uint32_t need;

    if (kind != AGENT_ACT_CALL && kind != AGENT_ACT_DONE) return 0u;
    if (kind == AGENT_ACT_CALL && (cap == NULL || cap_len == 0u)) return 0u;
    /* DONE 没有下一步，所以它不该带能力名。由编码端强制，而不是留给解码端猜。 */
    if (kind == AGENT_ACT_DONE) cap_len = 0u;
    if (args == NULL) args_len = 0u;

    if (cap_len > 0xFFFFFFFFu - AGENT_ACT_HDR ||
        args_len > 0xFFFFFFFFu - AGENT_ACT_HDR - cap_len)
        return 0u;
    need = AGENT_ACT_HDR + cap_len + args_len;
    if (out == NULL || out_cap < need) return need;

    out[0] = (char)kind;
    out[1] = (char)(cap_len & 0xFFu);
    out[2] = (char)((cap_len >> 8) & 0xFFu);
    out[3] = (char)((cap_len >> 16) & 0xFFu);
    out[4] = (char)((cap_len >> 24) & 0xFFu);
    out[5] = (char)(args_len & 0xFFu);
    out[6] = (char)((args_len >> 8) & 0xFFu);
    out[7] = (char)((args_len >> 16) & 0xFFu);
    out[8] = (char)((args_len >> 24) & 0xFFu);
    /*
     * 用 memmove 而不是 memcpy：这两处**允许源与目标重叠**。
     *
     * 不是防御性编程 —— v1.13 起 `agent_act_from_text` 会被**就地**调用
     * （文本 `CALL|name\nargs` 就是同一块缓冲里的输入，二进制块写在它前面，
     * 见 `AGENT_ACT_HEADROOM`），那时 cap/args 的源指针落在 out 内部、且在
     * 目标之后。这不是冷路径上的妥协：每次执行只编码一次，memmove 与 memcpy
     * 在这条路上没有可测差别。
     */
    if (cap_len > 0) memmove(out + AGENT_ACT_HDR, cap, cap_len);
    if (args_len > 0) memmove(out + AGENT_ACT_HDR + cap_len, args, args_len);
    return need;
}

/*
 * 一条动作在字节流里占多少。**自定界**：`cap_len` 与 `args_len` 都在头里，
 * 于是"N 条动作首尾相接"天然无歧义 —— 不需要分隔符、不需要总长前缀，
 * 也就不需要第二份解码器（§4.17.9 的 fan-out 就建立在这一行上）。
 */
static inline uint32_t agent_act_bytes(const AgentAct *a) {
    if (a == NULL) return 0u;
    return AGENT_ACT_HDR + a->cap_len + a->args_len;
}

/*
 * 解码。false = 这段字节不是合法动作。
 *
 * 刻意不做"尽量解读"：一个半合法动作被猜成 CALL，运行时就替产出方
 * 编了一张它没打算要的图。宁可让这次执行失败（可观测），也不猜。
 *
 * ★ `len` 可以**大于**这条动作的实际长度 —— 那说明后面还有下一块。
 *   这里只保证"我这一块读得完"，"正好读完"由调用方在拼接遍历里判
 *   （`off == res_len` 才算干净收尾）。
 */
static inline bool agent_act_decode(const char *p, uint32_t len, AgentAct *a) {
    uint32_t cap_len, args_len;

    if (p == NULL || a == NULL || len < AGENT_ACT_HDR) return false;

    a->kind = (uint8_t)(unsigned char)p[0];
    if (a->kind != AGENT_ACT_CALL && a->kind != AGENT_ACT_DONE) return false;

    cap_len = (uint32_t)(unsigned char)p[1]
            | ((uint32_t)(unsigned char)p[2] << 8)
            | ((uint32_t)(unsigned char)p[3] << 16)
            | ((uint32_t)(unsigned char)p[4] << 24);
    args_len = (uint32_t)(unsigned char)p[5]
             | ((uint32_t)(unsigned char)p[6] << 8)
             | ((uint32_t)(unsigned char)p[7] << 16)
             | ((uint32_t)(unsigned char)p[8] << 24);
    /* 先夹 cap_len 再判 args_len：顺序反了，`HDR + cap_len` 可能已经溢出。 */
    if (cap_len > len - AGENT_ACT_HDR) return false;
    if (args_len > len - AGENT_ACT_HDR - cap_len) return false;
    if (a->kind == AGENT_ACT_CALL && cap_len == 0u) return false;
    if (a->kind == AGENT_ACT_DONE && cap_len != 0u) return false;

    a->cap      = p + AGENT_ACT_HDR;
    a->cap_len  = cap_len;
    a->args     = p + AGENT_ACT_HDR + cap_len;
    a->args_len = args_len;
    return true;
}

/*
 * 文本 → agent.action 的**规范字节**（docs/agent.md §4.11）。
 *
 * 为什么需要它：模型只会吐文本。二进制格式可以当进程内的规范形式，但不能
 * 当模型的输出目标 —— 让一个 7B 模型生成定长头 + 小端长度前缀，是把
 * "格式正确"押在它的数学能力上。
 *
 * 语法（**只有两行结构，没有转义**）：
 *
 *     CALL|<能力名>\n<args 原样…>
 *     DONE\n<终值 原样…>
 *
 *   头 = 第一行。CALL 必须紧跟一个 '|' 和非空能力名（名字里不许有空白、
 *   控制字符或 '|' —— 语法只查这些，名字是否存在由注册表查，层次不混）。
 *   DONE 的头必须**恰好**是 "DONE"，不带 '|'。
 *   体 = 第一个 '\n' 之后的**全部字节**，原样，可以含 '|'、'\n'、乃至 '\0'。
 *   没有 '\n' 时体为空（合法：CALL 可以不带参数）。
 *
 * 为什么不是 JSON：JSON 要为 args 里的引号/换行定义转义，而转义规则一旦存在，
 * 读的一方就得实现一个与写的一方**对称**的转义解码器 —— 两份逻辑迟早不同步，
 * 而不同步的表现是"某些参数被静默截断"。这正是 §4.8 拒绝文本线格式的那条
 * 理由；这里的"头一行 + 其余原样"把转义的必要性消掉，于是那条理由不再成立。
 *
 * 只做**两个**归一化，且都是确定的：
 *   1. 头行尾容忍 "\r\n"（chat template 习惯补回车）；
 *   2. 体尾去掉**一个**换行（"\n" 或 "\r\n"）。只去一个 —— 去多了就变成
 *      "猜测产出方想说什么"。
 * 其余一个字不动：不 trim、不折叠空白、不补 NUL、不改大小写。
 *
 * 返回值与 agent_act_encode 同语义：需要多少字节（> out_cap 表示没放下，
 * 内容不可信），**0 表示这段文本不合语法**。0 与"放不下"必须分开处理 ——
 * 前者重试没用，后者要扩容。
 *
 * 实现上刻意**不自己拼头**，而是走 agent_act_encode：布局只有一个正本，
 * 于是"文本解析器引入一个编码器不会产出的状态"这件事在结构上不可能发生。
 * 调用方拿到字节后再过一遍 agent_act_decode，三条不变量自动生效。
 */
static inline uint32_t agent_act_from_text(const char *t, uint32_t len,
                                           char *out, uint32_t out_cap) {
    uint32_t hend = 0, hlen, body, blen, cap_off = 0, cap_len = 0;
    uint8_t kind;

    if (t == NULL) return 0u;

    /* 头：第一行（不含 '\n'，容忍一个尾随 '\r'） */
    while (hend < len && t[hend] != '\n') hend++;
    hlen = hend;
    if (hlen > 0u && t[hlen - 1u] == '\r') hlen--;

    /* 体：头之后的全部，去掉一个行尾换行 */
    body = (hend < len) ? hend + 1u : len;
    blen = len - body;
    if (blen > 0u && t[body + blen - 1u] == '\n') blen--;
    if (blen > 0u && t[body + blen - 1u] == '\r') blen--;

    if (hlen >= 4u && memcmp(t, "CALL", 4u) == 0) {
        kind = AGENT_ACT_CALL;
        if (hlen < 6u || t[4] != '|') return 0u;   /* 必须有 '|' 且名字非空 */
        cap_off = 5u;
        cap_len = hlen - cap_off;
        for (uint32_t i = 0; i < cap_len; i++) {
            unsigned char ch = (unsigned char)t[cap_off + i];

            /* 语法检查只到这里：空白 / 控制字符 / 分隔符不许进名字。
               名字是否存在是注册表的事，不在这一层判。 */
            if (ch <= 0x20u || ch == 0x7Fu || ch == '|') return 0u;
        }
    } else if (hlen == 4u && memcmp(t, "DONE", 4u) == 0) {
        kind = AGENT_ACT_DONE;   /* 不带能力名；encode 也会强制 cap_len = 0 */
    } else {
        return 0u;               /* 未知 kind：不做"猜一个"的解读 */
    }

    return agent_act_encode(kind,
                            kind == AGENT_ACT_CALL ? t + cap_off : NULL, cap_len,
                            blen > 0u ? t + body : NULL, blen,
                            out, out_cap);
}

#endif
