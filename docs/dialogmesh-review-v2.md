# DialogMesh 新版评审（delta）

> 对象：`https://github.com/aptshark-g/DialogMesh`（**网页 404，git 协议可读**，见 §0）
> 快照：`1f81f715` / 2026-08-22 / `feat: B-3 固化前缀（编译器侧）`
> 前作：`docs/dialogmesh-review.md`（评审对象 `APTShark/DialogMesh` @ `0c1d49f6` / 2026-07-13）
> 评审目的：只取与 cxf 相关的东西 —— 新版验证了什么、推翻了我上一版哪条、cxf 该拿什么、该避什么。
> 日期：2026-10-13

---

## 0. 访问问题（先解决上一问）

**结论：不需要你做任何事，git 协议本来就能读。**

| 通道 | 结果 |
|---|---|
| `github.com/aptshark-g`（网页） | **404** ← 账号被标记后对匿名访客隐藏 |
| `github.com/APTShark`（网页） | 200 |
| `api.github.com/rate_limit` | core: limit 60 / **remaining 0**（我出网 IP 的匿名配额，与你的账号无关） |
| **`git ls-remote https://github.com/aptshark-g/DialogMesh`** | ✅ **成功**，返回全部 ref |
| **`git clone`** | ✅ **成功** |

⇒ **被标记只影响网页与 API 的匿名可见性，不影响 git 智能 HTTP 传输。**
以后这类"账号被标记但我需要看"的情况，直接 clone 即可，无需共享凭据。

顺带：`git ls-remote` 拿到的分支比网页能看到的多 —— `main` / `codex/r1-r7-feature-complete` /
`topic/tbd-restore` + 一批 `refs/pull/*`。

---

## 1. 新版是什么（事实层）

| 项 | 旧版 @07-13 | 新版 @08-22 | 倍数 |
|---|---|---|---|
| 最后提交 | `0c1d49f`（blog 第 2 章） | `1f81f71`（**B-3 固化前缀**） | — |
| `.py` 文件 | 589 | **1419** | 2.4× |
| 源码行数 | 111,372 | **264,446** | 2.4× |
| `.md` 文件 | 117 | **607**（`docs/` 内 583） | 5.2× |
| 测试函数 | — | **3,276** | — |
| `core/agent/` 子包 | — | **87** | — |
| 定位 | "语义上下文驱动的对话记忆引擎" | **"自增长的认知运行时"**（v6） | 换赛道 |
| 语言 | Python 3.9+ | Python 3.11+，且新增 **Rust**（`persistence_rs/`） | — |

**性质变化**：旧版是"一个记忆引擎 + 一堆设计文档"；新版是**完整产品**——网关（`gateway/`，
9+ 厂商 + 断路器 + 加权路由）、前端（`frontend/`，TS/React）、召回（`recall/`，HyDE/BM25/BGE-M3）、
元认知（`meta/`）、持久化（Python + Rust 双实现）。

**这不再是同一件东西的下一版，是同一作者的第二次尝试，而且这次真的在跑。**

---

## 2. 我上一版结论的存废

上一版留了三条批评、三条该拿、三条别拿。逐条对账：

### 2.1 撤回一条（它做到了）

> 旧版批评：「15+ 自由常数且**零标注数据**；真问题是**没有能校准它的数据**，且**宣称自优化却无评估闭环/无基线**。」

**这条必须撤回。** 新版建了完整的评测闭环，而且是我要求的那种（**带基线、记负结果**）：

- `scripts/claim_eval.py` —— **claim 级**评测，RAGAS 口径：`Context Recall` /
  `Faithfulness` / **`幻觉率 = 1 - Faithfulness`**；有 gold set（`scripts/recall_goldset.py`）
  与标准文档（`docs/only/recall/RECALL_EVAL_STANDARDS_20260810.md`）。
