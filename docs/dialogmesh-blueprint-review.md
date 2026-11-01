# DialogMesh 蓝图/编排设计评审（面向 cxf L1）

> 对象：`aptshark-g/DialogMesh` @ `1f81f71`（2026-08-22）
> 范围：`core/agent/blueprint/`（19 文件 / 8586 行）+ `docs/DESIGN_BLUEPRINT_ORCHESTRATION.md`（662 行）
> 前作：`docs/dialogmesh-review-v2.md`（整体 delta）、`docs/dialogmesh-review.md`（旧快照）
> 目的：**这是全仓库里与 cxf L1 分解器最对得上的部分**——任务 → 能力 DAG 的生成、校验、执行、学习。
> 日期：2026-10-13

---

## 0. 结论先行

**一句话**：蓝图/编排是 DialogMesh 里**离 cxf 最近、也最有参考价值**的子系统 ——
它把 L1 的**三档自由度、四道关、tick 分层、四道保护门**都做出来了形状，
而且在两处独立收敛到 cxf 的选择；但**三处关键接线是断的**，且断法极其有教益。

**三个硬发现**（都是代码级、可复现、不是风格意见）：

| # | 发现 | 后果 |
|---|---|---|
| **1** | **`priority`（= tick 层号）由 LLM 自由填，而没有任何地方保证它与拓扑序一致** | 一张结构合法的图会**静默丢节点** |
| **2** | **候选路径枚举了，但从不校验 converge 的输出 ⊆ 候选集合** | 第三档自由度**白做**：多花一次 LLM 调用，零可验证性收益 |
| **3** | **学习闭环的终点是硬编码常量 `strategy="EXECUTION"` + 覆盖式权重更新** | 名义上接了闭环，实际**把人工先验抹平并塞进死桶** |

**该拿六条**（§4）：三档策略表与默认取中档 / diverge→learn→converge 的掩盖约束法 /
四道保护门 / 用户旋钮↔架构参数映射 / **反推验证判据「coverage 60-80%，100%=过拟合，0%=幻觉」** /
`edge.required` 与 `node.checkpoint` 把语义放进数据结构。

**该避六条**（§5）：名单式代替注册式 / `data_key` 自由字符串冒充类型化 DAG /
静默回退与静默跳过 / priority 当运行时数据 / LLM 自评 confidence / 无校验的权重覆盖。

---

## 1. 它设计了什么

`docs/DESIGN_BLUEPRINT_ORCHESTRATION.md` 是设计主文档，核心一句：

> **"LLM 是图的构建者，不是图的执行者。"**

```
SkillRegistry.match(intent) → 选策略 (5 档)
  ├─ RULE_BASED / TEMPLATE  → 直接编译模板（LLM 零介入）
  ├─ HYBRID（默认）          → 模板 floor + LLM override（加/删/改 priority）
  └─ LLM_DRIVEN             → diverge(T=0.8) → learn(检索) → converge(T=0.1)
                              → PlanGate 人工审核 → 才执行
        ↓
ConstraintChecker.validate(dag)   ← 结构/资源/依赖/数据键 四类约束
        ↓
BlueprintExecutor.execute(dag)    ← 按 priority 分 tick，tick 内并行、跨 tick 依赖
        ↓
QualityGate.evaluate → strategy_switch 事件 → MetaFeedback → SkillRegistry 权重
```

**文件地图**（19 个文件，按重要性）：

| 文件 | 行 | 职责 |
|---|---|---|
| `models.py` | 182 | `BlueprintDAG` / `BlueprintNode` / `BlueprintEdge` / `ExecutionAudit` |
| `engine.py` | 380 | `ConstraintChecker`（校验）+ `BlueprintEngine`（三策略路由） |
| `llm_dag_builder.py` | 380 | `diverge` / `learn` / `converge` 三段 |
| `executor.py` | 1253 | tick 主循环 + 13 个 chain handler + tool 并行/批量/重试 |
| `protection.py` | 251 | `PlanGate` / `Budget` / `LoopDetector` / `QualityGate` |
| `skill_registry.py` | 536 | 模板库 + intent→strategy 权重 |
| `meta_feedback.py` | 263 | 学习回写（§14.4/14.5） |
| `learning_bridge.py` | 343 | 成功路径沉淀为模板 |
| `heuristic_distiller.py` | 381 | 二阶抽象提炼（启发库） |
| `permission_engine.py` / `protection.py` | 237/251 | 权限门 + 破坏性操作判定 |

---

## 2. 与 cxf L1 的同构点（这是最值钱的部分）

### 2.1 三档自由度：**同一张表**

| DialogMesh §十 | LLM 拿到什么 | cxf 的形态（`l1-decomposer.md` §6.10 轴一） |
|---|---|---|
| **Level 1** `RULE_BASED/TEMPLATE` | 什么都不给，直接跑模板 | —（零 LLM，对应档 0/1） |
| **Level 2** `HYBRID`（**默认**） | **模板 + "下一步去哪"（单步路由，不建全图）** | **形态 `F3`：给候选让它选** |
| **Level 3** `LLM_DRIVEN` | 目标 + 自由建全图 | 形态 `F1`：自由生成 |

**它默认取中间档**，并且明说：

> **"限制 LLM 的自由度 = 提升可靠性"**（§十一，对比 LangGraph/BatchDAG/Hermes/CrewAI 后的共同结论）

⇒ **与 cxf 的四轮结论完全同向，而且它比 cxf 先走一步：把"单步路由"和"全图构建"分成了两个显式策略**，
不是一个"自由度旋钮"。**这是 cxf 可以照抄的一个具体形状**（见 §4.1）。

