# 端云协同基础 v1

契约 ID：D-HYBRID。2026-11-01 实施前冻结，现已通过 E50 基础验收；E60云端编辑验收与E62/E63逐轮观测见末节。主模型／本地文本辅助链可运行，尚不表示通用真实模型质量或自动路由已通过。

## 范围与基线

现有 agentctl 只选一个主提供者：HTTP 或 Native。新增可选 `llm.local` 数据工具，部署者使用 `--local-config FILE` 注册独立本地配置；未给参数时不注册、不请求网络。复用原工具注册、权限、执行与标准 tool message 回流，不新建 Agent、规划器或 provider 框架。

主提供者可指向云端代理，也可使用本地／Native；E50不自动判定任务难度或选择云服务，不接云 SSE，当时未执行外部云请求。已有C HTTP传输仍不支持直接HTTPS，仅连接回环，云服务需由可信本地代理承接；E60/E62使用用户明确指定的HTTP上游测试配置，不承诺上游TLS。主提供者默认UNKNOWN并拒绝外发；范围与授权统一遵循[D-DATA](data-policy-v1.md)，不能从回环地址自动推断本地。

## 配置与权限

本地配置复用 agentctl JSON 的 api 字段，必须显式提供 url 和 model，key 可空；不接受 agent 运行策略字段。仅接受精确 IPv4 回环 `127.0.0.1`，禁止任意主机名／私网／公网、零端口、URL／认证头控制字符。配置由启动者读取一次，不跟随主端点热加载，不把主 key、主 system 或 tools 复制给本地辅助。配置内容不写公开日志。

`llm.local` 属于 IO／LLM 数据能力，参数仅是待处理文本，最多 1024 字节。没有文件、进程、网络工具，没有能力追加权限或调用自身。主模型只能提交文本，不能指定 URL／模型／key／system／工具列表。主提供者和本地提供者保持独立认证与观测。

配置模板（填入自己的回环服务URL后使用，不要直接复制主配置中的 agent 字段）：

```json
{"api":{"url":"REPLACE_WITH_LOOPBACK_URL","model":"local-model","key":"","timeout_ms":5000}}
```

## 请求、结果与预算

单次请求包含固定 system 和单条原样 user 文本，使用现有 JSON 转义函数；不将 `#call`、`#result`、CALL 等内容解释为历史事件，不复制完整主会话。设置 temperature=0、max_tokens=256，不发送 tools，不保存本地历史。

响应只接受 assistant 文本；拒绝结构化 tool_calls、空输出、NUL 和明确的非 stop finish_reason。文本中的动作／协议标记仍是普通数据，不进入 action 解码器。输出最多 4096 字节，带 `#local` 事实收据；过长或传输截断明确失败，不静默裁剪，不宣称摘要正确或可信。

每个主任务最多两次本地请求，成功／失败均消耗一次配额；缓冲区不足在网络之前拒绝。配额在任务边界重置，不在每个模型轮次重置，同时受既有 LLM 闸门和总轮数限制。发起网络后不能返回 FULL 导致框架扩容重跑；不自动重试、回落主模型或切云。超时沿现有传输的 socket 超时语义，默认 5000ms，显式值 1–60000ms；尚非整请求硬 deadline。

本地调用同步、有界，由原 Agent tick 驱动，取消仍沿现有任务边界；不声称后台并行或可中断正在进行的网络操作。临时请求／响应／结果有界，不额外持久化 transcript 或 KV。HTTP 辅助尚未绑定同一个 Native engine/session，不能把它宣传成共享物理 KV、单份端云上下文或跨模型 KV 复用。

## 数据边界和验收

仅发送主模型显式选择的文本到部署绑定的本地服务；不自动搜索文件或上传工作区。辅助结果回流主提供者，所以在主提供者为云端时，这部分结果属于其正常工具上下文；当前E51要求启动者显式选择cloud workspace外发；只给本地辅助配置不会自动授权云回流。task模式没有工具调用。不给“只到本地、不会再到云”的承诺。

验收使用两个独立回环 HTTP 桩：主模型派发工具、本地端只收到指定文本／固定 system、本地结果回到主模型、文件修改和测试仍由原工具执行。覆盖认证互不混入、没有 tools、注入标记不变成控制流、参数／超限／HTTP 错误／截断／tool_calls 拒绝、任务配额和重置、默认不注册与越权工具不执行。真实模型、端云网络、HTTPS、吞吐、调度和任务质量另测，功能通过不计作加速数据。

## E50 当前实现与待办

