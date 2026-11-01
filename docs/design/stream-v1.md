# Stream 宿主 v1

| 项目 | 说明 |
| --- | --- |
| 契约 ID / 状态 | D-STREAM / 当前实现契约，2026-10-30 从源码与阶段记录整理 |
| 实现 | `tests/agent_stream.h/.cpp`；file/stdin runner：`tools/test_stream_replay.py` |
| 验证 | [E11](../evidence/stream-pipeline-20261030.json)，固定 Qwen3/b10938、macOS/Metal |
| 范围 | 中立 NDJSON 完整单元回放；未接真实云端 SSE 或 agentctl 自动流式调度 |

早期设计与性能过程保留在 `docs/llama-context-v1.md`。本文件规定当前行为；[D-PREP](prewarm-v1.md) 中的动态环境、提前 tools 和异步 open 尚未实施。

## 1. 所有权与线程

Stream 借用一个 [KV worker](kv-worker-v1.md)，从 open 到 close **独占使用**。宿主 API 由调用者串行；close 不得与 feed、查询、finish 等并发。不能再给同一个 worker 叠一个独立调度器／Native bridge。

源事件存在一份有界 deque 中，接受后不可改写；调度器构造本轮临时 Jinja 值／渲染文本，通过 owner 做 tokenize、append、prefill。独立文件处理线程运行已闭合文件的回调。engine 仍只有一个 owner，宿主和文件回调不能并发调用它。

open 会同步验证固定模板、创建 session、预填 system＋initial_context，再启动调度／文件线程；不是异步冷启动。system 在打开时固定，最多 512 bytes；渲染 tools 固定为空。初始上下文按一个不可变 user 单元加入。

回调 ctx 必须活到 Stream close 返回。文件回调的文本／ID 只在该次调用内借用；最终回调的 prompt 也只在该回调内有效。最终回调若生成，应 fork；不得改写／关闭接收 session。Stream close 经 owner 关闭自己的接收 session，worker 由外部关闭。

## 2. 输入与版本

每个 NDJSON 行是一个完整 JSON 对象，以换行完成帧。seq 从 1 严格递增；只接受正整数范围内的 seq/version。kind 为 `text`、`file_chunk`、`file_end`、`end`：

| kind | 必需字段 | 行为 |
| --- | --- | --- |
| text | seq、text | 接受一个不可变的独立本地 user-message 单元 |
| file_chunk | seq、id、version、text | 加入该文件版本；同版本闭合后不能继续追加；新版本必须更大 |
| file_end | seq、id、version | 必须匹配当前未闭合版本；标记完整，允许文件回调 |
| end | seq | 最新各文件版本必须闭合；之后不再接受事件 |

ID 长 1–64，限字母／数字／`_-.` 且不为 `.` / `..`。wire 可任意切字节，包括 UTF-8／JSON 转义内部；重组完整帧后验证，不能直接把云端 token ID 或任意未闭合文本提交到 canonical 池。

每个 payload 是一条独立消息，不是把“多段 text”自动拼成同一个 user 消息。比较基线时必须保持这个边界相同，否则可能改变 token 与语义。

## 3. 背压与容量

| 资源 | 当前上限／统计含义 |
| --- | --- |
| wire 帧 | 16 KiB；帧缓冲过长明确失败 |
| 源 payload | 64 KiB，按原始内容字节累计；system、文件标签等另计，不是全部分配总量 |
| 事件 | 128，包括初始上下文与文件结束事件占用；不含 end 的源正文槽位 |
| 文件版本 | 最多 8 个版本记录，旧记录仍占位，不是 8 个可无限换代的文件 ID |
| 流式 backlog | 16 个尚未提交的源事件；wait-all 对照以总事件容量为界 |
| 单次 receipt | 4096 bytes；回调报告超长或无效 UTF-8 导致失败 |
| prefill budget | 1–512；仍服从核心最小 batch，final flush 才绕过最小阈值 |

backlog FULL 时模块可已保留完整帧，`consumed` 包含这些字节；调用者等待后先 `feed(NULL,0)` 重试保留帧，再继续尚未 consumed 的字节。不得把整个输入块重发造成重复 seq。区分可恢复背压和 report.failed 的终止容量错误；任何超限均不静默丢事件。

文件回调拼接完整文本有一份临时 scratch，回调返回后释放。Jinja、渲染、token、KV 是不同表示；不能把源内容 64 KiB 上限当成进程只分配 64 KiB。

## 4. 前缀、收据与最终准备

只对已验收 Qwen3 模板使用完整渲染＋上次已提交前缀哈希验证；只分词新增完整消息后缀。接收期间不提交最终 generation header。前缀变化拒绝，当前 Stream 不在后台重建成另一套历史。

file_end 后处理线程才启动回调；一个处理线程按版本记录工作，不能抢占已经运行的旧版本回调。新版本使旧收据 stale；旧回调尚未结束时，finish 可能仍需等待。旧未运行版本可以跳过。查询接口保留历史收据及 stale 标志。

**旧版本源事件仍保留为历史输入。** 最终 render 追加的结果只包含最新版本收据，并按 file_end 顺序排序；这不等于自动删除旧源码／压缩上下文。旧稿与修正版通过事件标签和版本号区分。