### 2.2 四道关：cxf 的四关，它做了一半

cxf 的提取器四关：`parse` → `resolve` → `close` → `verify`。
`ConstraintChecker.validate()`（`engine.py:41-99`）对应关系：

| cxf 关 | DialogMesh 对应 | 状态 |
|---|---|---|
| `parse` 文本→结构 | `_extract_json`（`llm_dag_builder.py:150-164`）+ `BlueprintNode.__post_init__` 校验 chain/priority | ✅ **格式错 → 返回 None → 回退，不猜** |
| `resolve` 名字→id | `data_key ∈ valid_keys`（8 个硬编码字符串）+ `chain ∈ CHAIN_IDS`（13 个硬编码） | ⚠️ **有，但是"名单"不是"注册表"**（§5.1） |
| `close` 补中间边 | **无** | ❌ 缺 |
| `verify` 结构合法性 | 真 **DFS 三色环检测**（`engine.py:136-159`）✅ / 深度 ≤ 18 / 节点 ≤ 7 / 必须有 `llm_reply` / 根必须有 `pcr` | ✅ **比预期好** |

**意外收获**：`models.validate()` 只查自环（`from_node == to_node`），
但 `ConstraintChecker._has_cycle()` 是**真正的 WHITE/GRAY/BLACK 三色 DFS**。
⇒ 我原本准备批"只查自环"，**复核后必须收回**——它补上了。

### 2.3 tick 分层：**与 cxf 的 tick × phase 同构**

`executor.py:304-308`：
```python
tick_groups: Dict[int, List[str]] = {}
for n in dag.nodes:
    tick_groups.setdefault(n.priority, []).append(n.node_id)
for tick_num in sorted(tick_groups.keys()):
```
设计文档 §14.3 的表（Tick 0: pcr+intent / Tick 1: context+subgraph+profile / Tick 2: llm_reply / async: meta+behavior）
= **tick 内并行、跨 tick 依赖**。**这正是 cxf 的 tick 语义**。

**但实现方式见 §3.1 —— 这是它最硬的那个洞。**

---

## 3. 三个硬发现（代码级）

### 3.1 ★★ `priority` 由 LLM 填，但没有不变式保证它与拓扑序一致 ⇒ 静默丢节点

**证据链（四步，缺一不可）**：

**第一步：`priority` 由 LLM 自由生成。**
`llm_dag_builder.py:70-93` 的 `CONVERGE_SYSTEM` 里，示例是
```json
{"node_id": "pcr_0", "chain": "pcr", "priority": 0, ...},
{"node_id": "intent_1", "chain": "intent", "priority": 0, ...},
{"node_id": "llm_2", "chain": "llm_reply", "priority": 2, ...}
```
而 prompt 的"规则"部分**只约束了 `data_key` 与依赖的正确性**：
> "依赖必须正确: to_node 依赖的 data_key 必须由 from_node 产出"

**对 `priority` 与依赖的关系，一个字都没有。** ⇒ LLM 无从知道 `priority` 是 tick 层号，
更不知道 `priority(u) ≤ priority(v)` 是硬约束。

**第二步：`priority` 是 tick 层号，且主循环按它推进。**
`executor.py:304-308` 按 `priority` 分组，`for tick_num in sorted(tick_groups.keys())` 逐层推进。

**第三步：层间依赖检查的是全局 `completed`，不是"上一层完成"。**
`executor.py:322-326`：
```python
incoming = dag.incoming_edges(node.node_id)
deps_ready = all(e.from_node in completed or not e.required for e in incoming)
```
`completed` 是**全局已执行集合**。所以若 `v` 在 tick 2 而它依赖的 `u` 在 tick 5，
到 tick 2 时 `u ∉ completed` ⇒ `deps_ready = False`。

**第四步：整层无进展 → 静默跳过。**
`executor.py:421-427`：
```python
if not progressed:
    for pend_id in pending:
        logger.warning("Node %s deps not ready — skipping", pend_id)
        all_outputs[pend_id] = {"status": "skipped", "error": "dependencies not satisfied"}
        tick_result.errors[pend_id] = "deps not ready"
    break
```

**⇒ 完整失败链**：
LLM 给了一张 priority 与拓扑序不符的图（一次采样的事）→
`ConstraintChecker` **不查 priority**（它查了 8 项，无一项涉及 priority）→ 校验通过 →
执行到该节点所在 tick 层，依赖没完成 → **节点被标 skipped（只有一行 warning）** →
下游连锁 skip（因为该节点不在 `completed` 里）→
`QualityGate.score` 对 `skipped` 记 0 分（`protection.py:207-208`）→
总分低于 0.4 → 判定"降级"。

**这是"未被写下的不变式"的教科书案例**：
```
不变式：对每条边 u→v，必须 priority(u) ≤ priority(v)
  未声明 ✗   未校验 ✗   违反不报错、只静默丢节点 ✗
  唯一决定它的输入：LLM 的自由生成 ✗
```

**顺带两处相关的边界不一致**：
- `BlueprintNode.__post_init__` 把 `priority` 限死 **0–9**（`models.py:47-48`）⇒ **最多 10 个 tick 层**；
- 而 `ConstraintChecker.MAX_DEPTH = 18`，宽松时可缩放到 **24**（`engine.py:39, 66-67`）。
  ⇒ **一张深度 15 的合法图，必然有 6 层要挤进同一个 priority。**
