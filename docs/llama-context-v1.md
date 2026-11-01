# 原生 llama.cpp 上下文后端 v1

> 最新数据统一索引见 [测试成果汇总](benchmarks/README.zh-CN.md)、[完整指标](benchmarks/data/metrics.csv) 与 [测试方法](benchmarks/METHODOLOGY.md)。本文件保留设计契约及实现历史；正式数据展示以统一归档为准。

状态：v1 后端已实现并通过真实模型验收。2026-10-30。
后继更新：[Qwen3 聊天／工具提供者](llama-chat-v1.md) 已接入可选原生 CLI；
可选 KV 工作线程与提前恢复实测保留于本文阶段记录；尚未自动接管该 CLI。
后续预热设计已迁入 [D-PREP](design/prewarm-v1.md)，状态为待实现。
当前 KV/Stream 宿主契约见 [设计入口](design/README.md)，不能把本文早期目标当成最新实现范围。
本文前半维护核心 D-CTX 边界；后半为宿主接入与实验的阶段记录。以下原交接计划不再表示该首版接入尚未开始。
依据：[实测报告](llama-context-experiment.md)。本文件维护核心上下文契约及原始阶段记录；其他主题以设计入口登记的唯一契约为准。
`STATUS.md` 只保留索引，不平行定义接口。

## 目标与阶段

v1 把已验证的机制变成 cxf 可调用的可选 C 后端：一个引擎实例持有模型和统一 KV 池，
session 使用有世代的句柄；调用者给出追加或明确的替换边界，引擎不再扫描全文猜前缀。
支持预算内分块预填充、同模型公共前缀分支、回退与单步 greedy 采样。

本轮交付 `include/cxf/llama.h` / `src/llama.c`、真实模型验收程序与复现入口。
可由 `CapImpl.ctx` 绑定一个 session；验收须覆盖 cxf 的真实 WorkItem 调用。
它是可调用的后端模块，不改变既有 `agentctl` HTTP 路径和默认行为。

后续把这个后端接到完整聊天／工具模板时，必须另行验证标准 tool messages 的模板渲染、
生成结束标记和 token 边界。**不把旧 transcript blob 的字节追加直接冒充模型 token 追加**，
不以重新全文分词并扫描的方式声称完成显式边界适配。已有 HTTP 协议层继续承担其原有职责。

## 状态归属与存储

| 对象 | 唯一所有者 | 生命周期 |
|---|---|---|
| 原文、工具结果、事件 | 既有应用／事件存储 | 不因 KV 回退删除 |
| 已提交 token 块 | 本后端的 blob pool | session 借用块句柄；分支共享前缀，已有 token 不改写 |
| 块的父边／位置 | 本后端的 typed table | 单向无环；不引入引用计数 |
| session 与 revision | 本后端的 session table | 释放后句柄失效；复用槽位有新世代 |
| KV 与位置／序列映射 | 一个 llama_context | 后端串行驱动；不另存完整聊天记录 |
| decode 临时 batch | 引擎实例 | 有界 scratch，最多一个 batch，不是第二份历史 |

复用 cxf 的 `blobpool`、`table`、`handle`、`alloc`，不修改它们的单所有者契约。
session 共享 token 块不共享回收责任；引擎在编辑／关闭 session 后统一标记可达块，
回收不可达块。共享前缀不会在第一次追加时整段 copy-on-write。
单块最多 256 token；部分前缀切分可保留该块中未用的尾部，空间损耗有界。
实施审查补充：逐 token 提交不得每次都分配新块。块预留 256 token，只有当前 session
位于该块的物理尾部时才能扩展；既有 token 不改写，其他 session 的逻辑末端不随之增长。
旧视图若从块中间继续，创建新块。跨块追加先把新块分配完，再扩展旧块和发布新视图，
确保中途 OOM 不留下半次追加。这仍由引擎单一回收，不引入引用计数或整段 COW。
池的预留容量、存活 token payload 与逻辑 token 数分别统计，不混报内存节省。

句柄包含引擎身份和 session 世代。不同引擎的句柄不能互用。
所有编辑及 prefill／采样都带 expected revision：过期视图拒绝，不静默写到新版本。
一个引擎不允许多线程并发调用；宿主必须在一次 llama 调用完成后再修改同一实例。

## 两个边界与明确变更

- `committed`：调用者已经确认不会继续改写的 token 末端。
- `evaluated`：引擎已确实计算好 KV 的 token 末端，恒有 `evaluated <= committed`。
- append 只提交新 token；replace(keep, suffix) 明确声明前 keep 个 token 不变。
- 若 replace 触及已计算区域，删除相应 KV 后缀，`evaluated` 回到 keep。
- 内存分配、token ID、容量和 revision 校验在触碰 KV 之前完成。
- v1 只接收 token；分词函数是无持久副本的辅助接口。文本块尚未稳定或 BPE 尾部可变时，
  上游不得把它当 committed；聊天模板改变也必须声明正确的替换边界。
- 不接受 encoder-decoder、recurrent、hybrid、diffusion 模型；这些架构的状态语义另立验收。

## KV 分支与预算

fork(parent, keep) 共享前 keep 个 token 块，复制同一 KV 池里的序列归属，
只复用 parent 实际算到的部分；父任务释放后子任务仍可继续。
不同模型、不同引擎实例之间不共享 KV。共享操作不能被宣传成跨 context 的零拷贝。

