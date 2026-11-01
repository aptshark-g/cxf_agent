# 上下文复盘器（Context Review）：设计稿 v0.1

> 状态：提案，未落码。定位：**只读**复盘工具，不是 TUI/GUI，不碰 tick 内核。
> 产生缘由（2026-10-27 用户）："把上下文的展现做出来，这个是复盘很关键的内容"。
> 前置事实：`agent.md` v1.14.22；`--trace` 现状见 §1.1；确定性请求体契约见
> `agent_llm.h`；trace 丢失教训见 `STATUS.md` §5 末。
> 与 A7（`REQ_CAP`）无依赖，可并行；长会话复盘材料受 A7 上限影响，照 STATUS 先走。

## 0. 一句话与判据

复盘的第一性问题不是"引擎在干什么"，而是"**模型当时看到了什么**"。
本工具回答的就是这一句，且只回答这一句。

三条判据，任何一条不满足就是设计错了：

1. **不重述。** 视图必须对着原始字节，不经过任何"我以为模型看到的是……"的转述。
   教训来源：v1.14.9"接线的地方最容易假绿"——渲染层一旦出错，
   所有基于转述的分析全错。
2. **不加运行时状态。** 请求体是确定性纯函数，能重算的绝不另存；
   不能重算的（响应原始字节）必须落 trace。这是"少一处缓存与真值不同步"
   （`agent_llm.h`）在复盘侧的投影。
3. **观测不改变被观测。** trace v2 只落字节，不参与请求构造；
   请求体逐字节契约（`agent_llm.h`）不受本工具任何影响。

## 1. 数据源（先核代码，再谈视图）

### 1.1 现有 trace（v1）是什么、缺什么

`tests/agentctl.c:184-209`：每次调用落**解码后的可读形状**——

```
=== call N === rc=<int>
-- in (<len> B) --
<prompt 字节（即送给 LLM 能力的 args）>
-- act (<len> B) --
DONE <payload> | CALL <cap> <args> | (坏块 @off)
=== summary ===
final <n> B, failed <n>, ticks <n>, calls <n>
```

**缺四样，恰好是复盘最需要的**：原始请求体（system/tools/messages 的
完整 JSON）、原始响应 JSON、每轮墙钟与计数器快照、run 元信息
（model / cfg / 版本）。v1 的 `in` 只是 prompt 段，body 的其余部分
（system、tools、渲染后的 messages 结构）在 trace 里**不存在**。

### 1.2 transcript blob 与 trace 的对应关系

- blob 是**追加日志**：`#call` / `#result` / `#fail` / `#budget` 记录
  （`agent.md` §4.16.4），`convo_render` 把它渲染成标准 messages。
- trace v1 的 `in` = 当轮 prompt 字节，即 blob 的当轮快照（问题段 + 记录）。
- **对应键**：`call` 序号。同 run 内 call 递增 = 回路推进；
  跨 run 相同 body = 重发或重复测量（三层 signature，§4.16.3）。
  服务端侧用 `X-CXF-Ctx: run=<pid> call=<n>` 对齐（该头在 header 不在 body，
  不破坏确定性）。

### 1.3 trace v2：只加"不能重算"的部分

v2 = v1 全部内容 + 以下四段（文本头 + 原样字节，与 v1 同族）：

```
=== run ===
model <name>            ← AgentLlmCfg.model
system_len <n>          ← system 正文原样随后（可空）
tools_len <n>           ← tools 渲染文本原样随后（可空）
version <agent.md 版本或构建标识>

=== call N ===
-- req (<len> B) --      ← agent_llm_build_request 的完整输出，原样
-- resp (<len> B) --     ← scratch 里的原始响应 JSON，原样（可截断，标 #truncated）
-- meta rc=<int> wall_ms=<n> in=<n> out=<n> acts=<n> quota_wait=<n> ...
（v1 的 in/act 段原样保留，作为解码视图）
```