- `scripts/eval_100.py` + 消融矩阵。
- **负结果被记录下来了**——这是最难得的一条。commit message 原文：
  - `HyDE→BM25 词项扩展实测负（开关默认关）`
  - `K1 全负 / K3 doc+3.3pp 但 dialogue-7.7pp, 默认关`
  - `12 组融合消融矩阵（**基线局部最优**）`
- **修了一个"测量混淆"**：`DM_SPO_LLM_JUDGE 隔离（修测量混淆）` —— 知道自己污染了对照，去隔离。

**记负结果 + 承认基线已局部最优 + 修测量混淆**，这三件事一起出现，说明评测不是装样子。
旧版那条批评，凭这三条我就得撤回。

### 2.2 一条变形但未解决（而且更隐蔽了）

> 旧版批评：「参数体系：15+ 自由常数、零标注数据。」

新版做了两件看起来正确的事：
1. **给每个参数加"文献锚 + 有界区间"**（`core/agent/adaptive_parameter.py:46-70`）——
   `anchor from Stanza UAS ~88%` / `ACT-R` / `ESMM-MMOE` / `BGE-small MTEB` / `行为心理学 1:2-3 比例`。
   这直接把"自由常数"降格成"**有界扰动**"，是对的。
2. **建了活的参数中心**（`core/agent/compiler/parameter_registry.py`），docstring 写着：
   > *"All tunable thresholds ... live here. **No hardcoded values in individual modules.**"*

**但是**：

| 事实 | 证据 |
|---|---|
| 活的参数中心确实被读 | `kernel/dispatch.py:797` / `behavior/scheduler.py:22` / `rewarder/time_decay.py:7` 等都 `get_registry()` |
| 但 `0.6` 在**三处独立硬编码** | `compiler/stability_scorer.py:8` `MIN_STABILITY = 0.6`；`compiler/models.py:53` `stability >= 0.6`；`v3_2/un_use/integration.py:187` `stability < 0.6` |
| 参数中心里**根本没有这些键** | `grep "stability\|drift\|correction" compiler/parameter_registry.py` → **空** |
| 另有一套 `CALIBRATOR`（带文献锚那套）**是死的** | 全库引用者只有 `core/agent/v3_2/un_use/parameter_registry.py`（且注释自陈 "CALIBRATOR global remains untouched"） |

⇒ **新版不是"参数没标定"，是"标定体系搭了三套、活的那套没被用、死的那套最像样的那套"。**

**门槛升级的批评**：旧版是"常数没有出处"；新版是"**出处的架子搭好了，但决定行为的地方绕开了它**"。
后者更危险 —— 它**看起来已经解决了**。同一条 `0.6` 在三处各写一遍、互不引用，
说明"参数中心"在团队认知里是**一个文档**，不是一个依赖。

### 2.3 一条完全成立，且量级恶化

> 旧版批评：「文档纪律是最大负资产（117 个 md、四代并存、两个 v4）。」

| 项 | 旧版 | 新版 |
|---|---|---|
| `.md` | 117 | **607** |
| 代际并存 | v3_0 / v3_2 / v3_common / v4 | 之上再加 v3_legacy、v6 品牌、`un_use/` |
| `core/agent/` 子包 | — | **87** |

新版试过一个正确的姿态：**另起 `docs/only/`（234 个）当"唯一可信集"**。
但总量还是 583，等于给失控加了个标签，没治本。

**新增一类更硬的纪律问题：`un_use/` 制度。**

```
core/agent/v3_0/un_use       core/agent/v3_2/un_use
core/agent/v3_common/un_use  core/agent/v4/un_use
core/agent/topic_tree/un_use
un_use/                      （根目录，含 v32_legacy_archived / main_legacy / engine_legacy）
frontend/src/un_use          frontend/src/components/un_use
```

- `core/agent/` 下 **5 个** `un_use` 目录；
- 全库 `*/un_use/*` 下 **114 个 `.py`**（占 1419 的 8%）；
- **直接提交进主干**，不是分支、不是 tag、不是删除。