prefill 每次显式给 token budget；零预算不工作。
非 flush 调用不足最小批次时保留 pending；flush 用于输入结束，仍受 token budget 限制。
这里只提供确定性的合并与预算，不添加后台线程、云端网络、预测器或隐式定时器。
宿主可以在云端数据到达时提交稳定 token，在空闲预算内 pump，再在末尾 flush。

任意 decode 非零返回都不继续相信可能部分写入的 KV：清除此 session 的 KV，
保留 canonical token 块，把 evaluated 置零并记 native error、失败及失效 token 计数。
调用者可修正预算／释放其他 session 后明确重试，不偷偷切回 HTTP 或 CPU。

llama 的 logits 是 context 级临时输出。切换 session 后采样，必须重新取得该 session
末端的 logits；必要时只重算最后一个 token，记录 replay 计数，不能读取其他分支的 logits。
采样与提交分离：greedy 返回 token，不擅自把 EOS 或生成内容写入应用事件。

## 兼容与未实现

- 可选构建；默认 cxf 数据结构库和 `make test` 不依赖 libllama，不自动下载模型或引擎。
- 首版固定并核验 llama.cpp b10938 / f1e44dcc11d8802d107bd7331a3d3fd3e6f57b93。
  更换头文件或动态库必须重新跑 ABI／真实模型验收，不混用版本。
- Windows 原生构建／平台层尚未验收；不能因 llama.cpp 支持 Windows 就声明整个 cxf 已支持。
- 不改变工具权限、HTTP 协议、调度策略、工作流学习、摘要或事件留存。
- **预分配 KV buffer 不会动态缩小或归还设备**。session close／replace 释放池内逻辑占用。
- v1 原始交付不含跨进程恢复格式、KV 磁盘缓存、动态显存分页、通用聊天模板提供者或云端流接入；
  后继同引擎私有 KV 文件／宿主工作线程见本文末尾，Qwen3 聊天／工具接入见上面的后继文档。

## 验收清单

1. 与同一版本 libllama 的从头计算比对 greedy token；追加、回退重算结果一致。
2. fork 不增加已有 token payload；分支追加只增加新块，父释放后子仍可采样。
3. 关闭／复用句柄、跨引擎句柄、过期 revision、非法 token／容量拒绝均不改原状态。
4. 预算和 flush 可观测；预填充只计算新后缀；采样不会误用其他 session 的 logits。
5. 分配失败回滚、无主块回收、重复 fork／关闭后 payload 归零。
6. 实际挂入 cxf `CapImpl` 并由 WorkItem 执行，不能仅证明一个独立 llama 示例能跑。
7. 默认完整回归通过；真实模型验收显式运行，与默认无模型测试分开。
8. 实施后回填本文件完成项、命令、限制和下一阶段入口；旧实验结论仍保留。

## 实现与验收记录（2026-10-30）

上述 v1 范围已落在 `include/cxf/llama.h` 和 `src/llama.c`。验收程序为
`tests/test_llama_context.c`，复现入口为 `tools/test_llama_context.py` / `make test-llama`。
默认构建的源文件清单没有加入此可选模块；宿主链接时显式加入 `src/llama.c`、
匹配的 llama／ggml 头文件和动态库。公开头文件不依赖 llama 头文件。

本机命令（路径默认使用已安装的 b10938 与 Qwen3-0.6B-Q8_0；异机显式传入三项）：

```sh
python3 tools/test_llama_context.py --out build/llama-native-v1-final-20261030
python3 tools/test_llama_context.py --sanitize --out build/llama-native-v1-sanitized-20261030
# 自定义安装：
python3 tools/test_llama_context.py --sdk /path/to/llama.cpp-b10938 \
  --runtime /path/to/runtime --model /path/to/model.gguf
make test
```

验收使用 M5 Metal 与真实 Qwen3 0.6B，覆盖：

- 追加／回退后的采样与从头计算一致，并另用原始 libllama 独立计算作基准。
- 共享前缀不增加 token payload；父关闭后子可继续；逐 token 追加 200 次仍复用一个块，
  旧分支不看见新尾部，旧分支继续追加也不破坏另一分支。
- revision、句柄世代、跨引擎身份、范围和非法 token 拒绝；分配失败不发布半次编辑。
- 最小批次、预算、flush、只计算新增后缀及跨 session 的 logits 恢复。
- 真实 KV 池耗尽时保留 token 历史、清除受影响 session 的 KV；释放其他 session 后显式重试成功。
- 重复分支／关闭后块与 payload 归零；引擎关闭后自定义 cxf 分配器的存活字节归零。
- 原生 `CapImpl` 经真实 cxf WorkItem 执行，最终结果与模型基准一致。

普通与 AddressSanitizer／UndefinedBehaviorSanitizer 验收均通过，无编译或 sanitizer 诊断。
默认 `make test` 全部通过。可保留入库的[验收摘要及源码哈希](evidence/llama-context-v1-20261030.json)
不依赖 build 目录；完整原始输出在上述两个 build 目录内，清理构建时会删除。
预编译 libllama／Metal 本身不在本项目 sanitizer 插桩范围内。
测试输出为实际单步推理接入证明，不等于该 0.6B 模型
已经通过自主多轮工具任务，也不把这轮正确性测试当作新的性能收益测量。

## 宿主调用顺序与下一阶段

1. 一次打开 engine，创建 session；宿主只保留句柄、revision 和应用事件。
2. 已知稳定 token 到达后调用 append；编辑调用 replace(keep, suffix)，明确给 token 边界。
3. 用 session_view 取得当前 revision；在可用预算内 prefill，输入结束时 flush。
4. evaluated 等于 committed 后 greedy；检查 EOG，再把选中 token 显式 append。
   生成过程每步 prefill 可用 `flush=true, budget=1`，不必等待输入预填充的最小批次。