- 另外设计文档 §11 承诺 "Loop Detector: 重访已执行节点 3 次 → 强制 checkpoint"，
  代码里 `LoopDetector` 确实存在（`protection.py:137-166`），**但它计的是"同一节点被访问次数"**，
  而"跨 tick 逆序依赖"这个更常见的循环形态**根本进不到它面前**（节点在第一层就被 skip 掉了）。

#### 对 cxf 的直接含义

| | DialogMesh | cxf 应有的形状 |
|---|---|---|
| 层号从哪来 | **LLM 填的字段**（运行时数据） | **算法从拓扑序算出**（构建期事实） |
| fan-in 怎么等 | 全局 `completed` 集合 + 层内多趟重试 | `WorkItem.parent` + `pending` 计数（已在数据里） |
| 顺序不对时 | 静默 skip | **构建期拒绝**（有环/逆序 = 提炼器必须响亮失败） |

cxf 的原则"**凡是本可以提前知道的，别让它退化成运行时数据**"在这里有了最直接的对手戏：
**执行顺序本可由拓扑序在构建期一次性算出**，DialogMesh 把它做成了 LLM 的输出字段 +
执行期的 multi-pass 重试。`executor.py:313-314` 的注释自己也承认了：

> `# Multi-pass convergence: same-tick nodes may depend on each other in any definition order`
> `# (LLM_DRIVEN output is unordered).`

⇒ **它用执行期重试吸收构建期的顺序不确定性。** 这是"把不确定性往下游推"，
而 cxf 的做法是把不确定性**锁在构建期一处**。

### 3.2 ★★ 候选枚举了，但从不校验 `converge 输出 ⊆ diverge 候选`

**这就是你四轮追问的那个形状 —— 它做出来了，但做错在最后一步。**

`llm_dag_builder.py` 的三段：

| 阶段 | 产出 | 对应 cxf 的形态 |
|---|---|---|
| `diverge`（T=0.8，无上下文） | `List[Hypothesis]`，每条是 `[{chain, reason}, ...]` + confidence | **枚举候选**（形态 `F3` 的输入） |
| `learn` | arxiv/github/tools 检索结果 | 提供额外信息 |
| `converge`（T=0.1，全上下文） | **完整的 `nodes` + `edges` JSON** | **重新生成整张图** |

`CONVERGE_SYSTEM` 的原话是：
> "从多条候选路径中选择**最优一条**，输出完整的 BlueprintDAG"

**问题**：候选路径**只被拼成 prompt 里的一段文本**（`llm_dag_builder.py:276-279`）：

```python
hypotheses_text = ""
for i, h in enumerate(hypotheses):
    nodes_text = " → ".join(f"{n.get('chain','?')}({n.get('reason','?')})" for n in h.nodes)
    hypotheses_text += f"路径{i+1} (c={h.confidence:.2f}): {nodes_text}\n  理由: {h.rationale}\n"
```

然后 `converge` 的返回值 `data["nodes"]` / `data["edges"]` **直接被构造成 `BlueprintDAG`**
（`llm_dag_builder.py:322-361`），**没有任何一处把它与 `hypotheses` 比对**。

**验证**：全库检索 `hypotheses`，命中的全部是**另一个东西**
（`cli/inspect_cmd.py`、`planner/distillation_engine.py` 里的 `HypothesisEngine._hypotheses`），
**`llm_dag_builder` 的 `Hypothesis` 除 `converge()` 的参数之外零消费者**。

⇒ **这就是我上一轮给 cxf 讲的那句**：

> **"同一份约束，对 LLM 是提示，对算法是保证。"**

这里**候选集合对 LLM 只是 prompt 里的一段提示**：LLM 完全可以（而且经常会）输出候选里没有的图。
⇒ **第三档自由度的全部好处（枚举时已保证的东西）一个都没拿到**，
只多花了**一次 LLM 调用**（diverge）+ 一次检索（learn）。

**正确形状（cxf 应该这样做）**：
```
算法持有候选列表（结构化，有 id）
   → LLM 只输出 {"chosen": 2}（分类，答索引）
   → 图由算法从候选 [2] 取出
```
**LLM 的输出空间从"所有合法图"缩到"k 个索引"** —— 这才是形态 `F3`。
而 `diverge` 的产物已经足够算法直接采用（`Hypothesis.nodes` 就是一条合法路径序列），
**`converge` 那一步本该只是"选一个索引"，却做成了"再生成一次"。**

#### 附带：解析环节的"宽容"

`converge` 逐字段解析时有默认值兜底（`llm_dag_builder.py:327-329, 342-345`）：
```python
node_id=n.get("node_id", "?"),      # 缺 → "?"，多个节点可能同名
chain=n.get("chain", "intent"),     # 缺 → 默认 "intent"  ← 静默猜
...
from_node=e.get("from_node", "?"), to_node=e.get("to_node", "?"),
data_key=e.get("data_key", "data"),  # 缺 → "data"（白名单里的万能值）
```

`chain="intent"` 这个默认值**编码上合法**（∈ CHAIN_IDS）⇒ 能通过 `__post_init__` 校验
⇒ **一个 LLM 漏填 chain 的节点会被静默当成 intent 节点**。这正是 cxf 红线禁止的：
**猜错 = 静默错连**。

**公平地说，它做对了一处**：`_extract_json` 格式错时**返回 `None` 而不是猜**
（`llm_dag_builder.py:150-164`），`converge` 拿到 `None` 就回退（`:317-320`）。
**这一关是干净的。**

### 3.3 ★ 学习闭环：名义上接了，终点是硬编码常量 + 覆盖式更新

**先说好消息（不能抹）**：闭环**确实接上了**，不是文档许诺：