当前入口为 `tests/agentctl.c` 的部署注册／配额，`tests/agent_llm.c` 的单用户消息构造／纯文本响应路径；确定性验收为 `tests/test_agent_foundation.py`。8组在 sanitizer CLI 上通过，覆盖真实本地 HTTP、数据标记不执行、独立认证、配额／重置、失败不重发、写权限挂载和真实 C 编辑测试闭环；原生兼容回归另列。[功能证据](../evidence/agent-foundation-20261101.json)。

真实模型的提取／摘要质量、Native 主模型调用 HTTP 辅助的完整任务质量、直接 HTTPS、更细粒度的语义外发分类、云 SSE 语义分帧、异步调度和共享 Native session 仍待实现／验收。后续稳定前缀与准备时机按 D-PREP 维护；本契约不引入预测工具执行或第二个通用调度框架。

E51新增统一数据准入，当前外发以[D-DATA](data-policy-v1.md)为准；已知主／辅助凭据及编码表示不能进入任一提供者正文、工具结果或诊断。445项策略与12组安全套件通过，E50的8组基础套件在新策略下复验通过。E50原证据与历史限制保留。[E51功能证据](../evidence/agent-data-policy-20261101.json)。

## E62 云请求编码、权限提示与逐轮观测（2026-11-02，先冻结后实施）

目标：清理默认只读system中的整文件写入说明；仅挂载write_new或write_apply时加入该参数提醒。保留每轮单工具、实际挂载、外发准入与512字节system上限。角色称为工作区代码助手，不从执行地点推断模型地点。C协议原本已输出UTF-8；可信测试relay改用ensure_ascii=False与紧凑分隔符再编码UTF-8，JSON解析后的messages／tools／参数必须完全相同（权限提示的有意变化另列）。既有helper同步修正，不把E60旧证据重标成新代码。

观测默认不创建新的日志。显式--trace时增加仅含序号、注册能力名、结果码与单调时钟差的[timing]记录；不复制参数、文件路径、正文或认证信息。工具时间分为执行器自身与外层输入／结果策略检查；provider时间包含C请求构造、回环HTTP和解析，不能与上游等待相加。trace沿现有保护和私有保管规则；时钟失败返回零时长且标记无效，不影响业务。Native也可有provider跨度，但本轮不据此宣称Native性能变化。

测试relay逐请求记录：C→relay的JSON正文字节、实际relay→上游的JSON正文字节（不含HTTP头／TCP/TLS）、只读system字节、响应字节、HTTP状态、非流式上游发起至响应头／完整正文的等待、请求读取／解析／序列化／安全过滤／响应交付的本地耗时。API等待包含网络、服务端排队和推理，无法拆成模型纯计算；到头时间不是TTFT。provider跨度减relay完整跨度只称为C协议／本机传输残差；误差和失败保留，不将相交跨度求和。CLI整任务计时止于子进程结束，不含relay.shutdown的轮询等待。

usage只读取明确的非负整数（bool不算数）：prompt_tokens、completion_tokens、total_tokens、prompt_cache_hit_tokens、prompt_cache_miss_tokens，以及prompt_tokens_details.cached_tokens。缺字段为null，非零／零均原样保留；两个命中来源不一致标记冲突，不推算缺失命中、计费或服务器身份。总token与输入输出和不一致也保留并标记。不用请求字节推算token，不把远端缓存当本地物理KV。

冻结对照：两组短合成只读任务（直接读文件／列目录后读文件），每组1个预热对＋4个评价对，前后顺序逐block倒转，共20个真实CLI任务。两版仅默认system和relay序列化有意不同，共享相同新计时；改前源码从当前E59–61工作树冻结，不能使用缺少fs.edit的旧Git HEAD冒充。相同文件mtime／inode版本因临时工作区可能不同；版本回执字节与每轮实际token必须记录。任务固定文字、工具权限、模型、temperature=0、thinking=disabled、max_tokens=2048、预算；真实模型决定动作，不注入决定。改前／后请求形态变化和调用数变化完整登记，不强行逐轮配对不同动作。无远端cache清理能力，轮换与预热不保证冷／热状态；网关权重、缓存策略、网络漂移未验证。

同一安全合成请求另外进行离线2×3序列化回放（旧／新system × ASCII带空白／ASCII紧凑／UTF-8紧凑），区分删提示、取消转义与去空白的字节影响，记录本机序列化耗时，不能当真实API加速。验收覆盖权限提示组合／长度、UTF-8与嵌套转义等价、usage缺失／冲突／非法类型、上游失败／超限／凭据反射拒绝、实际工具计时与失败不重试。正式数据新编号、保留所有负对，旧证据和图逐字节保留；公开数据只含合成任务元数据／数值／源码哈希，不含真实端点、配置、请求或私有日志。以上为实施前冻结口径，已实施结果与边界见下节。