5. 子任务调用 fork 继承前缀；所有分支结束后关闭 session，最后关闭 engine。

原阶段交接（Qwen3 首轮已按后继文档完成）：补一个完整的聊天／工具模板提供者，先定义事件到 token 的稳定边界、
工具结果插入及模板尾部回退，再接 `agentctl` 的原生路径并做多轮工具实测。
现有 HTTP 路径仍保留；不要求在其内部维护第二套原生 token 历史。
真实云端流式接入、预分配 KV 内存动态归还和 Windows 平台验收分别立项，不能从本轮状态推导完成。

## 后续契约：上下文装配期间提前准备 KV

> 阶段设计记录；当前宿主 API、ready 与取消边界以 [D-KV](design/kv-worker-v1.md) 为准。

2026-10-30，实施前记录；当时尚未实现异步队列、磁盘卸载或自动接线。
状态更新：宿主工作线程与同引擎 KV 文件恢复已实现；实际交付与实测以本文末尾为准。
用户目标是让运行时在最终请求尚未装配完成时，就通知引擎将要使用哪个已有前缀，
使 KV 换入／预填充与事件汇聚、工具执行、上下文装配重叠，减少请求提交后的等待及尾延迟。
不能只在装配结束后入队，再把这个改名为异步。

### 时序与收益口径

原路径：确认下一步 → 装配上下文 → 提交请求 → 换入 KV → 计算新后缀 → 生成。
目标路径：确认可复用前缀 → 立即发准备提示；宿主继续装配，同时引擎准备该前缀；
最终提交时校验提示对应的版本与 token 边界，再等待未完成部分并计算新后缀。
提示仅说明未来需求，不授权删除事件或执行模型输出。

设装配耗时 A，前缀准备耗时 K，后缀计算与生成耗时 R。只有这两个阶段、没有资源争用时，
串行时间为 A+K+R，完全重叠的理想时间为 max(A,K)+R，最多隐藏 min(A,K)。
若提示可以更早在等待工具／子任务时发出，可利用的窗口 W 更长，请求提交后残余准备约为
max(0,K-W)。这些是时序模型，不是实测加速承诺；单独重叠两个阶段不会自动带来十倍总提速。

当前暖态单段长文本的渲染约 0.425 ms，不等于整个装配耗时；它本身不足以隐藏数百毫秒搬运。
首次实验必须测实际可用的提前量，优先考虑进入工具等待／已确定恢复父会话的时刻，
不能只在最后的模板渲染前触发。

### 身份、所有权与失效

- 提示携带 engine／model 实例身份、session 世代、预期 revision、稳定 token 前缀末端、
  snapshot 身份及可取消的任务编号；只携带元数据，不复制整份 transcript／canonical token 历史。
- 提示可在前缀身份已经确定而新后缀尚未确定时发出。首版采用保守的版本校验；
  版本改变、任务取消或槽位复用后，过期完成事件不得覆盖当前状态。
- 工具结果／子 agent 返回可触发父会话前缀准备，但不能把子 agent 的 KV 任意拼到父会话。
  KV 对应完整的先行 token、位置及模型配置；只有已验证相同的前缀才可复用。
  新返回文本仍须按父会话的模板与前文分词、计算。
- 新后缀预填充需要已提交且不可改写的 token 边界；未闭合 JSON、普通文本 BPE 尾部和
  可能重排的消息不得当成稳定边界。提前搬入已有前缀与提前计算未定文本是两个独立能力。
- 已准备的数据可以因最终请求不匹配而被丢弃，必须计量浪费的 I/O／计算；
  不能为了保留一次预测而阻塞取消或持久保存另一份消息历史。

### 并发与接口边界

固定 b10938 的 llama_state_seq_get_data_ext／set_data_ext／save_file／load_file 会同步 context。
现有 cxf_llama 仍为单线程模块，由调用者串行驱动；本契约不授权并发修改同一个实例。
可选宿主适配层应将所有引擎调用交给同一工作线程，宿主通过有界队列提交准备／取消／最终提交，
在引擎工作期间只处理自己的事件与上下文装配。若工作线程忙于其他推理，准备也可能排队。
单纯在宿主装配完后才 pump 队列不满足重叠目标。

只有文件 I/O 可放到独立 I/O 线程时，仍需给 staging 内存设预算，完成后由引擎所有者串行安装；
预读进 RAM 不等于 KV 已在设备上可用。区分读盘完成、设备恢复完成和可采样三个状态。
首版按已有序列状态接口验证；任意区间迁移、共享前缀在磁盘上的去重和并行设备传输另行验收。
保存状态不会自动驱逐 KV，移除序列只是让池内位置可复用，不能宣称预分配设备 buffer 已归还。

### 首次实验的接受条件

1. 固定同一模型、token／最终输出、KV 池和 staging 预算，对比“提交后准备”与“提前提示”，
   两边都有同等的缓存和分支复用能力；不得以没有缓存的对照夸大收益。
2. 覆盖已有驻留前缀、确实发生状态恢复、工具等待、子任务返回、提示过晚、提示错误及取消。
   脚本时序实验与真实 agent 装配分别报告，不把人为 sleep 当成真实装配收益。
3. 记录提示、I/O 起止、设备恢复起止、装配结束、最终提交、首 token 的时间戳，
   报告提交后等待、首 token 延迟、端到端耗时及 p50／p95／p99，样本不足时不报告可靠分位数。