⇒ **"不用的东西改个名字留着"是一种自我欺骗**：它骗过了 grep"还有没有人引用"，
但骗不过"新人读代码要读它"。**死代码的代价是阅读半径，不是磁盘。**

### 2.4 一条已修（加分）

旧版 §4.3 记过"README 里 clone URL 是错的（`aptshark-g`）"。新版把快速开始整个重写了
（commit：*"README 快速开始改造 — 修 GitHub 用户无法启动根因"*），
现在 README 里已无 clone 段，指向 **`docs/SETUP.md`** + `scripts/setup_env.py` 一键装。
README.zh-CN.md 也重写为 126 行，**没有坏代码围栏**（旧版那条问题不复现）。

---

## 3. cxf 该拿的（按价值排序）

### 3.1 ★ `PREFIX_CONTRACT`：与 cxf 提取器红线**同构**（第二次外部验证）

最后一次提交（08-22）是整个仓库里**最像工程制品**的东西。契约文档 49 行，
`docs/only/recall/PREFIX_CONTRACT_20260822.md`，含分层映射 / 铁律 / 验收 / 待办。

分层（P0-P4）对齐网关角色分段，**铁律五条**，其中两条对 cxf 直接有用：

> **铁律 2**：时间戳/uuid/trace_id/request_id/session_id **只进 P4 或 header** —— 进前缀 = 每次命中失效。
> **铁律 5**：**网关禁止改字节**；检测到漂移打 `prefix_drift_detected_total`，**不"修复"**。
> 分工原则：**编译器拥有前缀, 网关拥有局部性**。

**铁律 5 就是 cxf 那条红线**：

| | DialogMesh | cxf |
|---|---|---|
| 声明 | 网关禁止改字节，检测到漂移只打指标，**不修复** | 提取器一旦"宽容"补全，就在**悄悄替 LLM 承担正确性** ⇒ 猜错 = 静默错连 |
| 共同点 | **检测与修复必须分离；发现异常的人不许顺手"帮忙"** | 同 |

⇒ **"发现者不许自动修" 是两条独立设计线都收敛到的同一条**，可以升格为 cxf 的不变式候选。
它也是**你 relay-gateway 的正题**（前缀命中 ← 去噪 + 稳定指纹），
`core/agent/compiler/prefix_layout.py` 那份实现（`strip_volatile` / `normalize_stable_prefix` /
`stable_fingerprint` + 5 例 golden 0 漂移断言）值得直接看——**这是你缓存命中问题的一个已完成参考实现**。

### 3.2 ★ `HybridCompiler`：`undefined` 作为一等输出（形状对了，出口断了）

`core/agent/compiler/hybrid_compiler.py` 是个三步流水线，**跟你这四轮收敛出的 L1 形状一模一样**：

```
Step 1  LLM 伪拆解（草稿，不是权威）        llm_pseudo_parser
Step 2  规则引擎补全约束（算法侧）          rule_engine.refine / resolve_all
Step 3  稳定性评分 → undefined?            stability_scorer
```

`ParseResult` 带一个**独立字段 `undefined`** —— 三档输出里的第三档，**不是异常、不是失败**。

更值钱的是它**升级方向选对了**：全维度低置信时走的是
`slots = self.rule_engine.resolve_all(slots, ctx)`（**回到符号/规则层**），
不是"换个更强的模型再问一遍"。这与 cxf 的"疑点先归算法，不是先归更大的模型"同向。

**但形状对了，出口是断的** —— 这恰恰是 cxf 最该引以为戒的一点：

| `undefined` 的消费方 | 位置 | 干了什么 |
|---|---|---|
| `compiler/models.py:53` | `is_reliable` | 与硬编码 `0.6` 并列 → **又一处硬编码** |
| `hybrid_compiler.py:65` | 主干 | 只 append 一个字符串 `"stability_below_threshold"` |
| `v3_common/integration_bridge.py:206` | 主干 | 只是**序列化输出** |
| `v3_2/un_use/integration.py:160,187` | **`un_use/`** | ✅ **唯一真的据此改变行为的代码——在死代码区** |