**为什么 req 直接落而不是重算**：重算需要完整 cfg + prompt + 渲染路径全同版本，
三个条件任何一个漂移就算不出来。落 req 的成本是一次 fwrite，
换来"复盘器不需要理解构建版本"。**响应必须落**——它是唯一不可重算的证据。
（§0 判据 2 的准确含义：能重算的不另存**运行时状态**；trace 是证据文件，
证据按"完整、自足"记，不按"最小"记。）

## 2. 三个视图（字段级）

实现形态：`tools/ctx_review.py`（与 `seg_probe.py` 同族，标准库零依赖）。
输入：一个 trace v2 文件。旧 v1 文件**降级使用**，每视图标注能做什么
（见 §5），不假装全能。

### 2.1 V1 轮次时间线（主索引）

每轮一行，固定列：

```
call  req_B   resp_B  acts  rc    wall_ms  lcp%   flags
1     1872    1402    1     OK    1830     —      —
2     2244    1501    2     OK    1210     83.7   fanout
3     0       0       0     FULL  4        —      req_cap
```

- `lcp%`：与上一轮的**请求体最长公共前缀**占比（算法见 §3），第一轮为 `—`。
- `flags`：`req_cap`（rc=FULL 且 req 为 0）、`fanout`（acts>1）、
  `truncated`（resp 带截断标记）、`budget`（本轮 prompt 含 `#budget` 记录）、
  `denied`（统计快照里 denied 增长）。
- 末行汇总：calls、总 wall、final、failed、各 rc 类别计数。
  **rc 类别按语义分**（OK/FORMAT/RANGE/FULL/IO），读数不归并
  （STATUS 第 47 条）。

用途：异常轮一眼定位——请求体字节突刺（REQ_CAP 前兆）、lcp% 突降
（前缀意外断裂）、rc 非 OK、wall 突刺。

### 2.2 V2 单轮上下文（双层）

`ctx_review.py <trace> --call N` 输出两层，**先字节层后逻辑层**：

```
--- byte layer（原始 body，不转述） ---
<-- req 段原样>
--- logical layer（从同一 body 解析投影） ---
system      <n> B   <正文>
tools       <n> B   <逐项：name / desc 长度>
messages:
  [0] user        <n> B   <问题段>
  [1] assistant   <n> B   tool_calls: fs.grep(args=…)
  [2] tool        <n> B   <#result 正文；含 #budget 标记的行原位保留并高亮>
  ...
--- response ---
raw:  <原始 JSON 原样>
act:  <v1 的解码段：DONE/CALL/坏块>
```

- **丢段可视化**：prompt 里被 budget 丢弃的位置只留 `#budget` 标记，
  逻辑层把标记行单独列出：`#budget dropped <n> B` + 累计账，
  与"标记之和 == 账"（v1.14.9 不变式）对得上。
- **双层同源**：逻辑层**从 body 解析**，不是从 blob 重新渲染——
  两处若不一致，说明渲染路径与落盘路径分叉，那正是要抓的 bug。
- 旧 v1 trace：只有 `in`/`act` 两段 ⇒ 只出"prompt 原文 + 解码动作"，
  明确标注 `byte layer: unavailable (v1 trace)`。

### 2.3 V3 轮间 diff 与前缀重合度

`ctx_review.py <trace> --diff N [M]`（默认 M=N+1）：

```
lcp = 1920 B（req_N 的 85.6%）
first diff @1920，位于 messages 尾部（正常）
middle diff: none
```

- **预期形态**：append-only ⇒ diff 只应出现在尾部。
  **中部 diff = 报警**：工具清单重排、system 被动、历史段被改写，
  三种各自有名（对照 v1.14.14"可变内容只能追加在最新处"）。
- **相同 body**：单独报 `identical`，这是"重复测量 vs 框架重发"的
  判别入口（配合 X-CXF-Ctx 的 run/call）。
- 旧 v1 trace：无 req ⇒ V3 不可用，明示。

## 3. 算法：LCP 与"前缀重合度"的口径

```python
def lcp(a: bytes, b: bytes) -> int:
    n = min(len(a), len(b))
    i = 0
    while i < n and a[i] == b[i]:
        i += 1
    return i
```

