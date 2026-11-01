# DialogMesh 设计评审

> 对象：`https://github.com/APTShark/DialogMesh`（`aptshark-g/DialogMesh` 是 README 里的旧名，会 404）
> 评审目的：只取与 cxf 相关的东西 —— 它验证了 cxf 的哪些选择、暴露了 cxf 的哪个洞、哪些别抄。
> 日期：2026-10-13

---

## 0. 结论先行

**一句话**：DialogMesh 是一个**问题摆得比答案好**的项目。它独立收敛到了 cxf 的四条设计选择
（Pull/共享数据、信任级放行、三段式升级、least-commitment），这是对 cxf 的**外部验证**；
而它暴露的**"悬空类型在 LLM 作者手下不是 bug"**这一条，是 cxf 飞轮开启前必须先补的洞。

**三条该拿走**（按价值排序）：

1. **`τ` 不该是常数，应由 correction_rate 驱动** —— cxf 现在写"τ 是唯一需要调的参数"，
   但"唯一要调的"不等于"应该是常数"。这是飞轮最便宜的第一级：**先收紧符号层（加词/加排除/动 τ），
   再考虑让 LLM 写新能力。**
2. **疑点传给 LLM 时附上算法自己的猜测**（top-1 + margin），让昂贵的层做**纠正**而不是**重新推导**。
3. **给接口类型加第三态：隔离（quarantine）** —— 不变式 7/8 的"注册期拒绝"隐含假设
   **节点作者是程序员**；飞轮开启后作者是 LLM，悬空是假设而非 bug。**这一条改动最大，也最要紧。**

**三条别拿**：它的参数体系（15+ 自由常数、零标注数据）、它的线程/事件模型（与可复现性冲突）、
它的文档纪律（117 个 md、四代并存、两个 v4）。

---

## 1. 它是什么（事实层）

| 项 | 事实 |
|---|---|
| 定位 | "语义上下文驱动的对话记忆引擎"；v4 自称三层架构（Cognitive Scheduler / Cognitive Pipeline / Semantic World Model） |
| 语言 | Python 3.9+，MIT |
| 规模 | **589 个 `.py`（约 4.5 MB 源码）、117 个 `.md`**；其中 `core/agent/` 占 531 个 |
| 版本 | `core/agent/` 下 **v3_0 / v3_2 / v3_common / v4 四代并存**（文档里还有 v3.1 / v3.3） |
| 定位（作者自述） | `docs/implementation_assessment.md` 明写对标"**本科毕业设计 ⭐⭐⭐⭐⭐**" |
| 选题 | RST 话语块（Discourse Block）用于对话上下文压缩 + 三段式"编译器" + 9 维粘合度 + v1/v2/v3 渐进压缩 / Hot-Warm-Cold 分层 |

**读法**：这是**一份体量很大的中文毕设级别研究原型**，不是 README 说的 "industrial-grade"。
这个判断不贬低它 —— 它在毕设尺度上的工作量是超额的；但它决定了后面所有批评的严厉程度标尺。

---

## 2. 设计层：值得肯定的四条（含与 cxf 的独立收敛）

### 2.1 三段式升级流水线的形状是对的

`DESIGN_MULTI_TIER_PIPELINE.md` §1 给了一条 L0→L5 的"算力-精度谱系"：
缓存 → 符号规则 → 统计模型 → 小模型 → 大模型 → **人工**，每层带 confidence 阈值，
低于阈值才升级。**这与 cxf 的 `margin = score(top1) − score(top2) ≤ τ → 疑点`
是同一个结构**，独立收敛。连"L5 = 人工"都对上了 cxf 的"第一版疑点出口可以是人"。

值得单独记的一句（该文档 §4）：

> 每层的 context 包含前面所有层的输出作为提示。下层不需要从零开始。

**这条 cxf 现在没做**，见 §6 第 2 条。

### 2.2 Pull 而非 Push，且"谁看什么"与"何时跑"分开了

`DESIGN_V4_KNOWLEDGE_REFINEMENT.md` §3.2 的设计是：Observation 上挂
`consumer_marks: { behavior: claimed, engineering: unclaimed }`，各 Analyzer **自己来挑**。
作者原话："**这是 Pull 不是 Push。没有中央调度器决定这条信息属于谁。**"

这与 cxf 的宪法"**禁止组件间直接函数调用；同步走共享 SoA 表**"是同一招 ——
共享数据 + 每个消费者自己记游标，而不是互相调用。独立收敛。