⇒ **判完"无法定义"之后，主干里没有任何东西因此改变行为。**
第三档输出存在，但**第三档没有出口**。

**这条直接给 cxf 的 `plan_search` 立了个反面样板**：我上一版写的
"疑点必须结构化、必须有消费方（第一版可以是人）"——DialogMesh 证明了**不做这条会怎样**：
它是**死胡同**。⇒ cxf 的 `plan_search` 第一版必须让疑点**打印出来给一个真人看**，
而不是返回一个没人读的字段。

### 3.3 两级级联的两个可直接抄的机制

`core/agent/tiered/cognitive_compiler.py`（101 行）的 `Tier` 声明里有两个字段值得抄：

```python
Tier(level=0, name="cognitive.rule_only",
     confidence_threshold=0.75, confidence_threshold_param="cognitive.rule_threshold",
     time_budget_ms=20)                       # ← 阈值走参数中心
Tier(level=1, name="cognitive.full",
     confidence_threshold=0.6,  confidence_threshold_param="cognitive.full_threshold",
     time_budget_ms=2000)                     # ← 时间预算
```

1. **`confidence_threshold_param`**：阈值**不写值，写参数名**。这是"参数中心"唯一正确的用法——
   声明期引用符号，取值期查表。**注意：这里做对了，而 `StabilityScorer` 那三处 `0.6` 做错了**
   （同一个仓库里两种做法并存 = 纪律不一致，不是设计矛盾）。
2. **`time_budget_ms`**：每级带时间预算（20 ms / 2000 ms）。cxf 的 tick 是确定性的，
   但**层级预算**这个思路可以要——它把"该升到哪一级"从纯正确性判断变成"正确性 + 预算"的联合判断。

---

## 4. cxf 该避的（按危害排序）

### 4.1 ★ 主次颠倒：把规则当降级、把 LLM 当主路径

`TieredCognitiveCompiler` 的 docstring 直说：

> `TieredCognitiveCompiler: `**`degradation (_rule_only) → full LLM pipeline`**``
> `Tier 0 (rule_only): DegradationManager.rule_parse()  — pure rule, sub-ms.`
> `Tier 1 (full):      HybridCompiler.process()         — LLM + rule engine + scorer.`

**规则是"降级"，LLM 是"full"。** 这跟 cxf 的设计**方向相反**：

| | DialogMesh | cxf |
|---|---|---|
| 默认路径 | LLM 起手 | **算法起手**（TTN 枚举候选，正确性在枚举时已保证） |
| 便宜层 | 规则（降级/保命） | **算法（主路径）** |
| 贵层 | LLM（主力） | **LLM（兜底，只在疑点处）** |
| 依据 | confidence 不够就升级 | **margin ≤ τ 才问** |

**这个颠倒是有代价的，而且它自己已经付了**：
- 因为 LLM 在主路径，`undefined` 出口的缺失才致命（主力产出无法判定的结果 → 死胡同）；
- 因为规则在降级位，`PosteriorCorrector`（累积证据防抖）这种**符号层机制反而没人接线**（见 §4.2）。

⇒ cxf 别学这个主次。**但要注意**：README 里讲**工作流规划**时它方向是对的——
*"编排器先查已知工作流，没有就让 LLM 现场生成"*（模板优先 → LLM 兜底）。
⇒ **同一个仓库里，规划层方向对、解析层方向反**。这不是"它错了"，
而是**"LLM 放主位还是兜底"这个决定会渗透到每一个子系统，不做全局决定就会各写各的**。
cxf 需要的正是这样一个**全局决定** —— 但决定的内容要改：不是"LLM 永不当大脑"，
而是"**LLM 的角色必须显式选定、全局一致，且决策必须落成数据**"
（`agent.md` §1，v1.9 修正；原文的"LLM 不是 agent 大脑"是把可选项当成了禁令）。

### 4.2 `PosteriorCorrector`：死代码 + 阈值常数

