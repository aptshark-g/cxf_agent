# 会话 harness 与图编排：四条并发路线，以及 cxf 的定位

> 触发：用户纠正「那个掩盖是**启发链**的，适合并发，但也要考虑**命中率**」
> 对比对象：DialogMesh 蓝图（图编排） / **Pi** / **Hermes Agent** / **OpenClaw** / harness 类
> 材料：本机 `~/Desktop/pi`（`earendil-works/pi`）、`~/Desktop/openclaw` 为**本地源码实测**；
> Hermes 为**网络二手资料**（标注见 §9）；DialogMesh 为 `1f81f71` 快照
> 日期：2026-10-13

---

## 0. 结论先行

**一句话**：这四者不是"同一种东西的不同实现"，而是**两种架构家族 + 四条并发路线**。
DialogMesh 蓝图是唯一的"**先出图再跑**"，另外三家都是"**循环里逐步决策**"，只是各自用不同手段
解决同一个问题：**并发从哪来、上下文怎么不爆、能力怎么沉淀**。

| | 家族 | 并发从哪来 | 上下文怎么不爆 |
|---|---|---|---|
| **DialogMesh 蓝图** | **图编排**（plan-then-execute） | **图的拓扑**（同 tick fan-out） | 上下文装配链（context/subgraph 链） |
| **Pi** | 会话 harness | **AgentLane**（准入/排队/挂起恢复） | **compaction + branch summarization** |
| **Hermes** | 会话 harness | **子 agent + 脚本 RPC**（把 N 轮压成 1 轮） | 五层记忆 + FTS5 + 选择性召回 |
| **OpenClaw** | 多通道助理 harness | **subagent + task registry** | 会话记忆 + dreaming 压缩 |

**三条最值得记住的**：
1. **命中率有三种互斥策略**（§4）：**稳定前缀**（提高命中）/ **精确哈希**（提高精度）/
   **消灭上下文**（根本不需要命中）。Pi 还多一条：**识别"一次性 prompt"并主动不写缓存**。
2. **Hermes 的 `agentskills.io` 标准（SKILL.md）已被 11+ 家采用** ——
   而 DialogMesh 的 `ToolRegistry` 与 cxf 的 `CapDef` **都没有可移植格式**。
   ⇒ **cxf 的能力 manifest 应当兼容 SKILL.md，而不是自造**（§5）。**这是本次对比最有生态价值的一条。**
3. **cxf 应落在"图编排"这一侧，但吸收 harness 的耐久性工程**，不要抄 harness 的循环（§7）。

---

## 1. 先修正一处：**"掩盖"有两个作用域**，我上一版记错了

用户纠正得对。我把 `§12 掩盖约束法`（蓝图文档里的"发散不给上下文"）当成了唯一形态，
**漏掉了它的原生形态在启发链里，而且两者的"掩盖对象"根本不是同一个东西**：

| | 蓝图链（`DESIGN_BLUEPRINT_ORCHESTRATION.md` §12） | **启发链**（`heuristic_distiller.py:29-47`） |
|---|---|---|
| 名字 | "掩盖约束法" | **`DIVERGE_VARIANTS`（四种变体）** |
| **掩盖谁** | **上下文**（发散阶段不给完整上下文，避免被锁死） | **样本字段**（只给一部分，让 LLM 猜缺失的） |
| 具体形态 | 发散 LLM `T=0.8` 无上下文 → 收束 LLM `T=0.1` 全上下文 | `commonalize`（找共同机制）/ **`forward_mask`（只给结果，猜前置机制）** / **`reverse_mask`（只给前置，猜后果）** / `far_transfer`（A 域→B 域远迁移） |
| 目的 | 避免上下文锁死，保住探索多样性 | **制造归纳假设的多样性**（样本遮蔽 = 迫使模型补全结构） |
| 文献锚 | 自己的命名 | Gentner 结构映射 / Gick & Holyak 类比迁移 / Barnett & Ceci 情境维度 / Holyak & Thagard 三约束 |