而且它把两件事分开了：**Pull 决定"谁看什么"，Cognitive Scheduler 决定"何时跑"**。
这正是 cxf 的分工（共享表决定谁看见什么 / registry + phase 决定何时）**但 cxf 的 phase 是固定四段，
DialogMesh 的调度是集中式可配**。差异见 §5 对照表。

### 2.3 Vocabulary 替代 Schema —— 这就是 cxf 的信任级放行

同文档 §2.3：标签不预设 schema，分三池 ——

| 池 | 含义 | 对应 cxf |
|---|---|---|
| Core Vocabulary | 稳定标签（constraint / monitor / intent） | 已核准能力 |
| Candidate Vocabulary | **LLM 新造的标签，待批准** | 低权自动/高权人审的注册放行 |
| Unknown Tag Pool | 临时存放，**不被任何 Analyzer 消费**，等未来升级 | **cxf 没有对应物** ← 见 §6 第 3 条 |

升级路径："LLM 产生新标签 → Candidate → 人工批准或**自动批准（高频 + 高置信）** → Core"。
**这是 cxf §7 飞轮 + 信任级放行的独立同构物。** 注意它多了一个 cxf 缺的东西：Unknown Tag Pool。

### 2.4 "分类延后"= least-commitment（与 l1-decomposer.md §6.2 同一原理）

同文档 §3.1 的核心论证："不是所有信息都能立即分类……**如果要求每条输入必须立刻决定
属于哪条链，系统马上就僵住了。**" 于是先存 Observation、暂不解释、异步精炼。

这把 `docs/l1-decomposer.md` §6.2 引的 **SNLP/UCPOP least-commitment planning**
（最小承诺，把决定延后）在"观察归类"这一层重新发明了一遍。**独立收敛，值得记一笔。**

### 2.5 附带一条：FeedbackLoop 的机制写得比 cxf 具体

`DESIGN_MULTI_TIER_PIPELINE.md` §6 说"这是所有快慢系统最缺的一环"，这句是真的。
它的机制是：Tier N+1 纠正了 Tier N → `record_correction` → 若 `correction_rate > 阈值` 则
**扩充词典 / 调整正则 / 追加排除模式**，并生成一条 Observation
（`pattern_detected=tier_N_has_systematic_errors`）。

**它比"让 LLM 重写工具"便宜两个数量级** —— 收紧符号层 vs 生成新能力。见 §6 第 1 条。

---

## 3. 设计层：四条我有异议的

### 3.1 一个判定压在 15+ 个自由常数上，且零标注数据

`DESIGN_HYPOTHESIS_ENGINE.md`：

- `BeliefState` 声明 **7 维**（support / conflict / novelty / stability / coverage / recency / entropy），
  但 `compute_belief_score`（§5.2）只用了 **5 维** —— **`novelty` 声明之后从未被任何地方使用**。
- 打分是 5 项线性加权，权重 0.35 / 0.25 / 0.20 / 0.10 / 0.10（和为 1），**无推导、无校准来源**。
- 冻结条件是 **5 维 AND 硬阈值**：`min_support=8` / `max_conflict=3` / `min_stability=0.70` /
  `min_coverage=0.40` / `min_consensus_domains=2`（§6）。
- 再加 Decay 的 `half_life_days=7.0`、三个 Belief Propagation 系数 0.3 / 0.15 / 0.1（§4.1）。

**光这一个机制就有 15+ 个自由常数，而没有任何标注数据。**

而他们自己的 `implementation_assessment.md` 已经承认同类问题：
"阈值 0.5 是**经验值，未经过标注数据校准**"、"0.55 阈值"同样未校准。
**同一个批评在假设引擎上只会更重**（那里有 15+ 个而不是 2 个常数）。

**我的判断（这条要精确说）**：问题**不是"参数多"**，是**"有没有能校准它的数据"**。
有数据，60 个参数也能拟合；没数据，1 个参数也是许愿。
DialogMesh 的真正缺陷不是 `ParameterRegistry` 里那 60+ 个参数，
而是它**宣称在自优化（AdaptiveThreshold）却没有任何评估闭环去证明自优化在往好的方向走** ——
它连一个基线都没有（§4.5）。

**与 cxf 的对照**：cxf 写"`τ` 是唯一需要调的参数"。这句话的价值不在"少"，
在"**少到可以被一件事校准**"。DialogMesh 反证了这句话为什么重要。