```
executor.py:440-443   quality = self._quality_gate.evaluate(dag, all_outputs, llm_reply,
                                                            strategy=dag.strategy, ...)
runtime/engine.py:828-831     MetaFeedback(decision_bus=...) 实例化并挂总线
llm/task_runner.py:389-399    构造 ExecutionAudit(...) → self._meta_feedback.consume(audit)
meta_feedback.py:110-115      update_strategy_weights(intent, strategy, score)
                             → self.registry.update_weight(intent, strategy, score)
skill_registry.py:520-533     update_weight(...) 真实改权重
```
四个门也都是真的（`protection.py`: `PlanGate` / `Budget` / `LoopDetector` / `QualityGate`），
且 `QualityGate.score` 是**纯算法、零 LLM**（`protection.py:177` 注释 + 代码确认）。

**但终点有三个问题，连成一条链**：

**问题 A：`strategy` 字段在唯一的生产者里是硬编码常量。**
`llm/task_runner.py:394-399`：
```python
self._meta_feedback.consume(ExecutionAudit(
    request_id=..., blueprint_id=...,
    strategy="EXECUTION",        # ← 常量，不是 dag.strategy
    dag_quality_score=score,
    ...
))
```
而 `VALID_STRATEGIES = {"RULE_BASED","TEMPLATE","HYBRID","LLM_DRIVEN","RECOVERY"}`（`models.py:25`）
—— **`"EXECUTION"` 不是其中任何一个。**

**问题 B：权重写入端不校验 strategy。**
`skill_registry.py:520-533`：
```python
def update_weight(self, intent, strategy, score):
    ...
    for w in self._strategy_weights[intent]:
        if w.strategy == strategy:
            w.record(score); w.weight = ...; return
    # New strategy for this intent
    sw = StrategyWeight(strategy=strategy, weight=1.0)   # ← 凭空造桶
    sw.record(score)
    self._strategy_weights[intent].append(sw)
```
⇒ 每次 `update_weight(intent, "EXECUTION", ...)`，若该 intent 的表里没有 `EXECUTION`
就**新建一个**。而 `EXECUTION` **永远不会被 `match()` 选中**
（`match` 选出的 strategy 必须能路由 `build()` 的 5 个分支）⇒ **分数进了死桶**。

**问题 C：权重更新是"point estimate 覆盖"，不是先验 + 观测。**
```python
w.weight = max(0.0, min(2.0, 1.0 * w.success_rate))   # 注释写的是 base weight × success_rate
```
注释说"基础权重 × 成功率"，代码是 **`1.0 × success_rate`** ⇒
`SkillRegistry._init_defaults()` 里手工调过的先验
（`"代码分析": TEMPLATE 1.0 / HYBRID 0.7 / LLM_DRIVEN 0.2`，`skill_registry.py:383-392`）
**在第一次 `update_weight` 后就被 success_rate 覆盖掉**。
而 `StrategyWeight.record` 的 `success_rate` 在 `total_count==1` 时只会是 **0.5 或 1.0**
（`success_count/total_count`，`skill_registry.py:349-358`）
⇒ **一次观测就能把先验抹平。**

**问题 D（与 §3.1 联动）：回退后策略标签被改写。**
`engine.py:268-287`：
```python
valid, errors = self.checker.validate(dag, strictness=strictness)
if not valid:
    logger.warning("Constraint check failed: %s — falling back to TEMPLATE", errors)
    dag = self._build_template(intent, blueprint)      # ← 拿到模板图（strategy="TEMPLATE"）
    valid2, _ = self.checker.validate(dag)
    if not valid2:
        dag = BlueprintDAG(..., strategy="RECOVERY", ...)   # 最小保底 DAG
if dag.strategy != "RECOVERY":
    dag.strategy = strategy        # ← 把 TEMPLATE 的图标回"你请求的那个策略"
```
`BUILTIN_TEMPLATES` 声明的 strategy 是 `TEMPLATE`/`HYBRID`/`LLM_DRIVEN`（`skill_registry.py:148,172,208,231,251,287,326`）
——**都不等于 `"RECOVERY"`** ⇒ **回退后的模板图会被贴上原请求策略的标签。**

⇒ 后果：`QualityGate.evaluate(..., strategy=dag.strategy)`（`executor.py:441`）
把**假的 strategy 名**写进 `strategy_switch` 决策事件（`protection.py:240` `before=strategy or dag.strategy`）
—— 即"白盒可回看"的治理日志里，**降级记录写的是错的来源**。
（注：这条**没有**直接污染 `MetaFeedback` 的权重，因为那条路走 `task_runner` 且硬编码 `"EXECUTION"`——
两个缺陷刚好互相挡了一下。但事件日志层面的失真成立。）

#### 对 cxf 的直接含义

1. **"闭环接上了" ≠ "闭环在学对的东西"。** 判断标准应该是：
   **写进闭环的那个 key，是不是真实执行路径的 key？** 这里不是。
2. **权重更新必须是"先验 + 观测"，不是覆盖。**
   cxf 的 `τ` 由 `correction_rate` 驱动那条建议，在这里找到了反面样板：
   **一次样本替换先验 = 把学习退化成噪声跟随。**
3. **统计量的分母不够大时不许改先验**（或退火 / Beta 后验形式）。

---

## 4. cxf 该拿的（六条，按价值）

### 4.1 ★ 三档做成"三个显式策略"，且默认取中档

不是"一个自由度旋钮"，而是 **`RULE_BASED / TEMPLATE / HYBRID / LLM_DRIVEN / RECOVERY` 五个命名策略**，
且 **`HYBRID` 是默认**（`"HYBRID"` = **模板 floor + LLM ceiling**，`engine.py:300-339`）。