⇒ **同一个信息论手法（故意遮蔽信息以换取多样性）在两个不同层次上各用了一次。**
用户说的"是启发链的"，指的是**原生形态在这边，而且这边的用法更彻底**：
蓝图链只遮"上下文"，启发链遮的是"**样本本身的字段**"（只给结果或只给前置）。

### 1.1 为什么它"适合并发"

启发链的蒸馏管道天然可并行，且**彼此独立**：

```
取材 ─┬─ commonalize ─┐
      ├─ forward_mask ─┤
      ├─ reverse_mask ─┼─→ 收敛（暴露全上下文, T=0.1, 筛选 + 拒绝理由）
      └─ far_transfer ─┘        → 反事实扩展 → 反推验证 → 沉淀进 HeuristicInventory
```

**4 个变体 × N 个样本 = 天然并行**，且**没有依赖边**（不像蓝图链要拓扑排序）。
⇒ **这是与瓶颈链完全不同的并发形态**：蓝图链的并发是**拓扑算出来的**（有多少个 fan-out），
启发链的并发是**组合出来的**（变体数 × 样本数）。**后者才是"适合并发"的那个。**

### 1.2 但命中率怎么算 —— 这是用户真正指出的问题

**朴素做法（4 个变体各发一次调用）在命中率上是亏的。** 量化一下：

```
命中收益 ≈ 稳定前缀长度 × 并发分支数 × 缓存命中单价
净成本   ≈ 不稳定部分 × 并发分支数 × 全价
```

- **`DIVERGE_VARIANTS` 的 4 段文本是常量** ⇒ system 段稳定 ⇒ **那部分命中** ✔
- **但样本内容每次不同** ⇒ user 段不命中
- 而 `coverage 60-80%` 的判据（`heuristic_distiller.py:6`）意味着**要跑多次才能判定**
  ⇒ **并发把"不命中的那部分"放大 k 倍**

⇒ **净效果：命中率被稀释。** 只有"**稳定前缀占总 prompt 的比例足够高**"时，
掩盖+并发才划算；否则并发只是把不命中的部分按分支数放大。

**三条可操作的修法**（按收益排序）：

1. **★ 一次调用出 k 个结果，而不是 k 次调用。**
   DialogMesh 的 `diverge` 就是这么做的 ——
   `llm_dag_builder.py:177-178`：`prompt = f"...请生成 {b} 种可能的执行路径。"`，
   **一次调用返回整个 `List[Hypothesis]`**。**比 k 次调用在命中率上严格更优**
   （同样的稳定前缀，只付一次）。
   ⇒ **启发链的 `DIVERGE_VARIANTS` 应该照这个改：把 4 个变体拼成一次调用的 4 个输出槽。**
2. **把变体差异从 prompt 中间挪到末尾**（前缀稳定优先，对上 PREFIX_CONTRACT 的铁律 1
   "P0..P3 只追加、不插入、不重排"）。
3. **区分"一次性 prompt"**（见 §4，Pi 的做法）。

**顺带一条 DialogMesh 自己的反例**：它的 `_strategy_cache`（`engine.py:245-250`）键里塞了
`text[:200]` ⇒ 命中率≈0，且截断 200 会让不同输入**假命中**。
⇒ **它一边做了很好的 `PREFIX_CONTRACT`（前缀稳定），一边把应用层缓存做成了逐字缓存。**
两件事没有统一到同一套"什么算同一份输入"的判据上。

---

## 2. 四个系统的架构坐标