### 3.2 "贝叶斯信念更新"名不副实

`DESIGN_V4_KNOWLEDGE_REFINEMENT.md` 的目录第 5 节标题是
"Hypothesis Pool：**贝叶斯**信念更新"，同文档 §1.2 的注释又写"贝叶斯迭代逼近真相"。

但实现是 `support_ratio * 0.35 + stability * 0.25 + coverage * 0.20 + recency * 0.10 + entropy * 0.10` ——
**这是加权和，不是贝叶斯更新**：没有先验、没有似然、没有归一化、没有概率语义，
连"后验"这个词都没地方落。而且 `P(H|E) ∝ P(E|H)P(H)` 的乘性结构被换成了加性结构，
"多证据独立支持"这个最该被贝叶斯模型捕捉的性质反而丢了。

**"贝叶斯"这个词要节省着用** —— 它会把一个可调打分函数抬到一个它达不到的严谨性层级。
这条规矩我们在 relay-gateway 的数学评审里已经立过一次，这里是同类。

### 3.3 decay 是路径相关的（真 bug，不是风格问题）

`DESIGN_HYPOTHESIS_ENGINE.md` §3.3：

```python
belief.support = int(belief.support * decay_factor)
```

每步 `int()` 截断 ⇒ **同一段时间，按不同 tick 粒度衰减，结果不同**：

| 起点 support=5 | 结果 |
|---|---|
| 衰减 5 次，每次 ×0.99（≈ ×0.951 总效果） | 5→4→3→2→1→**0** |
| 一次 ×0.951 | 5×0.951=4.75→**4** |

**同一时长、同一衰减率，得到 0 和 4 两个答案。** 而且 `support` 声明为 `int`，
但衰减在数学上必然产生小数 —— 表示与运算不匹配。
修法很简单：**内部存 float，只在冻结/输出时取整**。

### 3.4 同一节里三套衰减公式

| 出处 | 公式 | 形状 |
|---|---|---|
| §3.3 散文 | `TimeWeight = e^(-λ × age)` | 自然指数，参数是 λ |
| §3.3 代码 | `decay_factor = 2.0 ** (-age_days / half_life_days)` | 2 为底，参数是半衰期 |
| §3.3 代码（recency） | `recency = max(0, recency - age_days * 0.1)` | **线性** |

三套不同参数化、不同形状，**没有一处说明为什么**。这不只是笔误 ——
散文与代码不一致会让"算法可替换"这句话（§5.2 的关键承诺）失去意义：
读者不知道该替换哪一个。

---

## 4. 事实层：可核实的缺陷

> 这一节每条都能按 `file:line` 复现，与设计偏好无关。

### 4.1 README 的 "lock-free ring buffer" 是假的，且背压是死码

`core/agent/v4/event_ir.py`：

| 行 | 内容 | 问题 |
|---|---|---|
| 4 | docstring: "The EventBus is a **lock-free ring buffer**" | **假** |
| 29 | docstring: "Thread-safe **ring buffer with backpressure**" | **假** |
| 34 | `self._backpressure_threshold = int(self._capacity * backpressure_ratio)` | **算出后从未被读取** |
| 35 | `self._buffer: deque = deque()` | 是 `deque`，不是 ring |
| 36 | `self._lock = threading.Lock()` | **每事件加锁** ⇒ 不 lock-free |
| 45 | `with self._lock:`（publish 内） | 同上 |
| 47 | `self._buffer.popleft()` | **静默丢弃最老的事件** |
| 49-50 | 每 100 次丢弃才 warn 一条 | 丢事件不可见 |
| 70 | `time.sleep(0.001)` | **轮询** |
| 101 | `"backpressure_ratio": pending / max(1, self._capacity)` | 是另一个临时算的比例，不是第 34 行那个阈值 |

⇒ 四个具体结论：
1. **既不 lock-free 也不是 ring**；
2. **背压完全没实现** —— 没有任何地方因背压而拒收或阻塞（字段是死码）；
3. **溢出丢的是最老的事件** —— 对一个"事件溯源 + 先说后解释"的系统，
   丢最老的正是最贵的。而它自己的 v4 设计写的是"**只追加不删改**、每条独立、
   可中断恢复"（`DESIGN_V4_CONTEXT_ENGINEERING.md` §4.1）。
   **两条设计互相矛盾，代码服从了错的那一条。**