文件回调返回错误也是结果事实，可形成失败收据；“收据就绪”不表示代码测试通过。模块不自行执行 receipt 中的命令；具体处理器和权限由部署提供。

finish 要求已经接受 end、没有残缺 wire 帧，等待文件工作和当前有效收据，追加最终 header，flush 到 evaluated=committed，再调用最终回调。`model_ready_ns` 表示完整请求的 KV 可用，尚未执行最终回调；不等于生成结束。

## 5. 取消、失败与关闭

cancel 禁止后续接收／最终发布并唤醒线程；已经运行的 native／文件回调在调用边界结束。取消不会撤回工具已经发生的外部副作用。close join 调度与文件线程，再由 owner 释放接收 session；回调不返回时 close 也可能等待，部署的进程回调需有自己的超时与收割规则。

无 end、最新文件不闭合、非法顺序／版本、UTF-8／JSON、超限、native 错误等均显式报告。finish 缺少 end 也会进入错误终态，不能用无限等输入来掩盖协议错误。错误不会自动扩大容量或转 HTTP。

当前 Stream 是为一次输入及其文件修订的流水线，不提供通用多轮会话重写、动态 tools/system、任意正文回滚或 preemptive callback cancellation。这些需要新版本／D-PREP 集成验收。

## 6. 验收与测量

E11 的正确性包含 wire 任意切分、Unicode／转义、完整分词与从头 greedy oracle、真实 C 测试、结束前失败反馈→修正版、部分文件不执行、序列／容量、stale 收据、在途取消、背压和清理。旧源码保留与旧收据排除的区分在本轮契约整理中明确，未新增测试成绩。

正式性能数据来自 `stream-final3-20261030`：结束帧紧跟最后 payload；旧试跑存在额外 EOF 间隔而被取代。6 场景×3 模式×7 样本，初始 538-token 上下文与模型加载排除，不能据此估计冷启动。text/file 输入实际新增 701/619 token，各模式一致。

总 ready、输入后等待、累计 prefill、decode 次数、工具时间和最终预览分开报告。预览回调含测试用全量分词 oracle，最多输出 16 token；不是完整总结质量／生产 TTFT 基准。无间隔无改善与高波动组保留，详情见 [报告](../benchmarks/README.zh-CN.md) 与 [方法](../benchmarks/METHODOLOGY.md)。

可选验收 `make test-stream` / `tools/build_agent_native.py --test-stream`，回放 `--replay-stream`。当前没有厂商 SSE、Windows/CUDA、多模型竞争或自动 agentctl 调度成绩；此次仅补契约。

## 7. E28 常驻／真实进程源验证（冻结，2026-10-31）

本轮只新增调用现有API的独立collector，不改变Stream消息／独占worker／容量／取消契约。无save/evict/restore。真实本地producer从固定文件读取12个语义单元，做实际CPU工作后输出NDJSON，完整帧到达后按同协议提交；两档payload×突发／计算paced源，wait_all/coalesced/eager各7任务，无sleep造窗口。source生产时间／接收时间／背压、累计工作、输入后等待与完整TTFT均保留，end立即跟最后payload。

首greedy在测试oracle之前取；完整canonical分词与fresh-KV参考在首token之后验证，不混为模型总结质量。初始化resident历史、load和producer首次执行暖机单列；完整任务从producer启动计时，计入源执行及与GPU的竞争。每块仍是独立不可变消息，tools固定为空，不宣称接通厂商SSE或Native多轮工具协议。与E27真实Native多轮工具分开报告，详细冻结规则见[D-PREP §23](prewarm-v1.md#23-e27e28-正常常驻流程验证实施前冻结2026-10-31)。

首次ASan验收在大输入的独立fresh-reference处触发共享8192池的slot不足：接收session和诊断参考各一份KV之和超过容量；9个已完成验收任务及失败日志保留，不计正式性能。修正为接收session按正常任务生命周期close后，再在同worker上单独重建fresh参考；保持8192容量、输入／模式／工作量不变，不在活跃任务内逐出，不扩容掩盖诊断占用。参考仍独立核对已捕获token／greedy，首token计时不含该诊断。

E28正式87条完成（84任务＋3状态），四场景各三模式七次；4个独立fresh参考、所有canonical／greedy一致。实际CPU-paced源的合批／急切完整任务逐对收益为80词136.054／159.388ms、240词213.587／234.151ms，各7/7更快，已计正常close；突发输入四比较的中位反慢.975–8.827ms，负例保留。额外小批次执行和producer变慢亦收费，不能只报输入后等待。所有同场景输入相同；0／4000万场景digest及token数量不同，不跨场景相减。当前默认／tools空schema／独占worker／未接云SSE边界不变。完整结果见[D-PREP §24](prewarm-v1.md#24-e27e28-常驻结果与当前选择2026-10-31)及[E28报告](../benchmarks/README.zh-CN.md#e28--真实进程源的常驻流式预填充)。

## E29接线边界（2026-10-31）

E29已经把真实agentctl/test.run的有界stdout/stderr接入Native当前会话预填充，详见[D-PREP§25](prewarm-v1.md)。它复用Native工具协议／稳定checkpoint，不是把本Stream独占接收session叠到Native上；本契约的tools空schema、独占worker和厂商SSE未接限制仍成立。E29性能独立记录，不能升级E11/E28为真实agentctl成绩或相加收益。