⇒ **cxf 的 `plan_search` 应该同样把"档"显式化**，而不是让自由度隐式浮动：
```
档0  RULE      纯算法（TTN 可达性枚举 → 唯一解）          ← 不调 LLM
档1  TEMPLATE  命中已知模板/缓存                            ← 不调 LLM
档2  SELECT    算法枚举候选 → LLM 只答索引                  ← 默认档
档3  GENERATE  约束不完整时才让 LLM 补约束（→ 回档2）
```
**默认必须是"不调 LLM"或"只答索引"的那两档。**

> ✅ **已采纳为正本**（2026-10-14）：见 `docs/l1-decomposer.md` §6.10。采纳时补了三处本表没覆盖的东西：
> ① **轴要分开** —— 这张表是**档位**（策略枚举，按 LLM 参与度递增）；
> `l1-decomposer.md` §6.10 原有的"自由生成 / 约束下生成 / 候选上选择"那张是**形态**
> （`F1`/`F2`/`F3`，按假设空间递减）。两者是**不同的量**，混进一张表必出歧义。
> ② **"禁止自由生成"从劝告升格为结构** —— 档位的值域里**没有任何一档映射到 `F1`**，
> 于是"让 LLM 自由生成整图"**不可表达**，不再依赖"约定别这么干"。
> （但调用点上仍要再断言一次：绕过档位直调 LLM 的路径不受枚举约束。）
> ③ **默认收紧为"只取档 0/1（零 LLM）"**，而不是本表说的"不调 LLM 或只答索引的两档"——
> 后者把 `SELECT` 也算进默认，粒度太松；正确口径是"`SELECT` 需档 0/1 给不出唯一解才升"。

### 4.2 ★ diverge→learn→converge 与"掩盖约束法"

`docs/DESIGN_BLUEPRINT_ORCHESTRATION.md` §12 给了一个 cxf 完全没有的手法，它自己命名为 **"掩盖约束法"**：

> **发散 LLM（T=0.8，不给上下文）→ 收束 LLM（T=0.1，给完整上下文）**

**故意在发散阶段藏起约束**，理由是"避免上下文锁死"（`§12 发散+收束 方案对比` 表）。
⇒ 这是个**真正的设计手法**：**用"信息不对称"换取探索多样性**，而把约束的施加集中在一次收束里。
cxf 的 `plan_search` 在设计"何时问 LLM"时可以直接采用：**第一问藏约束求广，第二问给全约束求准。**

### 4.3 ★ 反推验证的判据：`coverage 60-80%；100% = 过拟合，0% = 幻觉`

`heuristic_distiller.py:6` 原文：
> `→ LLM 反推验证（coverage 60-80%; 100%=过拟合, 0%=幻觉）`

**这是本次评审最值得抄的单条**。理由：

- 它是一个**可机械判定的区间判据**（不是"好不好"）；
- 它**明确指出两个极端都坏** —— 这是 cxf 目前**缺的那一侧**：
  cxf 的 `margin` 只编码"**不确定**"（margin 小 = 疑点），
  **没有编码"过拟合"**（过度确定、自洽但错）。
- ⇒ **cxf 的疑点体系应加一类 `AMB_OVERFIT`**：
  当候选集合**退化成单点**时（`|candidates| == 1` 且 margin 极大），
  **这不是"确定"，这是"枚举不足"**。DialogMesh 用 `coverage=100%` 命中了同一件事。

另外 `heuristic_distiller.py:9-11` 的触发设计也值得拿：
> "触发 = **变化驱动**（失败/用户纠正/公理冲突/启发过时/缺公理感），**定时蒸馏仅兜底**"

⇒ **蒸馏/学习的触发是事件驱动，不是定时**。且它列了五种触发信号 ——
cxf 的飞轮目前只写了"correction"，**缺"公理冲突"和"缺公理感"**（= 约束集不足以判定）这两类。

### 4.4 ★ 用户旋钮 ↔ 架构参数的映射（且明确"不是调 token"）

`§13` 给了四个用户可调旋钮及其架构含义：

| 用户旋钮 | 架构参数 | 代码落点 |
|---|---|---|
| 探索深度 `[1-5]` | 发散 LLM 的并行分支数 | `LLMDAGBuilder.diverge(breadth)`, clamp 到 `[2,6]`（`llm_dag_builder.py:176`） |
| **验证严格度** `宽/标/严` | **约束上限缩放** | **`ConstraintChecker._scaled_limit(base, strictness, lo, hi)`**（`engine.py:101-108`）|
| 学习广度 `关/核/全` | 搜索源数量 | `IngestionPipeline.run(max_results=...)` |
| 决策模式 `自动/半自动/手动` | PlanGate checkpoint 频率 | `decision_mode → strategy` 映射（`engine.py:220-225`）|

**并且它自己论证了为什么不是调 token**（§13 末）：
> 调 token：LLM 不可控 / 用户不理解 / 只影响单次调用
> 调迭代-层级：**结构性约束（分支数 = 确定性）** / 用户理解 / 影响整个图结构

⇒ **这是"阈值写参数名不写值"（我上一轮建议）的一个更好的版本**：
**不只是参数化，而是把参数暴露成"人能理解的架构选择"。**
且 `_scaled_limit` 是**全仓库唯一一处做对的参数化**：`strictness` 从 `build()` 一路传进来，
默认在 `__init__`（`engine.py:175-192`）。

### 4.5 `edge.required` 与 `node.checkpoint`：把语义放进数据结构