4. 轮询 + 线程 ⇒ **不可复现、不可回放**（DialogMesh 没有这条宪法，所以只是"不可回放"；
   cxf 有，所以这条是硬冲突）。

### 4.2 README 的代码围栏全坏了

`README.md` 里三反引号数量 = **0**，以单个反引号独占一行的有 **22 行**。
所有代码块都是 `` ` `` 起 `` ` `` 止 ⇒ **GitHub 上整个 README 的代码块渲染是坏的**，
那份"三层架构图"看起来像散落的单引号就是这个原因。

### 4.3 README 的 clone URL 是错的（就是你给我的链接 404 的原因）

`README.md` 第 91 行与第 208 行都写 `git clone https://github.com/aptshark-g/DialogMesh.git`
⇒ **404**。实际仓库是 `APTShark/DialogMesh`。
你已经踩到了这一点：`aptshark-g` 这个用户本身也返回 404。

### 4.4 两个互不兼容的 "v4"

| 出处 | v4 是什么 | 状态标注 |
|---|---|---|
| `docs/ARCHITECTURE_INDEX.md` L1 | **Context Engineering：双 Compiler（Memory / Context）+ Event Log + Context IR** | "**当前主设计**"（而该文档自己标"设计草案"） |
| `README.md` | **三条线：Cognitive Scheduler + Event→Observation→Hypothesis→Knowledge→Skill + Semantic World Model** | 9 份设计文档 / 8 份已实现 / 304 测试 |

两者共享 "Event" 入口然后分叉。**同一个版本号指两个东西**，读者无法判断哪个是实现基准。
这不是文档多的问题，是**没有一个地方声明"当前权威版本"**的问题。

### 4.5 304/304 绿 vs 自评"致命缺口"

README 挂 `tests-304/304` 徽章。而 `docs/implementation_assessment.md` 第一节就是：

> 🚨 **致命缺口：端到端质量未验证** …… 我们不知道 Cold 块被压缩为 v3 摘要后，
> LLM 是否还会记得早期话题 …… **我们甚至不知道话语块树是否比逐轮堆叠更好**。
> 根本原因：缺少一个**轮级基线系统**做对比。

**测试全绿证明的是内部数据结构自洽，不是设计有效。**
这是整份自评文档里最有价值的一句话 —— 作者能写出这句，说明他知道真正的缺口在哪。
本评审的 §6 三条就是把资源往这个方向挪的建议。

### 4.6 全库无法核实的精度表

`DESIGN_MULTI_TIER_PIPELINE.md` §1 给出 "~70% / ~85% / ~92% / ~95% / ~98%"
与 "<1ms / <5ms / 30ms / 200ms / 500ms / 无限"，**无出处、无测量、无数据集**。
结合 §4.5（端到端未验证），这张表只能是估计值。

**看着像测量结果的数字最危险** —— 它会被下游设计当作既成事实引用。

### 4.7 文档纪律是最大的负资产

| 现象 | 证据 |
|---|---|
| 四代并存 | `core/agent/` 下 v3_0 / v3_2 / v3_common / v4 |
| 半成品文件进库 | `docs/v3.0/DESIGN_SEMANTIC_WORLD_MODEL.md.part1` |
| 已关闭的历史文档仍在树里 | `design_architecture_gaps.md` 与 `design_architecture_gaps_v2.md` 均标"历史 / 已关闭" |
| 文档量 ~1/5 代码量 | 117 个 md vs 589 个 py |
| 清单落后于 README | `MANIFEST.md` 是 v3.0（2026-07-03），README 讲 v4 |

---

## 5. 与 cxf 的对照（同问题、不同选择）