### E62/E63 已实施与实际结果

20/20真实云只读任务、50/50请求HTTP200，模型按要求分别只read和list→read，文件未修改；最终答案均有39和无修改权限的文字标记。64权限组合中48合法、16既有create/write重叠组合拒绝，普通及ASan/UBSan各3套、16转发／失败路径和完整默认回归通过。不是一般模型准确率。Native推理／Windows未复测，旧性能仍绑定旧源码。

每轮prompt均少18token。direct每任务上游5533→4167B，listed8663→6530B。8评价对有4个变慢，完整任务逐对中位改后多12.707／46.684ms；范围分别[-126.151,65.581]／[-12592.580,750.230]ms，没有稳定加速。listed改前block1有8.531s和7.709s上游等待，本地relay分别0.369／0.439ms，不能据此区分网络、排队或推理；负例不裁剪。评价工具执行＋外层策略的每任务中位约0.11ms（direct）／0.42–0.50ms（listed），不是编辑工具的64KiB成本。

50轮均有usage.prompt_tokens／completion_tokens／total_tokens和prompt_tokens_details.cached_tokens，专用hit／miss字段50轮都缺失，保留null。评价缓存占输入token比例before／after分别：direct61.9%／36.3%，listed17.1%／20.5%；不能声称编码提高缓存命中。字段口径参照[DeepSeek官方API](https://api-docs.deepseek.com/api/create-chat-completion/)，网关仅转发统计，无法验证真实实现；缺失值不是零。

E63为60个离线序列化block，旧／新system×三种编码，每格1预热＋9评价，每block1000次。首轮2513→1840B：空白82B、旧system下取消Unicode转义510B、UTF-8下system变化81B。旧ASCII带空白7.066us／次，新UTF-8紧凑8.325us／次，序列化墙钟增加约1.26us（perf_counter_ns，未独立测线程CPU），负结果保留；并非API等待或模型推理成本。Unicode／紧凑分隔符依据[Python JSON文档](https://docs.python.org/3/library/json.html#json.dump)，解析对象等价，不把线缆字节当token。源码／二进制哈希、全部正负对与计时字段见E62/E63，不覆盖E60旧证据。

<a id="e62-cloud-reproduction"></a>

### 复现与数据边界

2026-11-02公开展示补充：保留请求模型标识`deepseek-v4-flash-0731`、合成任务、测量值及源码哈希。逐轮／任务CSV追加`model`列，来自对应证据的模型字段；不改历史JSON或重新标定数据。公开说明不记录实际服务地址、IP简称、凭据、账号、配置来源应用或个人配置路径。复现仅使用PROFILE_FILE／MODEL_PROFILE_ID占位符；配置格式示例与敏感目录防护规则可以保留，示例不得填真实值。

```sh
# 在新目录重建同计时旧提示版本；随后明确选择部署者自己的配置文件与profile。
PYTHONDONTWRITEBYTECODE=1 python3 tools/build_cloud_observe_baseline.py --out build/cloud-before-new
PYTHONDONTWRITEBYTECODE=1 python3 tests/test_cloud_observe.py
PYTHONDONTWRITEBYTECODE=1 python3 tools/bench_cloud_observe.py \
  --models-file PROFILE_FILE --profile MODEL_PROFILE_ID \
  --baseline build/cloud-before-new/agentctl-before --candidate build/agentctl \
  --out build/cloud-observe-new
```

此命令会真实请求所选模型、20任务约50次API调用，不自动选服务／重试／清缓存。默认C权限仍关闭云外发，runner显式给合成工作区workspace授权；真实key不写入子进程配置，子进程只用回环nonce。私有run保留stdout／stderr／trace／合成请求，在新0700目录中，不可整目录公开。--trace没有开启时不增加计时日志。公开归档只选数值／模型别名／合成任务名称／哈希，token／缓存／每轮工具与API等待另存；先做凭据／端点扫描再重建白名单。离线成果包包含分析／图表与基线重建器，联网采集器仅在完整源码包中。

当前64份证据、37,290主记录、43组图；本轮新增50轮云请求与60序列化block，20任务／30工具事件为附加表，不能相加成独立任务数。此前62证据和42组图逐字节保持。本轮本地完成、未推送。