4. 记录实际重算 token、搬运字节、无用预取、staging／KV 峰值、取消后资源回收；
   OS 页缓存命中与实际读盘分开说明，不把 warm page cache 当成 SSD 持续吞吐。
5. 在版本变化／槽位复用／失败恢复之后，与无提前提示的独立全文推理对照一致；
   默认工具行为、原生 CLI 和 sanitizer 回归继续通过后，才把该入口接入实际执行器。

### 首版落码范围（实施前补充）

新增可选 `agent_kv_worker` 宿主适配层，与已有 agent 原生桥同目录；不改变默认 C 构建。
引擎由一个工作线程独占，宿主使用同步的 owner 回调初始化／编辑会话，以及非阻塞的准备 ticket。
有界队列／ticket／快照数量和磁盘字节预算，最终提交回调只在 ticket、快照身份及会话版本一致时执行。
取消不强行打断 libllama 内部调用；在调用返回后阻止发布，并回收本次恢复的逻辑 KV。
关闭等待工作线程退出，删除其临时快照；回调借用指针仅在同步调用期间有效。

快照首版只服务当前引擎中仍存活且 revision 不变的完整已评估会话；通过上游序列文件接口
存 KV，不额外写 canonical token 列表。保存与驱逐分开；恢复后 logits 按既有规则最多重算末 token。
磁盘文件为适配层私有临时目录内的不可变产物，不提供外部文件导入／跨进程恢复入口。
保存时记录文件身份与大小，恢复前校验；格式／截断错误拒绝，失败时清除受影响 KV 并保留 token 历史。
这不是任意磁盘内容的安全解析器，逐块校验与故障注入需按实际存储格式继续补验收。

测量先使用真实 Jinja 上下文装配，并另列 25／100／250 ms 的受控等待窗口作为流水线边界实验。
等待窗口不代表真实工具性能。两档都从确实驱逐的相同 KV 开始，交替执行，多样本验证
最终 token 一致；保存成本单列，不混入本轮恢复比较。重复读盘属于 OS 缓存暖态；不清理全机页缓存。
首版不自动替换 agentctl；先以可调用宿主模块和实际 owner 回调／上游装配完成接线验收，
当前单会话 CLI 没有自动卸载需求，不能把该实验说成所有现有任务已加速。