| 问题 | DialogMesh | cxf | 我的判断 |
|---|---|---|---|
| 组件间通信 | Pull + `consumer_marks`，异步任意顺序 | 共享 SoA 表 + **显式 phase**（装配期定序） | 同招；**cxf 赢在可复现** |
| 何时跑 | Cognitive Scheduler（集中、可配） | registry + 固定四 phase | 同构；DialogMesh 更灵活，cxf 更确定 |
| 新节点准入 | Candidate → 人工批准或高频高置信自动放行 | 信任级放行（低权自动 / 高权人审） | **同构，可互证** |
| 悬空（无人消费）的类型 | **允许**：Unknown Tag Pool 等着被升级 | **注册期拒绝**（不变式 7/8） | **看谁造节点** ← §6 第 3 条 |
| 调参 | 60+ 参数 + AdaptiveThreshold | "τ 是唯一需要调的参数" | **取决于有无校准数据** |
| 记忆/目录更新 | Lazy merge + Patch Chain（追加 O(1)，checkpoint 批量 apply） | 每次 sync **全量 rebuild**（O(n²)） | **规模决定，不可直接搬**：n≤16 时全量重建是免费的 |
| 可复现性 | Python + 线程 + `sleep` 轮询 | C + tick + 确定性 + ASan/UBSan | cxf 为可复现付了代价，付得值 |
| 文档 | 117 个 md、四代、两个 v4 | 一份主设计 + 每轮追加一节 + 标注"已落地/未做" | **cxf 的做法是对的，别退化** |

---

## 6. 三条该拿走（含具体动作）

### 6.1 `τ` 不该是常数，应由 correction_rate 驱动

**来源**：`DESIGN_MULTI_TIER_PIPELINE.md` §5 `AdaptiveThreshold`（按 correction_rate 动态调阈值）
+ §6 FeedbackLoop（`correction_rate > 阈值` → 扩词典 / 改正则 / 加排除模式）。

**为什么对 cxf 重要**：cxf 现在把 `τ` 描述成"唯一需要调的参数"。
但**"唯一要调的"不等于"应该是常数"**。它的正确形态是自适应的：
上层纠正了下层的判断 → 记一次 correction → 该层的 `τ` 动一点。

这是 cxf §7 飞轮**最便宜的第一级**，顺序应该是：

```
1. 先收紧符号层      ← 加一个索引词 / 加一条排除模式 / 动一次 τ      （最便宜）
2. 再补一条规则能力   ← 已有能力的一个新实现
3. 最后才叫 LLM 写新工具                                            （最贵）
```

现在的 cxf 文档直接跳到第 3 步。**第 1 步不该被跳过。**

**落地形态**：`stat_correction[tier]` + 一个纯函数的阈值更新规则，
断言"τ 随 correction_rate 单调变化"——这本身是可测的。

### 6.2 疑点传给 LLM 时，附上算法自己的猜测

**来源**：`DESIGN_MULTI_TIER_PIPELINE.md` §4 那句话 ——
"每层的 context 包含前面所有层的输出作为提示。**下层不需要从零开始。**"

**cxf 现状**：`Ambiguity.cand[]` 给的是候选（带渲染后的名称 + 描述），
但**没给"本层倾向于哪个"**。

**该加的**：`top1` 的 `cat_id` + `margin` 数值。理由不是"多给点信息"，
而是**把任务类型换掉**：

| | 任务 | 难度 |
|---|---|---|
| 现在 | "给定输入，你会选哪个？" | 从零判断 |
| 加上猜测 | "我倾向 A（margin 0.03），对吗？" | **识别/纠错** |

后者是 LLM 明显更强的一档（也是 §6.9 说的"让 LLM 学表格分类器"的同一个方向）。
成本是 `Ambiguity` 结构体加两个字段。

### 6.3 给接口类型加第三态：隔离（quarantine） ← **最重要的一条**

**来源**：`Unknown Tag Pool: 临时存放，不被任何 Analyzer 消费，等待未来升级`。

**它暴露了 cxf 的一个隐含假设**：`catalog_rebuild` 的不变式 7/8
（`INT` 必须有人产出 / `EXT` 必须有人消费）之所以能"**注册期响亮失败**"，
是因为它假设**节点的作者是程序员** —— 程序员写出悬空类型，那是 bug。

**但 cxf §7 的飞轮是让 LLM 注册能力。** 此时：
LLM 声明一个新接口类型、暂时没人消费它，**不是 bug，是假设**。
按现在的规则，这次注册会被**直接拒绝** —— **飞轮被自己的不变式掐死。**

**建议的第三态**：

```c
typedef enum IfaceKind {
    IFACE_EXT   = 0,   /* 外部输入：无人产出，由 INJECT 提供 */
    IFACE_INT   = 1,   /* 能力产出：必须至少被一个能力产出 */
    IFACE_DRAFT = 2,   /* 隔离：声明了但尚未接入 —— LLM 作者态 */
};
```

规则（四条）：

