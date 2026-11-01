# KV owner worker v1

| 项目 | 说明 |
| --- | --- |
| 契约 ID / 状态 | D-KV / 当前实现契约，2026-10-30 从源码与阶段记录整理 |
| 实现 | `tests/agent_kv_worker.h/.cpp`；`src/llama_internal.h` 的私有序列文件接口 |
| 验证 | [E10](../evidence/kv-worker-20261030.json)，macOS/Metal、固定 b10938；默认 Agent 未自动接入 |
| 边界 | 一次 worker 一个 engine，一个 owner 线程；没有预测器／优先调度／通用磁盘缓存 |

本文件维护当前 API 行为。早期目标、实现过程和性能表保留在 `docs/llama-context-v1.md`；[新预热设计](prewarm-v1.md) 不能被读成 worker 已有能力。

## 1. 所有权与调用纪律

worker 在 owner 线程创建／销毁 engine，所有引擎调用都经 owner 执行。宿主可以并行做自己的上下文装配；同一 worker 的同步回调不能重入 worker，也不能把 engine 指针留到回调之外使用。多 engine 的后端全局初始化仍须由宿主串行安排。

模型／后端路径在 open 时复制，参数的 allocator 按 owner 契约使用。`open` 等待模型加载完成，当前不是异步冷启动入口。同步回调在某个宿主线程等待时，其他宿主线程仍可排队请求或 poll/cancel；实际 engine 调用始终串行。调用者应协调同一 ticket 的消费与依赖顺序，不能把多个 API 当成一个自动事务；close 不得与其他 API 并发。同步回调的 ctx 只需活到调用返回；异步准备 ticket 不保存宿主全文上下文。

## 2. 资源与 snapshot 身份

| 对象 | 上限／身份 | 回收 |
| --- | --- | --- |
| 等待队列 | 16 项，FIFO | owner 取出；满时返回 FULL |
| ticket | 16 个未消费 ticket，单调 ID | finish 消费；取消不自动消费；close 可排空 |
| snapshot | 8 个，单调 ID | drop 或 close；被任何未消费 ticket 引用时 drop 返回 FULL |
| 文件字节 | open 显式传入总预算 | 保存前检查剩余额度；drop 成功才归还 |
| engine/session | owner 身份＋slot 世代＋revision | 跨 engine、旧世代、相关修订变化均不可恢复 |

snapshot 仅描述当前 engine 中仍存活、完整已评估且 revision 不变的 session。文件只写 KV 状态，不附带 canonical token 列表（token_count=0）。保存不会改变原 token 所有权，也不会自动逐出；多个文件可以重复序列化同一个共享前缀。

私有临时目录为 0700。保存完毕后登记文件设备／inode、类型、长度和修改／状态时间；恢复前后检查身份，恢复前还检查版本头及零 token_count。正常替换／截断拒绝；没有全文件加密校验、外部导入、跨进程恢复、fsync 持久化或崩溃恢复承诺。正常 close 删除文件，异常进程退出可能遗留临时目录。

## 3. API 与结果语义

| API | 完成含义 / 注意事项 |
| --- | --- |
| `run` | 同步执行 owner 回调；返回回调结果，不能在回调中再调用 worker |
| `save` | 同步生成私有 snapshot；必须 committed=evaluated>0；预算失败不发布 snapshot |
| `evict` | 同步删除该 session 的 KV 归属，evaluated=0；canonical token 与 revision 保留 |
| `prepare` | 只接受异步请求并返回 ticket；snapshot 的实际有效性在 owner 上检查，不等于恢复成功 |
| `poll` | 返回报告和 `ready`；**ready 是 ticket 已完成，包括错误／取消完成**，不代表恢复成功 |
| `finish` | 等准备结束、重新校验 snapshot/session/revision，再可选执行 owner 回调；成功排入并执行后消费 ticket |
| `cancel` | 标记取消，禁止之后提交；不强制打断在途原生恢复，不立即释放 ticket |
| `drop` | 无未消费 ticket 引用时删除 snapshot；文件删除失败保留登记和预算，便于显式处理 |
| `close` | 停止接收、排空队列／在途操作、关闭 engine、清理临时目录；不能被用作并发 API 的竞态取消 |