```python
@dataclass
class BlueprintEdge:
    ...
    required: bool = True     # False = 可选依赖，不阻塞（executor.py:324 `or not e.required`）

@dataclass
class BlueprintNode:
    ...
    checkpoint: bool = False  # 此节点前是否需要 PlanGate 审核（写入图，不是运行期判断）
```

**`checkpoint` 是"把审批点静态化"** —— 审批点是图的一个属性，
因此**在 PlanGate 之前就能被人看到**（前端可直接渲染，§15）。
对比 cxf 的信任级：**信任级应该是能力/节点的静态属性，而不是执行期的判定**。
⇒ **`IFACE_DRAFT` 加"信任级"字段的形态，可以照这个抄。**

`required` 也值得拿：cxf 的 fan-in 用 `parent + pending` **只处理强依赖**；
`required=False` 提供"**软依赖**"（有就用，没有就用默认值），
这正好对应 cxf 的 HARD/SOFT 依赖边（cxf 已有 HARD/SOFT 的概念，但只在模块依赖层，**没进数据依赖层**）。

### 4.6 四道保护门与它们的常数

`protection.py:25-33`：
```python
DEFAULT_MAX_NODES = 7                    # 与 ConstraintChecker.MAX_NODES 对齐（但靠注释，不是引用）
DEFAULT_EXECUTION_MULTIPLIER = 4          # 执行次数上限 = 节点数 × 4
DEFAULT_LOOP_THRESHOLD = 3                # 重访 3 次 → 强制 checkpoint
DEFAULT_QUALITY_LOW = 0.4                 # 质量低阈值
HIGH_RISK_CHAINS = {"tool", "engineering", "metap"}   # 高风险 → PlanGate
```
⇒ **这就是 cxf 说的"疑点预算（可终止性）"的四个具体形态**。
`Budget.max_executions = 节点数 × 4` 这个"**相对预算**"（而非绝对次数）值得抄 ——
它随图规模自适应，避免固定常数在大小图上都不合适。

**但注意**：`DEFAULT_MAX_NODES = 7` 与 `ConstraintChecker.MAX_NODES = 7`
是**两份常数靠注释对齐**，而 `MAX_DEPTH` 在 `build()` 里会被**就地改写**
（`engine.py:226` `self.checker.MAX_DEPTH = depth`）⇒ **它们是两个可以漂移的真相源**。
（这正是我上一轮批"参数中心是文档不是依赖"的又一个实例。）

---

## 5. cxf 该避的（六条，按危害）

### 5.1 ★★ 名单式代替注册式：五处名单，彼此还不一致