`core/agent/compiler/posterior_corrector.py` 是**累积证据再动手**的机制：
同方向 drift 累积到 `DRIFT_THRESHOLD = 2` 才改节点归属（`should_correct`）。
**这是好东西**（防单次噪声触发纠正），但：

- **全库零调用方**（`class PosteriorCorrector` 之外没有任何引用）→ 死代码；
- `DRIFT_THRESHOLD = 2` 是**类常量**，不走 `CALIBRATOR`、不走 `parameter_registry`；
- `self._drifts[-10:]` 硬编码窗口 `10`。

⇒ 这是 §2.2 那条的**具体病理切片**：最像样的符号层机制因为没接线而失效，
而它恰好是"疑点应该累积而不是单次触发"这条正确直觉的实现。

**对 cxf 的用处**：`plan_search` 的疑点判定（`margin ≤ τ`）**单次判定就够吗？**
DialogMesh 的做法提示另一个选项：**同一个疑点重复出现 → 才升级**。
cxf 的 `WorkItem.tries` 已经有计数的位置，可以把"重试 N 次仍疑"作为升级条件之一。
**但别抄它的常数**——τ 仍应由 `correction_rate` 驱动（见 §5）。

### 4.3 `un_use/` 制度：与 cxf"类型永不卸载"直接冲突

见 §2.3。对 cxf 的具体含义：

- cxf 宪法说**类型永不卸载**（生命周期冲突是 `iface_id` 不复用 `cxf_type` 的硬理由）。
  这条**本身没问题**，但它隐含"类型表会单调增长"。
- DialogMesh 的 `un_use/` 展示了单调增长的**坏结局**：**用"改名"代替"删除"**，
  114 个 py 永久占用阅读半径。
- ⇒ cxf 需要一条配对的纪律：**类型永不卸载，但能力必须可卸载，且卸载后索引与入口必须真的消失**
  （这不冲突——类型是稳定契约，能力是易变实现）。
  `cxf_table` 支持卸载这件事，**不要让它退化成"留着但标记不用"**。

### 4.4 `stability = mean(ci) * (1 - variance(ci))`：把两种不确定性乘成一个标量

`stability_scorer.py:20`。这个式子的两个因子是**两种不同的不确定性**：

- `mean(ci)` 低 = **整体都没把握**（证据不足 / 都不知道）
- `variance(ci)` 高 = **内部有分歧**（有人很确定、有人很确定地反对）

**乘法把它们混成一个标量，丢掉了可定位性**：
`mean=0.5, var=0` 与 `mean=1.0, var=0.5` 得分相同（都是 0.5），
但前者是"大家都不确定"，后者是"两派打得很凶"——**该做的事完全不同**。

⇒ cxf 的 `margin = score(top1) − score(top2)` 是**更好的形式**：
它天然保留"**是哪两条在争**"，因此疑点**可定位**——
这正是我上一版推的"疑点四种来源 AMB_GOAL/IMPL/PATH/ARG"能落地的前提。
DialogMesh 只有一个 `undefined` 布尔，**位宽不够**，所以它没法把疑点按来源分类。

**但也别全否**：它的 `MIN_STABILITY = 0.6`（`is_undefined`）说明它至少有一个"够不够格"的门。
cxf 缺这条门——`plan_search` 第一版 τ 恒等于"无疑点"，
**意味着没有任何门槛**，会让"永远不产生疑点"变成默认行为。**要早点给 τ 一个初始值，别留空。**

---

## 5. 对 cxf 文档的直接动作