判断准备可用必须结合完成状态、`cancelled` 和 finish 的 `cxf_result`。poll 不单独返回已完成操作的最终 rc，宿主需 finish 收取结果。`ready_ns` 是完成时间，不能把失败样本当成成功恢复延迟。

若 finish 因 owner 队列满等原因未实际执行，ticket 尚未消费，`finishing` 恢复；调用者可以按报告／返回码处理后重试，不能丢掉资源。回调已经执行后其返回错误也消费 ticket，不能因此重新执行同一回调。

为区分“队列拒绝的 FULL”和“回调执行后返回 FULL”，应传入报告并检查 `finished_ns`：前者为零，后者在消费 ticket 前被设置。不要仅按返回 FULL 无条件重跑有副作用的回调。

## 4. 典型顺序与状态

```text
run(初始化/编辑 session) → save → 可选 evict
prepare(snapshot) → 宿主继续装配 → 可选 poll
finish(ticket, 提交后缀回调) → drop(snapshot)
```

已驻留且版本匹配时 prepare 可以直接完成而不读文件。部分 evaluated 的 session 不按“缺多少补多少”恢复；只支持完整驻留或完全逐出后的精确文件恢复。restore 后设备同步再发布 evaluated；logits 不随文件恢复，首次 greedy 可能仅重放末 token。

取消正在恢复的请求在原生调用返回后处理。若它恢复的 KV 只服务已取消请求，尝试逐出并记录 wasted_bytes；如果另一个未取消 ticket 仍需要同一 session/revision，则保留。释放一条序列归属不会缩小预分配 KV buffer。

finish 在 owner 上进入 `committing` 后，cancel 返回 BAD_HANDLE，不能再通过预热 ticket 撤回该回调。这是当前的提交边界；接活跃请求取消需要独立入口。后续协调器若要原子交接，遵循 D-PREP，不应先在宿主 poll 一次就自行采样。

## 5. 失败、兼容与限制

身份／revision 失效、文件格式／身份异常、预算／队列耗尽、native restore 错误分别传播显式错误。受损恢复清理相应 KV，保留 canonical 历史，不自动换模型、换引擎或重新发送 HTTP。是否重试由宿主决定。

版本校验严格作用于 snapshot 原 session，不能通过“parent 看起来差不多”放宽。没有任意块 paging、磁盘前缀去重或提前 CPU 读盘 staging pipeline。当前上游调用会同步 context；owner 忙时恢复会排队，不能承诺与另一项同 context 推理并行。

上游 tensor I/O scratch 峰值未单独跟踪；调用前后 RSS 不是峰值或独立显存。尚未完成原生 Windows/CUDA 文件／线程／ABI 验收，也未证明冷 SSD 或尾延迟稳定性。

## 6. 验收与性能证据

E10 覆盖驻留／磁盘往返、独立 token/greedy oracle、canonical 唯一所有者、资源上限、排队／在途／ready 取消、过期 revision、槽位复用、截断／未知快照、清理。ASan/UBSan 对 cxf/适配器插桩，上游预编译库除外。

性能在 OS 文件缓存暖态、相同快照与资源条件下比较提前／延后恢复；真实装配与人工等待窗口分组，保存成本单列，不能把 E01 内存态 restore 和本文件的磁盘 restore 混在一起。[全部指标](../benchmarks/data/metrics.csv) 与 [方法](../benchmarks/METHODOLOGY.md) 保留无收益／变慢样本。

可选入口：`make test-kv-worker`；runner `tools/build_agent_native.py --test-kv-worker` / `--bench-kv-worker`。新测量追加证据；本次补契约没有重新跑模型。

### E21 借用Native的宿主边界（实施前，2026-10-31）

