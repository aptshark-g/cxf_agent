# 原生聊天／工具提供者 v1

E58实验宿主新增agent_native_prepare_thinking显式模式：仍用上游Qwen3模板，thinking标识进入解析缓存；默认prepare/chat_action继续false。8项Native验收（含2项注入流程）及14K Native ASan/UBSan回归通过。真实自预测宿主与跨机测量入口见[D-SPEC](design/self-speculation-v1.md)，默认agentctl未接入，性能仍待测。

E55当前CLI工具环境补充：--test-toolchain BIN_DIR与--test-sdk SDK_DIR是成对的部署绑定，不增加文件／网络权限；未修改聊天、token或KV语义。Native14K／Stream／输出SAN回归通过，推理性能未重测；边界与测试见[D-EXEC](design/test-sandbox-v1.md)。

> E53当前test.run在配置／模型初始化前验证cxf宿主启动环境保护，文件／网络边界仍见[D-EXEC](design/test-sandbox-v1.md)。14K、Stream、输出SAN回归通过；新开销独立测量，E52为修复前阶段，不代表所有同UID进程隔离。

> E52（2026-11-01）：test.run默认macOS文件／网络隔离，详见[D-EXEC](design/test-sandbox-v1.md)。准备／执行／清理仍由父进程驱动，不改变Native KV所有权。14K、Stream、工具输出SAN回归通过；超时回执逐次用实际token／fresh-greedy核对，确定性Stream夹具显式选CLT，生产命令未替换。开销数据独立于历史推理收益。

> E50（2026-11-01）新增CLI单行编辑、默认最小测试环境与可选HTTP本地文本辅助，见[工具契约](basic-agent.md)和[D-HYBRID](design/hybrid-v1.md)。Native14K／Stream／工具输出兼容回归已重跑；本地辅助没有接共享Native KV，也未证明端云任务质量／提速。

> 本文件维护当前 D-CHAT。E21已实现借用engine的串行工具窗，E29已在同Native会话接真实test.run输出；通用交接／多消费者仍待实现，见 [D-PREP](design/prewarm-v1.md)。独立Stream仍独占worker，不可直接叠接。全部契约见 [设计入口](design/README.md)。