参考：上游固定版本的 [状态接口](https://github.com/ggml-org/llama.cpp/blob/f1e44dcc11d8802d107bd7331a3d3fd3e6f57b93/include/llama.h)
与 src/llama-context.cpp；[KVFlow](https://arxiv.org/abs/2507.07400) 说明工作流信息引导淘汰与重叠预取
已有可行性证据，其 CPU/GPU 工作负载的结果不能直接当作本项目 SSD／llama.cpp 的收益。

## KV 工作线程的实现与验收

实现：`tests/agent_kv_worker.h/.cpp` 为可选宿主模块，公开 C 调用边界；
`src/llama_internal.h`／`src/llama.c` 增加私有的序列保存、逻辑驱逐和恢复入口。
不修改 llama.cpp、不改变默认 agentctl；没有自动淘汰政策或新的持久消息树。
本节为上面契约的实际交付记录，未在本节列为完成的能力仍属后续工作。

引擎在 owner 线程打开／关闭。`run` 是等待完成的 owner 回调，借用参数只活到返回；
`prepare(snapshot)` 是不等待恢复完成的元数据提示。宿主可立即继续事件汇聚、消息构造和
模板渲染，`finish(ticket, callback)` 排在恢复之后，重新校验 snapshot／session／revision，
再在 owner 上执行最终提交。`poll` 读取进度、`cancel` 阻止最终提交；已开始的 native 调用
不被强行中断，完成后回收取消请求恢复的逻辑 KV，若另一个有效提示仍需同一版本则保留。
所有宿主 API 结束后才能 close；close 可排空未消费的异步 ticket 并清理私有临时文件。

队列最多 16 个等待项、16 个未消费 ticket、8 个 snapshot，磁盘总字节另设预算。
没有适配层的完整 KV 暂存 vector；上游文件恢复仍有 tensor I/O scratch，其峰值尚未单独追踪。
计时实验固定相同模型／context／磁盘预算，RSS 只取调用前后，不把它当峰值或显存。

快照只支持当前 engine 中 revision 不变的完整已评估序列；不支持跨进程／跨模型导入。
上游序列文件的 token_count 写为零，canonical token 仍由原块池唯一持有。
token、位置与版本检查通过后才恢复 evaluated；设备同步后才发布 ready。
logits 不在序列文件中，独立恢复后直接 greedy 可能按既有规则重算末 token；
本次最终提交追加新后缀后产生自己的 logits，不需要重算整个前缀。

文件放在权限 0700 的私有临时目录，snapshot 发布前保存完毕，恢复前检查设备／inode、
大小、mtime／ctime 以及格式头。正常修改、替换和截断会拒绝；没有加密签名或全文件校验，
不能作为不可信文件解析器，也没有断电持久性承诺。关闭正常清理，异常进程退出可能遗留临时目录。
保存不会自动驱逐；驱逐只回收统一 KV 池的逻辑位置。不同 snapshot 目前会分别序列化共享前缀，
没有磁盘去重或任意区间迁移，不能宣称整个存储层只保留一份 KV。

调用顺序：

```c
/* 初始化／编辑只在同步 owner 回调中操作 engine。 */
agent_kv_worker_run(worker, init_session, &task);
agent_kv_worker_save(worker, task.session, task.revision, &snapshot);
agent_kv_worker_evict(worker, task.session, task.revision);
/* 已知道恢复哪个父会话，不必等待最终文本装配结束。 */
agent_kv_worker_prepare(worker, snapshot, &ticket);
assemble_context(&task); /* host；不触碰 worker 的 engine */
agent_kv_worker_finish(worker, ticket, commit_suffix, &task, &report);
```

示意代码省略错误处理；实际调用必须检查所有 cxf_result，错误／取消时最终回调不执行。
这是可调用模块与真实模板装配的接线，尚未自动接管当前单会话 CLI，也没有执行器级的
工具 ETA 预测／淘汰决策。本机验证 macOS；Windows 的线程、文件身份与构建验收仍未完成。

复现（固定 SDK/runtime，异机显式传路径）：

```sh
python3 tools/build_agent_native.py --test-kv-worker --out build/kv-worker-local
python3 tools/build_agent_native.py --bench-kv-worker --repeats 7 --out build/kv-worker-bench-local
python3 tools/build_agent_native.py --test-kv-worker --test-long-context --sanitize \
  --out build/kv-worker-sanitized-local
```

`make test-kv-worker` 为可选正确性入口，默认 `make test` 仍不加载模型。
性能与 sanitizer 不混跑；正式数据和剩余边界在下文记录。

### 提前恢复的实测数据（2026-10-30）

Apple M5／16 GiB、Qwen3-0.6B-Q8_0、b10938，8192-token 统一 KV 池（896 MiB），
4 个 session 上限、batch 512、4 CPU 线程、1 GiB 私有快照预算。每组预热一对，随后
同进程交替顺序各 7 样本；没有并发运行其他测试模型或性能探针。
所有试次都先真正移除父序列的池内 KV，再恢复相同文件；两档共享同等缓存条件。
宿主调用当前生产快路径的 visitor、Jinja 值构造与渲染函数，owner 最终 fork 父前缀，
分词／计算新增后缀并取得首个 greedy token。完整 token 和 greedy 均与独立全文计算一致。
首 token 时间戳在采样后立即取得，不包含之后的子会话清理。

以下为从宿主阶段开始到首 token 的中位数，真实装配与受控等待分开：

| 场景 | 提示在装配后 | 提前提示 | 解释 |
|---|---:|---:|---|
| 长单文本，装配约 0.26 ms | 80.6 ms | 81.4 ms | 没有整体改善，窗口过短 |
| 已有 128 条工具历史，只新增 1 条结果 | 135.3 ms | 125.5 ms | 观察到约 12.7 ms 准备与装配重叠；收益有波动 |
| 一次新增 128 条结果 | 2757.8 ms | 2858.6 ms | 后缀计算 5383 token 主导，没有整体改善 |
| 额外 25 ms 受控等待窗口 | 129.1 ms | 98.5 ms | 含实际等待；不代表真实工具性能 |
| 额外 100 ms 受控等待窗口 | 213.2 ms | 147.4 ms | 含实际等待；不代表真实工具性能 |
| 额外 250 ms 受控等待窗口 | 380.9 ms | 298.7 ms | 含实际等待；不代表真实工具性能 |

100 ms 窗口组恢复 4257-token 前缀，序列文件 465.66 MiB，只新增计算 49 token。
提交后残余准备中位数 **62.6→0 ms**，提交后首 token **107.6→43.3 ms**；
提前档各样本实际观察到 45.7–58.9 ms 的准备被窗口覆盖。总阶段中位数约降低 31%，
但两档恢复本身的中位数也不同（62.5／51.0 ms），不能把全部差值归因于重叠。
7 对样本在三个受控窗口组均变快，仍不足以估计可靠 p95／p99 或作一般尾延迟保证。

已有密集历史组恢复 6561-token 前缀（717.69 MiB 文件），只计算 49 个新 token。
7 对中 5 对变快，但提前档范围 113.1–286.9 ms，装配后提示档 124.7–173.5 ms；
存在明显异常值，不能据此声称调度抖动已经改善。时间戳确认有重叠，整体性能仍需更多负载／样本。
一次新增大量内容组虽把约 11 ms 恢复完全藏住，数秒的新 prefill 及波动仍淹没该收益。

这里通过文件接口读取的快照已在 OS 页缓存中预热，是**文件缓存暖态＋Metal 统一内存**实验，
没有测冷 SSD／PCIe GPU；没有清理全机页缓存，也没有后台带宽竞争实验。
恢复计时包含上游同步文件读取与设备安装，未分别追踪每次底层 read 与设备复制。
本组 4257-token 快照单次保存耗时在 59.6–157.6 ms 间，不在上述恢复阶段中；
保存写入 OS 缓存，未 fsync，不是持久写入吞吐。完整任务收益必须计入保存／淘汰成本，
不能为了隐藏几十毫秒恢复而无条件每轮保存。显存预分配和持久 canonical token 容量没有因提示减少。

原始性能目录 `build/kv-worker-final2-20261030/`；84 条正式计时均通过 token／greedy 校验。
worker／14K 聊天的 ASan／UBSan 目录 `build/kv-worker-sanitized-20261030/`，
底层恢复／上下文 ASan／UBSan 目录 `build/kv-worker-core-sanitized2-20261030/`，
原生 CLI `build/kv-worker-cli-20261030/`，默认 `make test` 通过。
底层真实 KV 池满时恢复失败保留其他会话，释放容量后重试恢复成功，直接采样仅重算末 token；
外引擎身份和旧 revision 的 image 均被拒绝。新增测试还覆盖磁盘预算、snapshot／队列容量、驻留／实际恢复、
排队中／恢复中／已就绪后的取消、占用时删除拒绝、过期 revision、槽位复用、
截断文件、未知 snapshot、无 token 列表及关闭清理；预编译上游动态库本身没有 sanitizer 插桩。
原始样本、构建源码／头文件哈希及验证摘要见
[evidence/kv-worker-20261030.json](evidence/kv-worker-20261030.json)。

## 后续契约：流式输入与本地预填充／片段处理

> 阶段设计记录；当前协议、背压、收据与生命周期以 [D-STREAM](design/stream-v1.md) 为准。

2026-10-30，实施前记录。本轮把可回放的文本事件、持续预填充、完整文件处理接到同一
流水线；不调用付费云端 API，不声称已接入具体厂商 SSE，也不把模拟输入速率当成实测云端吞吐。
复用 `AgentKvWorker` 的引擎所有者；新增宿主流适配模块，接收线程、模型调度和片段处理
可并行，但同一 llama context 的调用仍串行。

首版输入为有序 NDJSON 事件，每条带递增 `seq`：`text`、`file_chunk`、`file_end`、`end`。
事件内容是已经由上游确认不会改写的独立单元，网络调用可任意切分字节；模块先重组完整行、
严格解析 UTF-8／JSON，再把独立单元封成模型消息。不把任意未闭合正文或 BPE 尾部声明为稳定。
源适配器将来可把云端／工具流转换成这些事件，不能直接转发云端 token ID 或 KV。
同一文件的新版本使旧测试收据失效；收到 file_end 之前禁止调用文件处理器。

每轮仍使用已验收 Qwen3 模板与上游 Jinja 全文渲染，验证上次已提交字节前缀的 SHA-256，
只分词新增的完整消息后缀。特殊 token 消息边界必须与独立全文分词对照一致；
未知模板／前缀改变拒绝，不能静默复用。接收过程中不提交 generation header；
输入结束、完整片段处理结束后统一按源序加入有效收据，最后提交 header 并调用最终处理器。
处理器可在独立分支生成总结，不能改写仍由接收流拥有的会话历史。

原始内容只有一份有界事件存储，模型 token／KV 是各自必要表示。渲染文本／分词 vector
仅为单次工作 scratch，不缓存另一份 messages 历史。文件处理器拼接完整文件需要一份有界
临时缓冲，调用返回后释放；局部处理与最终输出分别计量。限制帧长、总内容、事件／文件版本数
和未处理 backlog，提供可观测背压，超限不静默截断或丢事件。

取消先禁止发布与最终提交，再在 native batch／文件回调边界停止。关闭等待线程退出，
最后经 owner 释放会话。回调不被强行中断；执行命令和根目录仍由部署提供，模型不决定。
首版不自动修改 agentctl，也不宣称 0.6B 的总结能力已满足实际任务要求。

验收包括任意字节切分、UTF-8／JSON 残缺与无效输入、顺序／版本错误、缺少 end、
不完整文件不运行、旧版本收据失效、容量背压、取消／关闭，以及全部 canonical token
与独立全文分词、首 token 与批量基线一致。完整文件处理用隔离临时目录里的真实 C 编译／执行。
性能对照用相同事件边界、内容、模型、资源限制，比较等待全部输入后处理与持续预填充＋
完整片段处理；单列接收结束后等待、完整总耗时、渲染／分词／decode 开销、局部工具开始／结束时间。
另跑无间隔反例以显示分批执行的额外成本；只报告真实回放策略和样本数，不预设必须提速。

## 流式宿主实现与验收入口

实现为 `tests/agent_stream.h/.cpp`，继续借用 `AgentKvWorker`，不新增另一套模型／KV 所有者。
宿主 `feed` 重组 NDJSON；调度线程调用既有 Jinja 快路径，并通过 owner 回调分词／追加／预填充；
独立片段线程只在 file_end 后调用部署配置的文件处理器。接收期间可以查询完成收据，
把失败结果交给上游并继续发送修正版；不要求先结束整个输入流。

每个 text／file_chunk 的内容是一个独立的、本地 user 消息单元，文件标记带 id／version。
文件处理器收到去掉消息标记后按顺序拼接的原始文件字节。**这与把任意网络分片逐个当成
同一条消息追加不同**：网络分片先重组事件；首版不接受未经边界确认的裸文本增量，也没有
实现具体厂商 SSE／工具参数字符串的流式解码。比较的三种策略使用相同事件边界和完整最终 prompt，
不能把事件切分带来的 prompt 变化当作相对整段普通聊天请求的性能优势。

接收时暂不提交 assistant generation header；结束并取得片段结果后按源顺序追加仍有效的收据，
再提交 header／flush，调用最终处理器。更新文件版本会使旧收据的 stale 标记变为 true；
旧结果仍可通过收据接口读取，但不会当作当前文件的验证结果送进最终 prompt。
每次完整渲染先验证已提交前缀 SHA-256，只分词新增消息后缀；全部 canonical token 必须
与完整最终 prompt 的一次分词一致。模板只接受已验收 Qwen3 hash 和独立控制 token。

来源正文／初始上下文合计上限 64 KiB、wire 帧 16 KiB、事件 128 个、文件版本 8 个。
持续处理模式限制 16 个未提交事件；队列压力返回 FULL，consumed 可以包含保留待重试的完整帧，
调用者等待后用 feed(NULL,0) 重试它，不能重发已消费字节。报告 failed 区分终止错误和临时压力。
canonical 事件字符串保留一份；Jinja 工作树／完整渲染／分词数组是临时工作内存，全文渲染及
前缀 hash 的线性成本仍存在。文件处理临时拼接缓冲的峰值单列，收据每条最多 4 KiB。

策略：`defer_until_end` 等输入结束并完成文件处理后才统一预填充；默认持续处理允许上游最小
批次门槛合并 token，`flush_each_update` 是立即处理小批次的对照。积压时调度器会合并已经到达的
事件，因此立即策略不保证“一事件一 decode”。所有模型操作仍串行；并发来自接收、文件工具和
本地推理的重叠，不代表同一张设备上多次推理免费并行。

取消在 native batch／文件回调边界生效，不再发布最终结果；close 等模型调度、文件处理线程
退出后释放会话。片段回调和最终回调的 ctx 必须活到 close，worker 必须比 stream 活得更久；
宿主不得同时直接修改这个 worker。最终处理器应 fork 独立分支生成，保持接收会话不变。

可运行入口 `agent-stream-replay` 同时接受文件或 stdin。CLI 对外部文件仅做只读接收／本地预览，
不编译／执行输入代码；真实编译测试只由验收程序为受控 fixture 配置 `test.run`。
输出为最多 16 token 的本地预览，带 ended 标记；不是完整总结能力验收。

```sh
python3 tools/build_agent_native.py --test-stream --out build/stream-local
python3 tools/build_agent_native.py --bench-stream --repeats 7 --out build/stream-bench-local
python3 tools/build_agent_native.py --test-stream --test-kv-worker --test-long-context --sanitize \
  --out build/stream-sanitized-local
python3 tools/build_agent_native.py --replay-stream docs/examples/local-stream.ndjson \
  --out build/stream-replay-local
# 已构建后，异步接收 stdin，不再重编译（模型／runtime／scratch 用实际路径）：
build/stream-local/agent-stream-replay /path/to/model.gguf /path/to/runtime /path/to/scratch replay -
```

`make test-stream` 为可选入口。示例事件见 [examples/local-stream.ndjson](examples/local-stream.ndjson)。
不是默认 agentctl 的云端流接入；没有网络、计费或新凭据配置。

### 流式流水线实测（2026-10-30）

固定 Apple M5／16 GiB、Qwen3-0.6B-Q8_0、b10938、8192-token KV 池（896 MiB）、
4 session、4 CPU 线程、prefill budget 512、引擎最小批次 128。三个策略：
等待全部输入再处理文件／统一预填充、持续合批、持续立即处理。每档预热一轮，
随后轮换顺序各 7 样本；不同时运行其他模型／性能测试。初始 538-token 公共上下文
已预填充，模型初始化及该公共准备不计时；本轮不做磁盘 KV 换入，测的是新增计算与工具工作的重叠。

输入为 16 个不可改写的内容单元和结束标记，网络喂入每次最多 7 字节。
回放间隔为 0／20／100 ms，结束标记紧跟最后一个内容单元，**不额外留等待间隔**。
纯文本最终 1239 token，其中新增 701；文件组最终 1157 token，其中新增 619。
所有策略的完整 token 序列与首个 greedy token 一致，新增实际评估 token 数恰好等于新增 token 数；
没有重复计算整个前缀。这里仍是受控回放，不能当作真实云端生成速率或任意 SSE 接入。

以下为中位数，对照默认持续合批策略：

| 输入与任务 | 输入结束后到模型可用：整批→合批 | 从输入开始到模型可用：整批→合批 |
|---|---:|---:|
| 纯文本，无输入间隔 | 143.9→145.5 ms | 144.0→145.6 ms |
| 纯文本，每 20 ms 一段（约 320 ms） | 151.8→47.1 ms | 473.5→367.9 ms |
| 纯文本，每 100 ms 一段（约 1600 ms） | 161.3→35.2 ms | 1764.4→1637.2 ms |
| 完整文件编译测试，每 20 ms 一段 | 737.3→299.0 ms | 1061.4→619.0 ms |
| 完整文件编译测试，每 100 ms 一段 | 773.1→52.1 ms | 2378.1→1655.5 ms |

模型可用指最终 prompt／有效测试收据／generation header 的 KV 全部准备好，
不等于完整总结输出结束。首 token／16-token 预览结束时间也单独记录；最终回调还包含
测试用的完整分词验证，因此预览计时并非没有验收开销的生产生成基准。

文件组在第 4 个事件 file_end 后启动真实 `cc unit.c -o unit && ./unit`，
输入源继续送 12 段文本。100 ms 组中，持续策略的 7 个测试均在输入结束前完成，
整批策略在输入结束后才开始。合批总时间中位数约降低 30.4%，20 ms 组约降低 41.7%；
20 ms 组测试未能在输入结束前全部完成，但一部分测试工作与预填充已经重叠。
传统基线在最终文件结果到齐后才预填充，不能据此声称所有现代异步框架都慢这些比例。

更多并行不等于更少执行开销。纯文本 100 ms 组：

| 策略 | 输入评估 token | 输入 decode 调用中位数 | 本地 prefill 壁钟累计 | 输入结束后等待 |
|---|---:|---:|---:|---:|
| 整批 | 701 | 2 | 159.0 ms | 161.3 ms |
| 持续合批 | 701 | 6 | 259.5 ms | 35.2 ms |
| 持续立即 | 701 | 16 | 396.5 ms | 28.5 ms |

立即处理比合批少等约 7 ms，却支付更多本地执行时间／调用；没有测能耗或 FLOP，
不能把这些时间直接换算成算力节约。无间隔纯文本中，合批和立即策略都没有改善总耗时，
说明收益来自等待重叠而不是把同一工作强拆得更快。文件组无间隔时，工具与模型准备仍可重叠，
但该组时间波动较大：整批 561.6–1262.1 ms，合批 468.3–1300.3 ms，不能作普遍提速保证。

持续合批在纯文本 20／100 ms 和文件 20／100 ms 四组的 7 对样本中均更快。
文件工具耗时与部分渲染壁钟有明显波动／膨胀，可能包含调度和资源争用，未做调用栈或锁分析；
总体中位数差不能全部归因于某一个并行阶段。样本不足以报告可靠 p95／p99，
也未验证 Windows／CUDA、真实云端或多个本地模型竞争同一设备。

可复现数据：`build/stream-final3-20261030/`，126 条正式计时、63 条实际文件工具时序。
取消／顺序／版本／残缺输入／容量／背压／关闭清理以及真实失败收据→修正版→成功反馈已通过；
完整 token 与一次分词一致，兼容样例另与从头预填充对照首 token。
ASan／UBSan、既有 KV worker、14K 原生聊天和默认 `make test` 均通过，预编译上游库未插桩。
验证目录 `build/stream-sanitized-20261030/`；文件／stdin／错误输入 CLI 验证
`build/stream-replay-cli-20261030/`。持久样本及源码哈希见
[evidence/stream-pipeline-20261030.json](evidence/stream-pipeline-20261030.json)。
既有原生 CLI 验证在 `build/stream-native-cli-20261030/`，配置隔离、任务重置与生成上限继续通过。

## 后续设计：提前意图与临近提交预热（2026-10-30，未实现）

本节原提纲已扩展并迁至 [D-PREP：提前意图与按需预热 v1](design/prewarm-v1.md)，
它是该主题的唯一详细契约。本锚点保留作跳转，不再平行维护第二份方案。
新版补齐了接口与源借用、分层 ready、Native/Stream 所有权整合、版本与原子交接、
时机与容量、取消／失败、异步冷启动和 P0–P4 验收；均未实现、未增加性能成绩。

## E38私有decode时序诊断

`src/llama_internal.h`提供可选owner-only累计时序观测：llama_decode与随后显式同步的wall／owner CPU分别记录。默认关闭，无公开ABI变化；不改变session ready、版本、失败清理或KV语义。调用返回跨度不等于设备核函数时间，decode内部可能等待，sync含剩余异步工作和调度。宿主实验契约及结果见[D-PREP §30](design/prewarm-v1.md)；统计对应各自源码归档。

### E39可选prefill内部合并同步

私有owner-only开关允许在同一次prefill内部延迟显式host同步；公开prefill返回前无条件排空未完成工作，保持evaluated/greedy/fork/save/失败失效语义。预算为0或不足min无新工作时不新增同步，直接greedy重放保持即时同步。默认strict。defer不跨公开API或工具边界持有pending工作，不改变canonical存储或物理KV。上游可在下一decode内等待，因此不能只用sync计时下降当提速，见[D-PREP §30](design/prewarm-v1.md)。

## E42–E44 私有设备观测与尾批策略（2026-10-31）

`src/llama_internal.h`增加owner-only trace observer及`cxf_llama_chunk_policy(engine,cap,coalesce)`，均非稳定C API；生产默认observer为空、cap0（使用已分配batch）、coalesce false。cap超过已分配容量返回RANGE且原设置不变，0恢复容量默认。可选合并只有剩余token完整落在本次budget和已分配batch内才发生；预算、最低batch、flush、revision与公开返回排空的契约不变，greedy重放仍strict同步。

Metal observer只链接专用测试harness，固定b10938运行时不改。GPU命令区间、CPU driver调度、通知与host返回的区别，以及实验与负结果，以[D-PREP §31](design/prewarm-v1.md)和[统一报告](benchmarks/README.zh-CN.md)为准；不是跨平台稳定的Metal adapter。E44两组均分配1024 batch，不能将其与旧512容量结果直接相减；物理KV不变，计算缓冲区增大单列。默认C参数与默认CLI batch仍512。

## E45/E46 计算容量配置（2026-10-31）

本轮没有修改推理核心或参数结构。既有`batch_tokens`同时设置上游n_batch与n_ubatch；固定b10938按微批上限建立最大计算图。显式640容量能容纳约600-token增量，初始较大上下文仍走既有分块。候选参数只在打开引擎时确定，不在任务中调整；默认仍512。

640容量计算预留为Metal373.44MiB／CPU15.02MiB，对比1024的597.50／24.03MiB，KV均896MiB（8192-context基准）。这是初始化计算图预留，不是峰值RSS／KV回收，也不建立动态资源预算器。640的14K验收由测试入口`CXF_TEST_BATCH`选择参数，非生产环境变量接口；其Native canonical／greedy及默认strict Stream回归通过。完整旋转容量性能与负结果见[D-PREP §32](design/prewarm-v1.md)。

## E47–E49 私有逐批观测（2026-10-31）

`cxf_llama_trace_batch_observer`是owner-only诊断callback，默认NULL；decode提交前通知逻辑position／tokens。测试harness将其关联到Metal buffer batch ID，生产不链接ObjC probe，也不分配第二份历史。回调不修改budget、ready、revision、失败清理或公开返回同步；buffer跨度不等于纯kernel忙时，batch也不保证一对一对应kernel。

E47同容量工具隔离、E48固定shape的算术／内存压力和E49较大量子负结果见[D-PREP §33](design/prewarm-v1.md)。E48共享root物理保留2048 cells，逻辑768 prefix只是受控布局，不作为生产单会话上下文缩放曲线。新增hook后最终640容量14K／strict Stream／默认回归通过；稳定C参数结构不变。