工具准备切片经现有run及prepare/finish调用worker，不增加第二个engine或直接跨线程llama调用。Native在run回调中绑定／关闭借用对象；该对象仅拥有自己的session，不关闭worker的engine。宿主串行工具包装负责定时、取消／排空和最终prepare，最多一个准备任务；不是worker FIFO本身变为优先调度器。详细范围在[D-PREP E21](prewarm-v1.md#13-e21-工具执行窗口切片实施前冻结2026-10-31)。

E21上述借用边界已实现并经ASan/UBSan验收；现有worker FIFO和snapshot语义不变。性能归档分别记录native restore时间和宿主prepare总时；二者差值包含尚未细分的协调开销，不代表纯磁盘／总线传输。正式结果与负例见[D-PREP §14](prewarm-v1.md#14-e21-实现状态与正式结果2026-10-31)。

### E22 完成路径观测（实施前，2026-10-31）

AgentKvReport追加finish进入／owner开始／返回三个单调时间点；原queued/started/restore/ready/finished语义保留。finish owner开始仍先于验证与提交，返回仅是宿主收到同步结果；两者都不代替设备ready。无ticket／版本／取消行为改变，观测不调额外引擎接口。详见[D-PREP §15](prewarm-v1.md#15-e22-定时恢复等待归因实施前冻结2026-10-31)。

E22正式时间线将大段before-native延迟定位到与fork重叠的校验区间；queue／finish较小，不能一概归为KV读盘／传输。E23不改变worker恢复／取消内容规则，只在宿主用进程进度更新定时；详见[D-PREP §16](prewarm-v1.md#16-e23-进程启动与进度条件eta实施前冻结2026-10-31)。

E23在同worker上保留上述restore／finish／cancel版本规则；实际优化在宿主进程启动及进度ETA层。阶段时间显示fork干扰主要在verify附近，spawn／等待父进度均能避开它；内部libc锁未直接观测。progress仍有等待／空等权衡，不替换默认timed。当前结论见[D-PREP §17](prewarm-v1.md#17-e22e23-当前结论与选择2026-10-31)。

E24宿主可在finish返回、ticket已释放后立即调用drop，原快照仍归同一个worker所有；不改worker的“被ticket引用时拒绝删除”规则。原返回清理作为默认及失败兜底，提前清理仅复用已有准备线程，execute返回前排空。prefix_ready不等待文件删除，cleanup有独立时间与结果。详见[D-PREP §18](prewarm-v1.md#18-e24-完整任务归因与提前释放实施前冻结2026-10-31)。

### E25 unlink 与最后 close 隔离（2026-10-31）

正常resident工具窗口不需要snapshot；文件只在显式KV卸载/恢复路径使用。当前snapshot绑定同一live engine/session/revision，不提供跨进程／冷engine恢复；不会额外保存token列表，逻辑逐出也不缩小预分配KV buffer。不能为了测试提前恢复而把逐出变成默认操作。

`agent_kv_worker_unlink_hold`是有界诊断入口：只对没有未消费ticket引用的snapshot，验证私有文件身份、O_NOFOLLOW打开后unlink，fd只由owner持有。snapshot原槽位和used budget保留；正常drop才执行最后close并释放记账，close worker也排空fd。已unlink snapshot不能参与后续恢复；prepare沿原异步验收语义可返回ticket，但finish拒绝该恢复，不能被理解为提交成功。重复hold返回BAD_HANDLE，被ticket引用时返回FULL，失败不重新执行工具。操作仍使用原8槽位，不加隐藏无界retired队列。具体对照及验收见[D-PREP §20](prewarm-v1.md#20-e25-删除回收隔离诊断实施前冻结2026-10-31)。

POSIX文件引用语义按[Apple unlink手册](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/unlink.2.html)：最后目录项删除与最后引用关闭可以分开；这提供隔离控制，但不能从API语义推断本机回收一定耗时多少。资源字段为可选观测，不改变worker FIFO、ticket取消、版本或engine所有权。