| 维度 | **DialogMesh 蓝图** | **Pi** | **Hermes** | **OpenClaw** |
|---|---|---|---|---|
| 定位 | 自增长的**认知运行时** | **coding agent harness**（终端） | 自进化**通用 agent** | 个人/**团队助理** |
| 语言 / 许可 | Python / MIT | TypeScript / MIT | Python / MIT | TypeScript / MIT |
| 规模（本地实测） | 1419 py / 264k 行 | 12 个包，agent 包 24.3k 行 | —（未本地） | src 17405 文件 / extensions 9989 |
| **决策粒度** | **整张图**（一次出 DAG） | **一步**（ReAct 循环） | **一步**（ReAct 循环） | **一步** |
| 核心新词 | `BlueprintDAG` / `SkillRegistry` / `DecisionBus` | **`Session` / `Harness` / `AgentLane` / `compaction`** | `SKILL.md` / 五层记忆 / `Honcho` | `subagent` / `task registry` / `approval policy` |
| 编排概念 | **有**（DAG + tick + 四道门） | **无**（本地检索确认，见 §3.1） | 弱（子 agent + 脚本） | 弱（subagent + tasks） |
| 权限模型 | `PermissionEngine` + `FileSandbox` + `HIGH_RISK_CHAINS` + PlanGate | **明确不做**（README 原文） | 有（含被绕过的历史，见 §6） | **Security 列为 Priority 1** |
| 学习产物 | `LEARNED_TEMPLATES`（图蒸馏为模板） | `skills/` 目录 + `docs/skills.md` | **SKILL.md（agentskills.io 标准）** | `skills/` + `custodian-skills/` |

**两处高置信度的事实**（我本地验证过，不是二手）：

- **Pi 的 README 明确写**：*"Pi does not include a built-in permission system for restricting
  filesystem, process, network, or credential access."* ⇒ 它把安全**推给容器**：
  Gondolin 扩展（micro-VM）/ Plain Docker / OpenShell 三种模式。
- **Pi 自己划定了词汇边界**（`packages/chord/PLANNING.md`）：
  *"The following terms must not become Chord concepts: **Session, Harness, AgentLane**,
  server, client, attachment, TUI, model, tool, hook, provider credential, or workspace.
  Those belong to consumers."*
  ⇒ **所以 "harness" 在 Pi 里是一个明确定义的东西**：会话 + lane + 工具 + 钩子 + 上下文管理，
  **且它被有意排除在"application-neutral 基础层"之外**。这正好回答了"harness 是什么"。

---

## 3. 四条并发路线

这是四者最本质的差异。**同一个需求（同时做多件事），四种完全不同的机制**：

### 3.1 路线一：**图的拓扑**（DialogMesh 蓝图）

并发是**静态的、可预先算出的**产物：
`BlueprintExecutor.execute()` 按 `node.priority` 分 tick，同 tick 内节点并行、跨 tick 依赖保证
（`executor.py:304-308`）。**并发度 = 图的 fan-out**。
⇒ 优点：**可预知、可校验、可缓存**。缺点：**§3.1 那个 priority 不变式没人管 ⇒ 静默丢节点**
（见 `docs/dialogmesh-blueprint-review.md`）。

**Pi 侧对照**：我本地检索 `packages/` 下的 `DAG` / `topological` / `blueprint` / `taskGraph`，
**只命中 `packages/chord`**（`facets/host.ts` + `PLANNING.md`）——
而 Chord 是**插件/服务的组合运行时**（plugin host / service token / replicated state / symmetric RPC），
它的"图"是**服务依赖图**，**不是 agent 任务图**。
⇒ **Pi 的 agent 核心没有任务 DAG 这个概念。**

### 3.2 路线二：**AgentLane**（Pi）

`packages/agent/src/harness/runtime/lane.ts`（**2012 行**）+ `harness/agent-harness.ts`。
从它导出的类型能看出这是一套**运行时的操作裁决机制**，不是静态图：

```ts
AgentLane, LaneExecutionInfo, LaneSnapshot, LaneConfigEventPayload
QueueResult, CancelQueuedResult, ResumeResult, SuspendedRun
OperationAdmission, OperationAdmissionResult, OperationRequest
NavigationResult, AbortRequestResult, DriveOptions, DriveResult
LaneBusy, InvalidLane          // ← 错误类型里有"lane 忙"
```

关键词：**准入（admission）/ 排队（queue）/ 挂起恢复（suspend-resume）/ 取消（cancel）/ 导航（navigation）**。
⇒ **并发是"运行时通道 + 裁决"，不是"预先算出的层"。**
且 `packages/agent/src` 里 **`subagent` 出现 0 次** ⇒ **Pi 的并发单位是 lane，不是子 agent。**
（用户记忆里的 Pi "Codemode（模型写脚本并发调工具）"我在 `src` 里没找到同名词，
但 `packages/coding-agent/docs/rpc.md` 存在 —— RPC 模式是 headless 嵌入，
**不是**"模型写脚本调工具"。这条我保留怀疑，见 §9。）

### 3.3 路线三：**对话分支**（Pi，且是 harness 独有的）

`packages/coding-agent/docs/compaction.md` 原文列出两种摘要机制：

| 机制 | 触发 | 用途 |
|---|---|---|
| **Compaction** | 上下文超阈值 / `/compact` | 压缩旧消息，腾出上下文 |
| **Branch summarization** | **`/tree` 导航** | **切换分支时保留上下文** |

⇒ **harness 把"并发/多路径"表达为"对话树的分支"**，而不是图的 fan-out。
`branch-summarization.ts` 的存在说明**分支是一等公民**。
⇒ 这与 cxf 的"分支"完全不同：cxf 的分支在**能力图**上，Pi 的分支在**对话历史**上。

### 3.4 路线四：**子 agent + 脚本 RPC**（Hermes / OpenClaw）

- **Hermes**（官方 README 原文）：*"**Delegates and parallelizes** — Spawn isolated subagents
  for parallel workstreams. **Write Python scripts that call tools via RPC, collapsing
  multi-step pipelines into zero-context-cost turns.**"*
  ⇒ **两个手段**：① 隔离子 agent（并发工作流）；② **脚本化 = 把 N 轮工具调用压成 1 轮，
  中间结果不进上下文**。
  ⇒ **第二条不是并发优化，是"上下文优化"** —— 它是唯一**从根上消灭"需要被缓存的 token"**的路线。
- **OpenClaw**：本地 `src/` 里能看到 `tasks/harness-owned-subagent-task.ts`、
  `status/status-subagents.runtime.ts`、`tasks/task-registry.maintenance-projection.test.ts`、
  `tasks/task-executor-policy.ts`、`talk/agent-consult-runtime`。
  ⇒ **子 agent 任务 + 任务注册表 + 执行策略 + agent 互相咨询**。
  名字里那个 *"harness-owned"* 很精确：**子 agent 任务由 harness 拥有/管理**。

### 3.5 四条路线的判断表

| 路线 | 代表 | 并发度怎么定 | 何时可预知 | 代价 |
|---|---|---|---|---|
| 图的拓扑 | DialogMesh 蓝图 | 图结构（fan-out） | **构建期** | 需要不变式（它缺） |
| Lane | Pi | 运行时准入裁决 | 运行时 | 状态机复杂（lane.ts 2012 行） |
| 对话分支 | Pi | 用户/agent 主动分叉 | 交互期 | 分支间的上下文要各自维护 |
| 子 agent + 脚本 | Hermes / OpenClaw | 显式 spawn / 脚本内 | 决策时 | 隔离成本 + 权限面扩大（§6） |

---

## 4. 命中率：三种互斥策略（用户问的重点）

| 策略 | 做法 | 代表 | 适用 |
|---|---|---|---|
| **A 稳定前缀** | 让 prompt 的前 N 个 token 尽量不变（去噪/排序/前置） | **DialogMesh `PREFIX_CONTRACT`** | prompt 长且结构固定 |
| **B 精确哈希** | 按"编译后上下文"的 sha256 做缓存键 | DialogMesh `X-Context-Hash` | 同一上下文反复出现 |
| **C 消灭上下文** | 脚本化：中间结果不进上下文 | **Hermes**（"zero-context-cost turns"） | 多步流水线 |
| **D 一次性 prompt 不写缓存** | **识别出不会被复用的 prompt，主动禁用 cache 写** | **Pi**（见下） | 压缩/摘要类调用 |

**D 是 Pi 独有的，也是最容易被忽略的一条。** `docs/compaction.md` 原文：

> *"Compaction and branch-summary requests use **fresh routing session IDs** and,
> where supported by the provider, **disable prompt-cache writes** because these one-off
> prompts are unlikely to be reused."*

⇒ **它把缓存当成本项双向管理**：不只是"怎么提高命中"，还有"**怎么不浪费一次写**"。
**对 cxf 的含义**：`τ` / 权重之外的**第三个参数面是缓存预算**，
而"哪些调用值得写缓存"本身是个**可判定的分类问题**（一次性 vs 可复用），
不需要 LLM 判断。

**四种策略之间的关系**（重要）：
```
A 提高命中率（分子）
B 提高命中精度（减少假命中）
C 把分子和分母一起砍掉     ← 唯一"降维"的做法
D 减少无谓的分母（不写不会被读的缓存）
```
⇒ **C 与 A 是竞争关系**：如果脚本化把 N 轮压成 1 轮，那 A 优化的对象根本不存在了。
⇒ **因此 cxf 的顺序应该是 `C > A > B`**：先问"这个调用能不能不出现在上下文里"，
再问"能不能让它命中"，最后才是"用哈希精确匹配"。

**而掩盖/变体技术（§1）命中的是 A**：它**有利于 A**（system 段稳定），
**但不利于并发经济性**（放大不命中的部分）⇒ **这正是用户说"也要考虑命中率"的实质**。
**结论：把 k 个变体折叠成一次调用的 k 个输出槽（§1.2 修法 1）。**

---

## 5. 学习闭环：图蒸馏 vs SKILL.md（**最有生态价值的一条**）

| | DialogMesh | Pi | Hermes | OpenClaw |
|---|---|---|---|---|
| 沉淀物 | `BlueprintDAG` → `LEARNED_TEMPLATES`（图） | `skills/` + `docs/skills.md` | **`SKILL.md`（agentskills.io 标准）** | `skills/` + `custodian-skills/` |
| 格式 | Python dataclass + JSON 持久化 | 目录 + markdown | **markdown + YAML frontmatter** | 目录 + markdown |
| **可移植** | ❌ 自造 | ❌ 自造 | ✅ **跨 11+ 框架** | ❌ 自造 |

**Hermes 的这条标准是关键事实**（二手资料，`openwalrus.xyz` 的调研文）：
`agentskills.io` 的 skill = 一个含 `SKILL.md` 的目录，YAML frontmatter 只有 `name` / `description`
为必填，正文是 markdown（建议 <5k tokens），可选 `scripts/` / `references/` / `assets/`。
**已被 11+ 工具采用**：Claude Code、Cursor、GitHub Copilot、Gemini CLI、VS Code、Amp、
Goose、Roo Code、Kiro、Codex、OpenCode。

⇒ **这意味着"能力注册表"的跨框架事实标准已经出现了，而它是一份 markdown + YAML。**
⇒ **DialogMesh 的 `ToolRegistry`、cxf 的 `CapDef` 都没有可移植格式 —— 这是一个真实的缺口。**

**对 cxf 的具体建议（有观点的判断）**：
cxf 的 `CapDef` 是**编译期 C 宏 + 生成注册物**，不可能直接变成 markdown。
但 cxf §7 飞轮承诺的"**明文脚本 + manifest + git**"那一层**可以且应该**：
```
capability/
  SKILL.md          ← 兼容 agentskills.io（人可读、跨框架可用）
  manifest.yaml     ← cxf 扩展：in_iface[] / out_iface / trust / cost   ← 机器校验的承重部分
  impl.c            ← 或 script.py
```
⇒ **`SKILL.md` 给生态，`manifest.yaml` 给 cxf 的端口签名校验。**
**"兼容一个已存在的标准"比"自造一个更好的格式"收益大得多** ——
因为 cxf 的护城河是**机器可校验的端口契约**（`docs/dialogmesh-review-v2.md` §5），
而**格式的生态位不需要 cxf 去打**。这条不用额外成本就能拿到。

---

## 6. 安全：四家对照（有一份真实的漏洞清单）

| | 权限在哪 | 边界靠什么 |
|---|---|---|
| **DialogMesh** | `PermissionEngine` + `FileSandbox`（COW/diff）+ `HIGH_RISK_CHAINS` + PlanGate | 自有实现 |
| **Pi** | **明确不做** —— README: *"does not include a built-in permission system"* | **容器化三选一**（Gondolin micro-VM / Docker / OpenShell） |
| **OpenClaw** | **Security 列为 Priority 1** + `exec` 的 approval/allowlist 策略 + `security/` 目录；`taxonomy.yaml` 里有 `security.approval-policy-approval-denial` 覆盖率 ID | 策略 + 白名单 + 通道隔离 |
| **Hermes** | 有信任机制（曾被绕过，见下） | 六种执行后端（local/Docker/SSH/Daytona/Singularity/Modal） |

**Hermes 的实据（二手，来源为百度百科词条 + 360 公司通报）**：
2026-08-25，360「图龙锋」团队在 Hermes 中发现多项严重/高危漏洞，**类型覆盖**：
**任意命令执行、信任机制绕过、沙箱隔离失效、越权访问、服务端请求伪造（SSRF）、供应链攻击**。
⇒ **"信任机制绕过"与"沙箱隔离失效"是并列的一类** —— 这对 cxf 的信任级设计是直接证据：
**信任级如果只是运行期判定，就存在"被绕过"的攻击面；做成静态属性 + 注册期机器校验，
绕过面就被压到"注册期"这一个点上。**

**顺带一条 OpenClaw 值得抄的工程制品**：`taxonomy.yaml` —— 一份**成熟度记分卡**，
带 `process_version`、`snapshot.source_ref`（精确到 commit）、以及**命名化的覆盖率 ID**
（如 `agent-runtime.tool-safety-controls`、`security.redaction-personal-redaction`），
并按 `profiles` 组合成 `smoke-ci` / `personal-agent` 等验证档。
⇒ **"能力/特性 → 覆盖率 ID → 验证档"** 这个结构，
比 cxf 现在"测试绿"的表述**更可审计**（能回答"哪条能力由哪个 profile 证明"）。

---

## 7. cxf 的定位判断（有观点）

**这两条路线的赌注不同：**

| | harness 系（Pi / Hermes / OpenClaw） | 图编排系（DialogMesh 蓝图 / cxf） |
|---|---|---|
| 把不确定性放在 | **LLM 的循环里**，harness 只负责**围起来** | **构建期**，用结构把它**消掉** |
| 承诺 | 上下文不爆（compaction）/ 会话不丢（session）/ 权限不越 / 并发不乱（lane） | 图可校验 / 可缓存 / 可蒸馏 / 可审计 |
| 赌注 | **模型能力继续涨** ⇒ 循环越强越好，harness 越薄越好 | **可验证性 > 自由度** |
| 证据 | Pi 的贡献指南：*"Do not preserve backward compatibility unless the user asks"*（薄、快、不留包袱） | DialogMesh §十一：**"限制 LLM 的自由度 = 提升可靠性"** |

⇒ **判断：cxf 应落在"图编排"这一侧**，理由是 cxf 的三个目标
（**超高性能 / 白盒性 / 分层上手难度**）全都是**结构性质**，不是"模型能力"性质 ——
harness 路线下这三条目标无从体现（循环里的行为是运行时的、不可静态验证的）。

**但必须吸收 harness 的耐久性工程**，这四家在这四件事上比 DialogMesh 成熟得多：

| 要吸收的 | 从谁 | 为什么 cxf 需要 |
|---|---|---|
| **compaction 式上下文有界** | Pi（`compaction.md` + 分支摘要） | cxf 的 arena 帧栈有界，但**跨 tick 的上下文累积**没答案 |
| **lane 式并发通道**（准入/排队/挂起恢复/取消） | Pi（`lane.ts` 2012 行） | cxf 的 SPSC ring 只解决**单生产者单消费者**；lane 的"取消/恢复/导航"是 ring 没有的语义 |
| **一次性 prompt 不写缓存** | Pi | cxf 的缓存预算面（§4-D） |
| **覆盖率 ID + 验证档** | OpenClaw（`taxonomy.yaml`） | cxf"测试绿" → "**哪条能力由哪个档证明**" |
| **SKILL.md 兼容** | Hermes（agentskills.io） | cxf 能力 manifest 的生态位（§5） |

**明确不要抄的**：harness 的**循环本身**（ReAct while-loop）。
cxf 的 tick × phase 已经把"一步决策"变成了"一批决策"，
退回逐轮循环会把**调用期零查找**和**确定性 tick**两个立身之本一起丢掉。

---

## 8. 对 cxf 的动作（6 条）

| # | 动作 | 依据 | 优先级 |
|---|---|---|---|
| 1 | **启发链的 `DIVERGE_VARIANTS` 折叠成"一次调用 k 个输出槽"**，而不是 k 次调用 | §1.2 —— 命中率被并发稀释 | ★★★ |
| 2 | **缓存的取舍顺序定为 `C > A > B`**（消灭上下文 > 稳定前缀 > 精确哈希），并加 **D（一次性 prompt 不写缓存）** | §4 | ★★★ |
| 3 | **能力 manifest 兼容 `SKILL.md`（agentskills.io）+ cxf 专属 `manifest.yaml` 放端口签名** | §5 —— 生态位不需要自造格式 | ★★★ |
| 4 | **能力 → 覆盖率 ID → 验证档**（抄 OpenClaw `taxonomy.yaml` 的结构） | §6 | ★★ |
| 5 | **信任级必须静态化**：Hermes 的"信任机制绕过"与"沙箱隔离失效"并列 = 直接证据 | §6 | ★★ |
| 6 | **并发原语补 lane 的三个语义**：取消 / 挂起-恢复 / 导航（ring 没有） | §7 | ★ |

---

## 9. 材料出处与置信度（重要）

**本地源码实测（高置信，我自己验过）**：
- `~/Desktop/pi` @ `9661762`（`origin = earendil-works/pi.git`）：
  README、`AGENTS.md`、`packages/agent/src/harness/**`、`packages/coding-agent/docs/{rpc,compaction}.md`、
  `packages/chord/PLANNING.md`
- `~/Desktop/openclaw`：`VISION.md`、`taxonomy.yaml`、`src/tasks/*`、`src/status/status-subagents*`；
  规模计数（src 17405 / extensions 9989 / apps 2527 文件）
- DialogMesh `1f81f71`：`core/agent/blueprint/**`、`docs/DESIGN_BLUEPRINT_ORCHESTRATION.md`、
  `docs/only/reference/**`

**网络二手资料（中置信，未本地核实，**引用时请勿当作实测**）**：
- Hermes Agent：`github.com/NousResearch/hermes-agent` README（经搜索结果摘录）、
  `openwalrus.xyz` 调研文（五层记忆 / `agentskills.io` / 六后端 / Atropos）、
  百度百科词条（360 漏洞通报 / 争议事件 / 星标口径）
- **星标数各来源口径不一**（CSDN 61K+ / espressio 121K / 百度 14 万+），**不作准**
- 我不对本节二手事实的准确性背书；若需据此做决策，建议本地 `git clone` 后核实

**一处存疑**：用户记忆中的 Pi "Codemode（模型写脚本并发调工具、中间结果不进上下文）"
我在 `packages/*/src` 里**没有找到同名机制**。
Hermes 的官方描述里明确有等价物（"write Python scripts that call tools via RPC,
collapsing multi-step pipelines into zero-context-cost turns"）。
⇒ **这条我标为"待核实"**，不写进结论。