> 测试与能力探测统一见 [成果汇总](benchmarks/README.zh-CN.md#正确性能力与未验证项)（E04），实现正确性与真实模型自主任务结果分开统计。

验收口径更新：0.6B 仅作为 llama.cpp 兼容与优化载体，模型任务能力不作门槛。
后续对照使用确定性 messages／工具结果，检查 token、KV、采样一致性和开销；
不继续通过调整提示词追求该模型的代码修复成功。

当前新增真实聊天路径对照：同一模型／引擎、相同模板输入，一边全文分词＋token 前缀
比较＋已有 KV 缓存，一边实际原生提供者的检查点后缀更新。初始公共前缀不计入续轮
结果；预热后交替顺序、多样本，分别报告分词量、KV 计算量、前端和整体耗时。
这不是 llama-server 的完整 HTTP 基准，不能把已有 KV 缓存收益算成新增优化。

后续优化契约（2026-10-30，实施前记录，现已实现验收）：缓存已验证 Qwen3 模板的解析准备，仍调用
上游 Jinja 渲染，不手写 ChatML。仅对已验收模板 SHA-256 启用快路径；其他模板继续
原路径。首次／工具 schema 改变时从完整上游路径构造解析器，随后只保存解析器和工具
schema 哈希，不保存 messages／历史文本。system／任务切换不改变该固定模板的输出语法，
但仍执行原来的 token 前缀校验。原始 JSON 参数须经过与上游一致的对象化处理。
提供诊断开关作同进程三路对照：全文分词已有缓存、旧检查点路径、解析准备复用路径。
所有 token／动作与不带此缓存的上游路径对照后才能宣称兼容，缓存命中本身不算验收。
实现／性能与限制以[同进程复测记录](llama-chat-compat-benchmark.md#后续优化复用模板解析准备)为准；
未知模板和独立 tool_use 模板继续原路径。
后继内存更新：[上下文单份审计](context-memory-audit.md) 已验证 14K 级兼容和显式分支共享；
已缩短请求／DOM／messages 的重叠存活期，但仍未消除所有临时表示。
原生准备现已进一步通过借用 visitor 跳过完整请求 JSON 往返；冷／热路径均保留
上游消息语义。[借用消息阶段记录](context-memory-audit.md#实现与兼容) 和
下文早期完整请求转换的说明保留为阶段历史。
最新缓存命中路径直接构造 Jinja 值并只读收集结果，分词先尝试有界缓冲；
保留上游输入来源标记。[本轮实测](context-memory-audit.md#直接-jinja-值的实现与实测)
为当前实现口径；冷准备／未知模板仍经完整上游入口，完整模板渲染尚未消除。

2026-10-30，先立契约后实施，Qwen3 首版已完成并通过验收。依赖 [上下文后端 v1](llama-context-v1.md)。

本轮把同版本 llama.cpp 的 common/Jinja 模板与输出解析器接到可选原生 agentctl。
cxf 核心仍是 C；C++ 桥只依赖固定 b10938 的 libllama-common，不另造工具协议。
首轮验收 Qwen3 ChatML 模板、关闭 thinking、greedy 有界生成；其他模板不能自动视作兼容。

## 所有权与稳定边界

应用继续持有原始 transcript；桥不持久化另一份 messages、完整文本或 token 历史。
每轮使用既有 transcript→标准 messages 转换，再由上游模板临时渲染完整输入。
这是过渡阶段：完整模板渲染仍有线性开销，不宣称事件级增量模板已完成。

桥只持有 session、前一轮 generation header 起点的字节／token 检查点、该前缀的
SHA-256 和解析参数。检查点选择独立特殊 token `<|im_start|>` 的起点，避免 BPE
跨普通文本边界。新一轮先验证已知前缀，再只分词检查点之后的模板后缀，明确 replace。
前缀验证失败（问题、system、tools、摘要或模板改动）显式从零重建并计数；不猜 LCP。
这里保留 O(n) 的模板／前缀哈希校验，换取安全接入；省掉的是重复全文分词和引擎前缀匹配。
临时完整渲染不等于持久的第二套 canonical 历史，也不能声称完全消除瞬时内存副本。

生成内容提交到同一 token session。工具调用回收后 transcript 会按标准结构重放，
因此下一轮允许从 generation header 起点替换模型输出；绝不假定原始生成字节与
重渲染 tool call 逐字节一致。只复用检查点之前的 KV。

## 失败与装配

有界输入、上下文和生成预算；超限不执行部分工具动作。不完整输出／没有结束标记不交给
工具执行器。上游模板和解析异常转换为 cxf 错误，禁止 C++ 异常穿过 C ABI。
解析成功后复用既有 action 编码、工具白名单、权限和执行／结果回流，不增加平行执行器。

agentctl 原生路径显式选择，默认 HTTP 不改；原生不需要 URL 或 key。
模型加载一次，任务边界重置 session，REPL 的 model/reload 不得假装已换本地模型。
不上传本地配置、凭据、模型或测试日志；新仓库暂不推送，上传前做独立检查。

实施修正：既有 WorkItem 把 FULL 当作“扩大输出缓冲并重新执行”。原生 CLI 内部
raw/action 缓冲固定，原生 FULL 表示上下文／生成／解析上限，不会因外层扩容解除；
因此在 CapImpl 边界返回 RANGE，保留原始 last_rc=FULL 诊断。真实生成预算耗尽验收
要求只执行一次推理，不允许悄悄放大成五次。外层实际 action 缓冲不足仍沿用原有 FULL。

## 验收计划

- 后缀分词得到的完整 canonical tokens 与独立全文分词完全一致，覆盖多轮工具、
  特殊字符、模板尾部、system/tools 修改及任务重置。
- 使用上游输出解析器验收原生 tool_call 到既有 action 的转换；畸形调用拒绝。
- 真实 cxf 工具执行和结果回流使用可重现输出做集成测试；真实模型另测，区分基础设施
  正确与 0.6B 模型任务能力，不把脚本输出宣传成真实自主推理。
- 真实模型启动和生成、默认 HTTP 回归、ASan／UBSan；记录依赖及剩余边界。

## 装配与复现

实现为 `tests/agent_native.h/.cpp`，由 `tests/agentctl.c` 的可选构建使用。
`src/llama_internal.h` 仅暴露本桥所需的私有模型借用接口，不成为公开 cxf ABI。
未更改默认 C 源文件清单；普通 `build/agentctl` 仍不需要 libllama 或 C++。

需要 b10938 对应的源码头文件、libllama、libllama-common、ggml 动态库及兼容的
C++ 工具链 ABI；SHA-256 复用同一源码树中的 public-domain 实现。
不自动下载依赖或模型，不将上游完整源码复制进 cxf。

```sh
python3 tools/build_agent_native.py --test \
  --sdk /path/to/llama.cpp-b10938 --runtime /path/to/runtime \
  --model /path/to/Qwen3.gguf --out build/native-chat-local

build/native-chat-local/agentctl-native --repl \
  --llama-model /path/to/Qwen3.gguf --llama-backend /path/to/runtime \
  --root /path/to/workspace
```

需要写文件时显式增加 `--write /path/to/workspace`；运行测试显式增加
`--test-command '你的固定测试命令'`。原生模式不用 URL/key，也不读取 agentctl.json；
与显式 `--config` 合用会拒绝，避免悄悄忽略用户配置。
`CXF_LLM_SYSTEM` 只提供 system 正文；未提供时沿用现有代码助手说明。

可选参数：`--llama-context`（4096）、`--llama-max-tokens`（512）、
`--llama-gpu-layers`（99；0 表示 CPU）、`--llama-threads`（4）。
模型／部署参数在启动时固定；REPL 普通输入各自为独立任务，不能把上一个任务的 KV
当作自动会话记忆。原生参数缺少 `--llama-model` 会拒绝，不会意外转去 HTTP。

## 验收结果与剩余开销

M5 Metal、Qwen3-0.6B-Q8_0、固定 b10938：

- `build/native-chat-final2-20261030/`：模板与 canonical tokens 对照、多轮工具执行、
  system/tools 改动、Unicode／特殊标记、批量动作和畸形批次拒绝、超限不改历史均通过。
- `build/native-chat-sanitized2-20261030/`：同一套 ASan／UBSan 通过，无编译或 sanitizer
  诊断。预编译 libllama-common／libllama／Metal 本身不在插桩范围。
- 六轮脚本选择工具，实际执行 C 文件读取、退出码 1 的编译测试、覆盖修正、退出码 0
  的重测、回读和 DONE。每轮真实预填充并与独立全文分词对照；**选择动作来自脚本**。
- 真实模型独立生成 `OK`，不是脚本生成。真实 CLI 连续两任务、配置隔离、固定模型
  控制、生成上限仅一次执行和非法参数拒绝通过。
- 真实单文件任务通过：模型实际调用 fs.read，读取 `ANSWER=COBALT_7319`，第二轮按
  收据回答；共两次模型调用、无失败。另一次指定步骤的 C 修复探测未通过：首轮动作
  格式校验返回 FORMAT，未派出工具，文件仍为 `return 1`。这不构成真实代码修复能力通过。
- 默认 `make test` 全部通过。

验收摘要及源码哈希保存在 [evidence/llama-chat-v1-20261030.json](evidence/llama-chat-v1-20261030.json)，
不含凭据或私人端点。完整编译命令、原始引擎输出在相应 build 目录，清理构建时会删除。
CLI 复现入口为 `tools/test_native_cli.py`：传 `--binary`、`--model`、`--runtime`、`--out`；
可增加 `--probe-tools`，单独记录真实模型任务能力，不把该能力探测混入基础设施通过判据。

正确性套件计数为分词 2077 token、累计复用前缀 2462 token、前缀变更重建 3 次。
计数包含不同场景，**不是速度提升比例**；本轮没有新的耗时对比结论。

目前仍逐轮转换完整 transcript、渲染模板并校验前缀哈希；首次输入／前缀变化时还会
临时完整分词。每轮生成 header 和前一轮重放区域可能重算。由此不能声称零临时副本、
完全消除线性前端工作或所有生成 KV 都能保留。后续要从事件边界推进增量模板，
而不能把这个过渡桥当成永久的第二套历史存储。

仅验收 Qwen3 ChatML 且关闭 thinking；缺少所需特殊 token／工具模板／预填充形状或
额外 stop 字符串时拒绝。生成目前为无 grammar 约束的 greedy；合法格式、权限、
工具存在性仍由解析器和原有执行器检查。模型名／架构满足入口检查并不等于任务能力已验收。
本轮没有原生 Windows、模型卸载迁移、真正云端流或后台并行调度。

## 工具窗口的借用模式（E21，2026-10-31）

`agent_native_bind(engine, context_tokens, max_generate, &native)`在已就绪engine上创建自己拥有的session，借用engine；caller必须传匹配的context容量，并串行执行所有Native／engine操作。`agent_native_close`在借用模式只关闭自己的session；原`agent_native_open`仍拥有并关闭engine。`agent_native_checkpoint`截到已验收的generation-header前规范token边界，返回当前session/revision/长度；未prepare返回EMPTY，不隐式计算新源。

工具准备宿主在worker owner中bind／prepare／checkpoint／关闭；工具进程与另一线程排队的准备任务并行，不跨线程直接调用libllama。该串行窗口没有正在服务的生成请求，checkpoint在同一独占Native session上就地操作，不是完整D-PREP多消费者候选fork／ADOPT实现。真实收据返回后仍调用普通Native prepare，完整渲染和前缀hash验证保留；system/tools或前缀改变会重建。只有明确选择recompute／restore策略才逐出KV；普通resident不额外造冷态。规则与测量口径统一维护在[D-PREP E21](design/prewarm-v1.md#13-e21-工具执行窗口切片实施前冻结2026-10-31)。

原生CLI新增`--tool-prepare late|immediate|timed`和`--tool-kv resident|recompute|restore`，要求已有`--llama-model`及部署的`--test-command`，仅包装已授权test.run；原有独立Native和HTTP路径兼容。E21时间测试用同宿主API和确定性工具步骤，不是0.6B自主修复能力分数。

E23的原生CLI还支持`--tool-prepare progress`：test.run父进程创建成功后以当前execute epoch更新剩余时间，未知／无进度按事件／工具返回继续，不改变prompt／tool parser或engine owner。macOS26+工具默认posix_spawn；旧fork保留为部署测试入口，模型无法指定backend。全Native／14K／KV／Stream与实际CLI确定性路由验收通过。progress没有稳定同时改善等待与提前空等，默认仍timed；详见[D-PREP §17](design/prewarm-v1.md#17-e22e23-当前结论与选择2026-10-31)。

E24追加显式`--tool-snapshot-release early|return`（默认return），只影响工具窗口内已恢复快照的释放时机。early在同准备线程finish排空后删除，返回仍join；不会改变token归属、最后完整render／guard、取消回滚或工具调用次数。计时另列父回调／线程创建／函数入口，旧E23时钟口径保留，详见[D-PREP §18](design/prewarm-v1.md#18-e24-完整任务归因与提前释放实施前冻结2026-10-31)。

## E29 Native工具正文的有界推测尾部（冻结，2026-10-31）

仅显式agentctl工具输出通道使用。保留旧历史checkpoint/hash，在同canonical session上追加已验收Qwen3模板的局部test.run调用／tool正文；正文token化可随新字节变化，故只对有界当前工具tail做LCP/replace，不能把正文byte末尾发布成原完整消息检查点。最终完整render／guard后复核tail token，采纳相同已算KV，状态收据和生成header仍等真实工具结束。partial期间prepared=false，不允许动作发布；取消回滚新tail，旧历史常驻。默认Native准备路径和HTTP序列化不改变。详细所有权、共同tail-status布局和验收在[D-PREP §25](design/prewarm-v1.md#25-e29-agentctl真实工具输出接线实施前冻结2026-10-31)。

E29此通道现已在实际agentctl/test.run接线并验收；off/on最终收据、canonical token与首greedy一致，持续输出三轮任务逐对节省95／184ms，突发反慢43／21ms，默认不开启。显式通道共同使用最终状态尾收据和入站补LF；默认路径未改。真实0.6B生产探测rc7、零工具执行的失败单列；脚本只控制模型动作选择，工具、模板、KV与CLI执行均真实。详见D-PREP§25当前结果及统一E29。

E31/E32增加可选输出策略setter及REPL`/tool-output-policy old|on|thread|token|progress`，需已启用输出通道且空闲；`/tool-output-prefill off|on`仍控制是否启用。on为E30计时策略，progress依据后续输出增长；thread/token只作隔离诊断，不提交推测KV。最终正常prepare保持完整校验。规范与实测以D-PREP§27为准。

### E33–E35：输出进程回收诊断与可选修正

本轮不改变聊天模板、canonical所有权或最终校验。输出模式在空闲Native REPL中可用`/tool-output-policy progress-race`并`/tool-output-prefill on`选择增长预填充＋退出竞态修正；`off-race`配合`/tool-output-prefill off`为仅改执行器的控制组。`on`、`progress`、`on-event`、`progress-event`及诊断`thread/token`继续保留。启动仍须显式输出profile，模型常驻；默认行为不变。

macOS EOF后的事件／退出竞态重试属于test.run执行器协调，不是llama.cpp接口改动，不主动创建快照或逐出KV。契约与验收以[D-PREP §28](design/prewarm-v1.md)为唯一规范；实测E33–E35含负对，见[成果报告](benchmarks/README.zh-CN.md)。Windows/CUDA尚未验收。

### E36/E37诊断与按需消费者

空闲Native输出profile可显式选择`idle-race`、`thread-race`、`token-race`作同负载诊断；`lazy-race`沿用growth门槛，首次合格输出才创建消费者。实验已证明无合格增长时不建线程，但没有证明完整任务比`progress-race`更快，默认不切换。最终token/greedy校验和单owner仍不变，契约见[D-PREP §29](design/prewarm-v1.md)；性能、CPU观测口径与全部负对见[成果报告](benchmarks/README.zh-CN.md)。

### E38/E39私有时序与内部同步策略

空闲Native输出profile中，`off-drain`＋prefill off与`progress-drain`＋prefill on显式选择同次prefill内部合并同步；旧off-race/progress-race保留逐块屏障作同负载控制。默认strict，所有公开prepare／prefill返回仍完成KV，不跨工具携带pending工作，不改变最终token／greedy或canonical所有权。实际增益较小且有负对，大幅sync计时下降主要移入decode。契约与模型验收见[D-PREP §30](design/prewarm-v1.md)和[D-CTX后继说明](llama-context-v1.md)。

## E42–E44 CLI诊断与可选尾批合并（2026-10-31）

原生CLI新增`--llama-batch N`，默认512，映射已有`cxf_llama_params.batch_tokens`；只在打开引擎时分配容量，不在每轮重新分配。空闲诊断命令`/tool-output-policy`增加`off-split`／`progress-split`和`off-coalesce`／`progress-coalesce`，分别设cap512且关闭／打开有限尾批合并，均strict同步；旧`off-drain`／`progress-drain`才开启内部defer。切换回旧策略显式重置chunk设置，不能因数字ID增大意外继承defer。

`-bare`／`-notify`后缀仅供链接Metal probe的测试harness归一化使用，生产不实现这些后缀。bare关闭采集，notify注册最小时间marker；默认生产路径不链接ObjC probe。所有路径仍同一canonical session、同收据、同单owner，最终prepare保留完整render／prefix复核与独立验收；未扩展自主规划、Windows/CUDA或云SSE。当前结果见[D-PREP §31](design/prewarm-v1.md)及[成果报告](benchmarks/README.zh-CN.md)。

## E45/E46 显式640容量（2026-10-31）

CLI配置沿用已有参数，无新调度命令或生产环境变量：启动时`--llama-batch 640 --tool-output-prefill off --repl`，空闲后`/tool-output-policy off-coalesce`启用有限尾批合并；需增长预填充时选择`/tool-output-policy progress-coalesce`并`/tool-output-prefill on`。`*-coalesce`实验固定普通cap512，因此这些模式要求分配容量至少512；默认容量与策略不自动切换。

容量只在引擎打开时分配，E46为比较各容量而重开模型；模型加载不计任务计时，但实际应用重开有成本。较大初始上下文继续分块，约600-token工具增量可以合并。Qwen3/Metal以外的设备、模型及更大工具增量未因这组结果自动验收；超容量继续分块，不创建第二份canonical或KV。结果以[D-PREP §32](design/prewarm-v1.md)及[统一报告](benchmarks/README.zh-CN.md)为准。

## E47–E49 同容量输出控制与失败量子试验（2026-10-31）

空闲Native诊断增加`thread-coalesce`（仅owner空操作）和`token-coalesce`（仅分词），两者与`off-coalesce`／`progress-coalesce`保持同cap512／尾批合并设置，避免控制组改变最终分块。`progress-coarse`只将非final输出发布量子512改为1536字节，观察窗／预算／最终复核保持。它在新任务中相对旧growth退化，不作为推荐默认；默认容量512、输出预填充off及旧growth门槛保持。

较大量子并未减少所测marker批次数（42→42），却增加每批token并推迟首次owner开始，使工具后join变长。全部负对、CPU供给异常和设备内存敏感性分开保留，见[D-PREP §33](design/prewarm-v1.md)与[完整报告](benchmarks/README.zh-CN.md)；不宣称旧217ms已消除或都属于kernel／内存争用。