| # | 动作 | 依据 |
|---|---|---|
| 1 | `docs/agent.md` §7 飞轮：补一条**"LLM 产出的能力必须过注册期机器校验，不能只靠测试绿"** | DialogMesh 的自修闭环只靠 `pytest` 绿 + `git apply -R` 回滚（`SELF_REPAIR_DESIGN` §七）。cxf 的端口契约是**机器可判**的，**这是 cxf 相对它的真实优势**，要显式写下来别浪费 |
| 2 | `IFACE_DRAFT`（隔离态）：**触发条件不变**，但新增一条理由 | §3.2：DialogMesh 证明"有第三档输出、无消费方 = 死胡同"。`IFACE_DRAFT` 升级的唯一途径是"有人消费它"，这个设计的正确性被反证了 |
| 3 | `plan_search`：**疑点必须带消费方**，第一版就是人（CLI 打印） | §3.2 |
| 4 | `plan_search`：**τ 不能留空**，要有初始值 | §4.4 |
| 5 | `docs/agent.md` §7：飞轮顺序确认为 **先收紧符号层 → 再补规则能力 → 最后才让 LLM 写工具** | §4.1/§4.2：DialogMesh 把 LLM 放主位，导致它最像样的符号层机制（`PosteriorCorrector`）没人接线 |

**第 1 条是这次评审最实质的收获**：DialogMesh 的自修是**文本层（unified diff）+ 测试绿**；
cxf 的飞轮是**能力层（脚本 + manifest）+ 类型契约机器校验**。
cxf 的校验强度严格更高，因为**它的"经验"被结构化成了端口签名**。
这条要在文档里钉住——**这是 cxf 相对同类项目的真实护城河**，不是修辞。

---

## 6. 一条提醒（作者身份）

新版提交者署名 `APTShark <815866785@qq.com>`，与本机账号一致；
`aptshark-g` 是你新开（且被标记）的账号。**如果 DialogMesh 是你自己的项目**——
我上一版把它当"**外部**参照"、说"它**独立**收敛到 cxf 四条"，这个认识论权重是要打折的：
**同一作者的设计直觉会自洽，不构成独立佐证。**

但仍有价值，且理由变了：
- **时间上早于 cxf**（7-8 月 vs cxf 现在），不是从 cxf 抄的；
- **问题域不同**（对话记忆/认知运行时 vs 组件框架）；
- ⇒ 它是**同一个人在不同问题域上的两个独立产物**——这比"两个人撞到同一个设计"弱，
  但比"自己重复自己"强。**真正的价值不在"验证"，在"它踩过的坑你不用再踩"**
  （§4 那四条全部属于这一类）。

上一版 `docs/dialogmesh-review.md` 作为**旧快照的评审记录**保留，不删；
它的 §2（值得肯定四条）与 §5（对照表）依然有效，§3（四条异议）按本文 §2 对账。

---

## 7. 怎么复现本文全部结论

```bash
# web 404 但 git 可读（本文 §0）
git ls-remote https://github.com/aptshark-g/DialogMesh

cd /tmp && rm -rf dm-new dm-old
git clone --depth 50 https://github.com/aptshark-g/DialogMesh dm-new   # 新版 08-22
git clone --depth 50 https://github.com/APTShark/DialogMesh    dm-old  # 旧版 07-13

# §2.1 评测闭环与负结果
cd dm-new && git log --format='%ad %s' --date=short | grep -E "HyDE|消融|归因|eval"
# §2.2 三处硬编码 0.6 / 参数中心无 stability 键 / CALIBRATOR 只在 un_use
grep -rn "0\.6" core/agent/compiler/{stability_scorer,models}.py
grep -n "stability\|drift\|correction" core/agent/compiler/parameter_registry.py   # 空
grep -rn "CALIBRATOR" --include="*.py" . | grep -v "adaptive_parameter.py"          # 只有 un_use
# §2.3 un_use 规模
find core/agent -type d -name un_use | wc -l          # 5
find . -path '*/un_use/*' -name '*.py' | wc -l        # 114
# §3.2 undefined 的消费方全在 un_use
grep -rn "\.undefined\|undefined=\|is_reliable" --include="*.py" .
```

> 注意：本机 shell 的 `grep --include` 在这个仓库上**偶发返回空**（且会 SIGTERM）。
> 本文所有结论都用 `rg` 或专用检索工具复核过；`rg` 也有静默返回空的情况，**关键结论务必双通道验证**。