报告两个数：`lcp 字节数` 与 `lcp / len(req_N)`。
diff 区域归类：按 body 结构把偏移映射到 `system / tools / messages 头部 /
messages 尾部` 四段（解析 JSON 顶层字段的偏移区间，不是猜字符串位置）。

**口径必须写死的三条限制**（不说清就会被当成实测命中率）：

1. 这是**结构估计**，不是引擎实测。真实命中还取决于服务端是否启用
   prefix cache、KV 池驱逐、token 边界（字节 LCP 与 token LCP 在边界
   附近可差几个 token）。STATUS 第 42 条说"命中率零观测"——本工具
   补的是**结构侧**那一半，引擎侧仍缺。
2. body 的字段序 ≠ chat template 的渲染序（`agent_llm.h` 已注明
   template 总把 tools 渲在 messages 前）。本工具按 body 算 LCP，
   对"我们自己的请求稳定性"是精确判据，对"服务端 token 前缀"是
   近似代理——**前者才是本工具的职责**。
3. lcp% 高不等于成本低：尾部新增一大段时 prefill 照样贵。
   时间线上 lcp% 与 req_B 必须一起读。

## 4. `traces/` 落盘约定（顺手把上次的教训收掉）

- **目录**：`traces/`（仓库根，与 `build/` 平级）。`make clean` 不动它。
- **命名**：`traces/<YYYYMMDD-HHMMSS>-<pid>.trace`，一次 run 一个文件；
  同 run 的 call 序号在文件内递增。
- **版本**：trace 文件头带格式版本（`=== run ===` 段的 `version`），
  v1 无头即视为 v1。
- **git**：`traces/` 默认进 `.gitignore`（可能含私有语料）；
  **证据归档用 `git add -f` 显式进行**——判据沿用 STATUS §5：
  "能被一条命令重建的不进历史；trace 花过钱、不可重建"，
  归档是显式动作，不是默认动作。
- **清理**：不自动清理；复盘器只读，永不改写 trace。

## 5. 明确不做（各带触发条件）

- **不做全屏/交互**：V1~V3 覆盖复盘九成需求。需要交互时它变 TUI
  一个面板，数据源不变（trace 文件 + 本工具的解析函数）。
- **不做 token 精确计数**：没有 tokenizer，不引入；字节数 + 口径说明
  足够支撑复盘。需要精确 token 的那天，接引擎的 usage 字段（响应里
  有就展示，没有不估）。
- **不做实时 attach**：只复盘已完成的 run。实时观测走快照通道
  （控制面设计，另案）。
- **不改内核/不改请求路径**：trace v2 的写入点在 `agentctl.c` 与
  `agent_llm` 的调用边界，不进 tick 内核。
- **不自动对比不同 run**：跨 run 的 diff 有真实需求（同一问题两次跑），
  但先做单 run 三视图；跨 run 是 `--diff` 的自然扩展，等第一个用例。

## 6. 落地清单与验收

| # | 位置 | 改动 |
|---|---|---|
| 1 | `tests/agentctl.c` | trace v2：run 头 + req/resp 原样落盘 + meta 行；写 `traces/` |
| 2 | `.gitignore` / `Makefile` | `traces/` 忽略但 clean 不碰；`make live` 的示例路径更新 |
| 3 | `tools/ctx_review.py` | V1/V2/V3 + v1 降级路径 + LCP/diff 归类 |
| 4 | `tools/` 测试（与 seg_probe 同族） | 合成 fixture trace（含 FULL、fanout、连丢两条、中部 diff 各一） |

**断言**：
- fixture 上 V1 的 rc 分类计数逐格正确（含 FULL 且 req=0 的 `req_cap` 格）；
- V2 逻辑层与字节层同源：改 fixture body 的一个字节，逻辑层必须变
  （防止"逻辑层独立渲染"回潮）；
- 中部 diff fixture 必须报警，纯尾部 diff 必须静默；
- 相同 body 报 `identical`；v1 fixture 三个降级提示逐一出；
- **变异验证**：把 LCP 改成从 1 开始比、把丢段标记改成不列出，
  对应测试必须红（沿用 mutate_check 的纪律）。