1. `DRAFT` **可以被声明、也可以被能力产出**，但**不参与 `plan_search`**（不可达）；
2. `DRAFT` **不满足不变式 7/8 也不报错**，只记 `stat_quarantined`；
3. 它升级为正式 `INT`/`EXT` 的**唯一途径是有人消费它**（或人批准，走 §7 信任级）；
4. 升级发生在**注册期**（与现有不变式同一时机），**不引入运行期状态机**。

**为什么这值得动宪法级不变式**：它保住了"响亮失败"的**语义**
（针对人类作者依然是错误），同时给 LLM 作者留了合法中间态。
**不变式的目的从来不是禁止状态，是禁止"没人知道的"状态。**

**注意触发条件**：这条只在**飞轮开启时**才需要。当前第一刀没有 LLM 作者，
所以**现在不动**，只记在案 —— 到"真实 LlmApi 原位替换 mock"那一步再落。

---

## 7. 三条别拿

### 7.1 别拿它的参数体系

60+ 参数 + 5 项加权打分 + 5 维 AND 阈值，在**没有标注数据和评估闭环**时
是 15+ 个未验证假设的乘积。

cxf 该保持的形态是：**每个参数必须能指向一个具体的、能跑出来的断言**。
DialogMesh 自己缺的那个"轮级基线"，恰恰是 cxf 已经有的东西 ——
测试里每个数都是一条可复现的对照（`stat_injected == 7` 而不是"覆盖率提升了"）。

### 7.2 别拿它的线程/事件模型

`threading.Lock` + `deque` + `sleep` 轮询 + **静默丢最老事件**，
与 cxf 的"确定性 + 可复现 + tick 边界同步"直接冲突。

要借鉴 lazy merge 可以，但**只能在 tick 边界内做，不能引入后台线程**。
而且注意 §5 对照表那条：cxf 的 `catalog_rebuild` 是全量 O(n²)，
但 **n ≤ CAP_MAX = 16 且只在注册期跑** ——
DialogMesh 需要 lazy merge 是因为它有 50000 节点的图。**这条不可直接搬。**

### 7.3 别拿它的文档纪律

117 个 md、四代并存、两个 v4、`.part1` 文件提交进库、
已关闭的 gaps 文档还留在树里。

cxf 现在的做法是**对的**：一份主设计（`docs/agent.md`）+ 每轮讨论追加一节
+ 明确标注"✅ 已落地 / 未做" + 关键判断钉进不变式与测试。
这是可审计的形态，**不要为了"写得更全"退化。**

---

## 8. 附：本次评审核实过的数字与引用

| 结论 | 出处 / 命令 |
|---|---|
| 589 个 `.py`，约 4.5 MB；117 个 `.md`；`core/agent/` 占 531 个 | GitHub tree API（recursive） |
| `aptshark-g/DialogMesh` 与 `aptshark-g` 用户均 **404** | `curl -o /dev/null -w '%{http_code}'` |
| README 三反引号 = **0**，单反引号独占行 = **22** | `grep -c '```' README.md` / `grep -c '^\`' README.md` |
| README clone URL 错（第 91 / 208 行） | `grep -n "git clone" README.md` |
| `_backpressure_threshold` 算出后从未被读 | `grep -n backpressure core/agent/v4/event_ir.py`（4 处，仅 34 行是赋值） |
| EventBus 非 lock-free、非 ring | `event_ir.py:4,29,35,36,45,70` |
| 溢出策略 = 丢最老事件 | `event_ir.py:47` |
| `novelty` 声明后未使用 | `DESIGN_HYPOTHESIS_ENGINE.md` §5.1 vs §5.2 |
| decay 路径相关 | `DESIGN_HYPOTHESIS_ENGINE.md` §3.3 的 `int()` |
| 端到端未验证 / 缺轮级基线 | `docs/implementation_assessment.md` §三 🚨 |
| 阈值未校准（0.5 / 0.55） | `docs/implementation_assessment.md` §三 🟡 |
| 两个 v4 定义 | `docs/ARCHITECTURE_INDEX.md` L1 vs `README.md` |

---

## 9. 一句话收口

**DialogMesh 的答案不能直接用，但它把问题摆得够宽** ——
它独立收敛到了 cxf 的 Pull/共享数据、信任级放行、三段式升级、least-commitment 四条
（外部验证），也暴露了 cxf 飞轮前必须先补的洞（`IFACE_DRAFT`）。
**取它的形状与问题清单，不取它的参数、线程与文档。**