| 名单 | 位置 | 条数 | 内容 |
|---|---|---|---|
| `CHAIN_IDS` | `models.py:18-22` | **13** | pcr/intent/context/subgraph/profile/llm_reply/behavior/meta/discourse/association/engineering/metap/**tool** |
| `DIVERGE_SYSTEM` 提示词内的节点类型 | `llm_dag_builder.py:41` | **8** | pcr/intent/context/subgraph/profile/llm_reply/behavior/meta（**少 5 个**） |
| `valid_keys`（data_key 白名单） | `engine.py:87-94` | **8** | route/intent_context/assembled_context/compiled_subgraph/profile_text/compass/data/anchors |
| `HIGH_RISK_CHAINS` | `protection.py:33` | **3** | tool/engineering/metap |
| `reference_map`（意图→模板） | `llm_dag_builder.py:248-254` | **5** | 代码分析/通用对话/任务规划/数据搜索/因果推理 + **子串匹配** |

**五处名单，四种条数，没有任何单一真相源。** 后果是具体的：

**`engine.py:87-94` 那段 2026-08-13 的补丁注释，自己把问题写清楚了**：
> `recall_pipeline / intent_multi_recall 模板的 recall_anchor→subgraph 边用 "anchors"（锚点注入通道）`
> `— 此前不在白名单, **模板构建恒校验失败回退最小 DAG**,`
> `"意图→召回→图扩展→回复"链路在 build 层就断掉。`

⇒ **一个正确合法的模板，因为校验器忘了往手写名单里加一个字符串，恒校验失败，
被静默回退到"最小保底 DAG"（`pcr → llm_reply`），整条召回链路在 build 层就消失了。**
**这就是"名单式校验"的必然失败模式**：名单与真实契约不同步，而失败方式是静默降级。

⇒ **cxf 的 `iface_id`（第三个 id 空间）+ 不变式 6/7/8 正是要根除这个**：
端口引用**必须已注册**（`resolve` 关）、**INT 必须有人产出**、**EXT 必须有人消费**（不变式 7/8）。
⇒ **这份带日期、带后果、带自证的案例，值得直接引用进 `docs/agent.md` 或 `l1-decomposer.md`
作为"为什么必须是注册式而不是名单式"的实证。**

### 5.2 ★★ `data_key` 是自由字符串 —— 引了 BatchDAG 说"类型化 DAG"，但没有类型

`docs/DESIGN_BLUEPRINT_ORCHESTRATION.md:31` 引前沿：
> **BatchDAG** | LLM 生成**类型化 DAG** → 确定性引擎执行 | arXiv 2607.18241

而 `models.py:57-62` 的 schema：
```python
@dataclass
class BlueprintEdge:
    from_node: str
    to_node: str
    data_key: str      # 传递的数据字段名 (如 "intent_context", "compiled_subgraph")
    required: bool = True
```
`data_key: str` —— **一个自由字符串**，语义只存在于**注释和 prompt 里**
（`CONVERGE_SYSTEM` 的"规则"第 2 条列了 6 个合法值）。

**并且校验器的"白名单"里有个万能值 `"data"`**（`engine.py:88`），
而 HYBRID 路径**无条件使用它**（`engine.py:358`）：
```python
for dep in deps:
    dag.edges.append(BlueprintEdge(dep, new_id, "data"))   # ← 恒为 "data"
```
⇒ **`"data"` 在白名单里 ⇒ 任何边都能通过 data_key 校验 ⇒ 这一道关形同虚设。**

**正确性上的区别（cxf 的立场）**：
| | 名单式（它） | 注册式（cxf 的 `iface_id`） |
|---|---|---|
| 校验回答的问题 | "这个名字**在不在**这 8 个里？" | "这条边把 A 的**输出类型**接上了 B 的**输入类型**吗？" |
| 生产者/消费者关系 | 无 | **不变式 6/7/8 逐个校验** |
| 失败方式 | 名单没同步 → 静默回退 | 端口不齐 → **注册期响亮失败** |

⇒ **"类型化 DAG" 是它引用的目标，不是它实现的现实。** 这个 gap 就是 cxf 的立足点。

### 5.3 ★★ 静默回退 / 静默跳过 / 静默吞异常（三处）

| 位置 | 代码 | 静默了什么 |
|---|---|---|
| `engine.py:268-284` | 校验失败 → 回退 TEMPLATE → 再失败 → 最小 2 节点 DAG | **一张不满足用户意图的图被当成成功返回**（只有一行 warning） |
| `executor.py:421-427` | 层内无进展 → 全部标 `skipped` + `break` | **依赖未满足的节点被静默丢弃，主循环继续往下走** |
| `engine.py:355-360` | `try: ... except ValueError: pass` | **LLM 建议的节点因 chain/priority 非法而被静默丢弃** |
| `llm_dag_builder.py:363-366` | `errors = dag.validate(); if errors: logger.warning(...)` 然后 `return dag` | **校验失败照常返回**（设计文档 §14.1 却写"失败 → 回退设计阶段重试(1次)"）|

⇒ **这是 cxf 那条红线的正面反面**：
> "提取器一旦宽容，就在**悄悄替对方承担正确性** ⇒ 猜错 = **静默错连**。"

DialogMesh 没有"补全"，但它**把非法/无法满足的东西换成合法的空壳，然后照常返回**。
**"静默降级"比"拒绝"更危险 —— 调用方无从知道。**

**注意最后一行的元问题**：设计文档 §14.1 的时序图明确画了
`B-->>B: 失败 → 回退设计阶段重试(1次)`，**而代码里没有这件事**。
⇒ **文档与实现不符**，而且方向是"文档比实现更严格"。评审文档时要看代码，不看时序图。

### 5.4 ○ 把"执行顺序"做成运行时数据（§3.1 的抽象）

`priority` 是 LLM 填的字段、tick 由它分组、层内靠 multi-pass 重试收敛
（`executor.py:313-314` 注释自陈 "LLM_DRIVEN output is unordered"）。
⇒ **本可在构建期用拓扑序一次性算出的东西，退化成运行时数据 + 重试。**

**cxf 已经有一条完全对应的原则**（`MEMORY.md`）：
> "**凡是本可以提前知道的，别让它退化成运行时数据** —— 在 tick 里查表是，用指针 chase 也是。"

⇒ 这条原则原本是用来解释"为什么 handle 比指针快"的，**在这里暴露出它是普适的**：
**执行顺序也是一种"本可以提前知道"的东西。** 建议把这条原则在文档里从"性能解释"升格为"**设计判据**"。

### 5.5 ○ `confidence` 由 LLM 自评

两处：
- `BlueprintDAG.confidence: float`（`models.py:78`）—— 注释原文 "**LLM's self-assessed confidence**"；
- `diverge` 阶段每条 `Hypothesis.confidence` 也由 LLM 给（`DIVERGE_SYSTEM` 要求输出 `confidence`）。

而这两处 confidence **都进入了决策**：
- `Hypothesis.confidence` 被拼进 converge 的 prompt（影响选择）；
- `BlueprintDAG.confidence` 存档进学习模板。

**文献依据（我上一轮引过）**：Valmeekam 一派 ——
**self-critique / 自评不可靠**，verifier 必须是外部符号层。
⇒ **cxf 的 margin 必须是"算法算出来的"**（`score(top1) − score(top2)`），
**绝不能取 LLM 自报的 confidence**。这条在 DialogMesh 里刚好是个反例：
它有一条算法侧的自评（`QualityGate.score`，纯算法零 LLM ✅），
却同时保留了 LLM 自评的 confidence，**两套并存**。

### 5.6 ○ 缓存：键里塞全文前 200 字符，无 TTL、无上限

`engine.py:245-250`：
```python
cache_key = f"{intent}:{strategy}:{template or '-'}:{text[:200]}"
if cache_key in self._strategy_cache:
    return copy.deepcopy(self._strategy_cache[cache_key])
...
self._strategy_cache[cache_key] = dag
```
- **键里含 `text[:200]`** ⇒ 命中率接近 0（自然语言输入前 200 字符几乎不重复）；
- **截断到 200** ⇒ 前 200 字符相同、后面不同的**不同输入会假命中同一张图**；
- **`self._strategy_cache` 是实例 dict，无 TTL、无容量上限** ⇒ 长跑内存单调增长。

**两头都错**：粒度太细（不命中）+ 截断（假命中）。
⇒ 我上一轮建议 cxf 的缓存粒度是"**同一 task 家族 = 同一份约束**"，
对照这个反例正好说明**为什么"一张具体图"不是正确的缓存单位**。

---

## 6. 对 cxf 的具体动作

| # | 动作 | 依据 |
|---|---|---|
| 1 | ✅ **已落地** —— **`plan_search` 的档位显式化**：`档0 RULE / 档1 TEMPLATE / 档2 SELECT(LLM 只答索引) / 档3 GENERATE`；**默认只取档 0/1（零 LLM）**；并加一条结构性不变式：**「LLM 自由生成整图」没有对应档位**（不可表达，而非不推荐）。落点：`docs/l1-decomposer.md` §6.10（双轴：形态 `F1/F2/F3` × 档位 0-3）+ `docs/agent.md` §4.6/§10.7 | §4.1；DialogMesh 默认 `HYBRID` 且 §十一 结论"限制 LLM 的自由度 = 提升可靠性" |
| 2 | ✅ **已落地** —— **候选集合必须由算法持有、LLM 只输出索引**；**禁止"给候选让 LLM 重新生成整图"**（已从劝告升格为结构：`F1` 在档位枚举里不可表达；调用点上仍需断言"输出里不能出现候选集外的 id"） | §3.2；`docs/l1-decomposer.md` §6.10 已写入并附真实反例 |
| 3 | **疑点体系加一类 `AMB_OVERFIT`**：`\|candidates\| == 1` 且 margin 极大 = 枚举不足，不是确定 | §4.3 的 `coverage 100% = 过拟合` |
| 4 | **疑点预算采用"相对预算"**：执行次数上限 = 节点数 × k（而非绝对次数）+ 四道门的形式（节点数 / 环 / 预算 / 质量） | §4.6 |
| 5 | **`iface_id` 的注册式校验要写进文档并引用这个案例**：名单式（5 处名单 4 种条数）+ `"data"` 万能值 + 2026-08-13 那个"合法模板恒失败"的补丁自证 | §5.1/§5.2 —— **这是"为什么必须是注册式"的最好实证** |
| 6 | **"凡是本可以提前知道的，别让它退化成运行时数据"从性能解释升格为设计判据**，并加一条推论：**执行顺序（tick/层号）必须由拓扑序在构建期算出，不能是 LLM 的输出字段** | §5.4；`priority` 静默丢节点 |
| 7 | **软依赖进数据依赖层**：`edge.required = False` 的形态（HARD/SOFT 目前只在模块依赖层） | §4.5 |
| 8 | **信任级作为节点/能力的静态属性**（抄 `node.checkpoint`），而不是执行期判定 | §4.5 |
| 9 | **权重/τ 的更新必须是"先验 + 观测"**，禁止 point estimate 覆盖；**统计分母不足时不许改先验** | §3.3 问题 C |
| 10 | **闭环的自检：写进闭环的 key 必须是真实执行路径的 key**（反例：常量 `"EXECUTION"` + 不校验导致凭空造桶） | §3.3 问题 A/B |

**第 2 条和第 5 条是本次评审最该马上落到文档里的。**

> **进度（2026-10-14）**：第 **1、2** 条已落地 —— 正本在 `docs/l1-decomposer.md` §6.10
> （§0 结论先行加了一句引述）；`docs/agent.md` §4.6 改为指针、§10.7 第 1 项补上
> `plan_search` 签名里的落点。第 **5** 条待办。
> **⏸ 剩余待办随 L1 一并暂缓（2026-10-15）** —— 主线改为先做 agent 运行时多轮闭环；
> 理由与恢复条件见 `docs/l1-decomposer.md` §0 顶部状态块。

---

## 7. 怎么复现本文全部结论

```bash
cd /tmp && rm -rf dm-new
git clone --depth 50 https://github.com/aptshark-g/DialogMesh dm-new && cd dm-new

# §2.2 环检测是完整的三色 DFS（不是只查自环）
grep -n "_has_cycle" -A 24 core/agent/blueprint/engine.py

# §3.1 priority 是 LLM 填的 + 校验器不查它 + 执行层按它分层 + 无进展就静默跳过
grep -n "priority" core/agent/blueprint/models.py core/agent/blueprint/engine.py
sed -n '304,308p;322,329p;421,427p' core/agent/blueprint/executor.py

# §3.2 候选只进 prompt，没有任何地方校验 dag ⊆ hypotheses
grep -rn "hypotheses" core/agent/blueprint/

# §3.3 闭环终点的 strategy 是常量
sed -n '389,399p' core/agent/llm/task_runner.py
sed -n '520,533p' core/agent/blueprint/skill_registry.py
sed -n '268,287p' core/agent/blueprint/engine.py

# §5.1 五处名单
grep -n "CHAIN_IDS" -A 5 core/agent/blueprint/models.py
sed -n '41p' core/agent/blueprint/llm_dag_builder.py
sed -n '87,94p' core/agent/blueprint/engine.py
sed -n '33p' core/agent/blueprint/protection.py
sed -n '248,254p' core/agent/blueprint/llm_dag_builder.py

# §5.2 data_key 自由字符串 + "data" 是万能值
sed -n '51,63p' core/agent/blueprint/models.py
sed -n '348,360p' core/agent/blueprint/engine.py
```

> 注意：本机 shell 的 `grep --include` 与 `rg` 在该仓库上**偶发静默返回空**（且会 SIGTERM 137）。
> 本文所有"零消费者 / 不存在"类结论均**双通道复核**过；`sed -n` 段落引用不受影响。
> 本轮曾据此误判过一次（以为 `_has_cycle` 缺失、以为 `StabilityScorer` 无调用方），均已复核更正。
