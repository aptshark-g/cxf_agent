# 本机预填充耗时估算 v1

契约 D-COST，更新2026-10-31。本轮先独立实现时间测算，再逐个实施 D-PREP 其他切片；这是用户对原顺序的调整。新估算器不启动预热、不改变 KV 或现有推理路径。

状态：独立经验／Roofline／形状表及残差 C 估算器与E13–E20实测已完成；这些复杂估算API仍独立。E21工具窗口另用按tool key的耗时／准备ms每token EWMA，未把本节Roofline／shape表自动接入调度。实现 `include/cxf/cost.h` / `src/cost.c`，单测 `tests/test_cost.c` 进入默认 `make test`；实测 runner 为 `tools/bench_cost_estimator.py`。首轮证据 [E13](../evidence/cost-estimator-20261030.json)，汇总见 [报告](../benchmarks/README.zh-CN.md#e13--本机耗时测算)。

当前新增Kalman层看文末E20，残差契约与冻结排查看 [E19](#e19-当前残差校准契约与验证)，形状查表仍按 [E17](#e17-当前形状估算契约与结果)，E15原因定位见 [阶段记录](#e15-定位结果与当前实现)；旧E13／E14段落保留当时口径，不能混作当前成绩。

## 范围与模型

机型、内存、模型／后端和执行配置组成 profile 身份，帮助判断能否复用标定；机型不直接给出有效吞吐。第一版仅估算固定 profile、暖态、无争用、一次完整 flush 的 prefill 服务时间；不估计 owner 排队、Agent ETA、冷加载、工具执行或任意程序复杂度。

```text
calls = ceil(new_tokens / batch_tokens)
pairs = prefix_tokens * new_tokens + new_tokens * (new_tokens - 1) / 2
estimated_ms = a * calls + b * (new_tokens / 512) + c * (pairs / 1048576)
a,b,c >= 0
```

这是经验工作量特征，不是精确 FLOP／GPU kernel 模型。batch、上下文、KV 类型、模型／量化、引擎、设备及线程配置改变均应换 profile。零 token 返回零；未标定不凭空返回有效速度，输出 EMPTY。超出标定范围标 extrapolated，不隐藏外推风险。

## 状态、纠正与代价

新增纯 C `cxf_cost_model`，固定 64 条数值观测，无动态分配、不记录原文／token 内容。每条 observe 更新有界窗口、非负最小二乘系数与拟合残差；predict 不修改状态。过旧观测逐出后重新适配，profile 不匹配拒绝。残差是观测误差，不是概率置信区间。

采样只纳入成功且完整完成的工作，单位 ms；失败、取消和未 flush 的操作不用于拟合。NaN／无穷／负耗时／非法工作量拒绝且原状态不变。源调用者串行使用模型。标定与本地硬件读取在独立 runner，内核不读取系统信息、不启动计时线程。

## 冻结的首轮实验

- 固定 Qwen3-0.6B-Q8_0、b10938、Metal、CPU4、batch512、8K FP16 KV；模型加载与初始图预热排除。
- 标定：prefix=0/2048/6144，new=32/128/512，每格 7 次，共 63 条。
- 评价：prefix=0/1024/4096，new=16/64/256/768，每格 7 次，共 84 条；新工作形状未参与初次拟合。
- 三种预测都使用同一实际耗时：固定 token/s 也用全部标定数据；frozen 模型不学习评价标签；online 每条先预测、测量后才 observe，不偷看当前值。
- 每个 prefix 分组单独持有该长度的根 session，避免保留更长根的 KV 改变实际 attention span；子分支共享前缀，只有后缀 prefill 入计时。
- 每个工作形状的 canonical token／greedy 与关闭旧会话后的从头计算核对；实际 evaluated token 和 decode 次数核对。
- 新证据 E13 追加保存；记录 MAE、相对误差、总绝对误差比、带方向误差、内插／外推分组、估算／更新耗时、固定状态大小。无收益或 online 变差如实保留。

独立 C 单测覆盖未标定、profile 隔离、非法输入不改状态、已知公式的未见形状、窗口逐出、速度改变后纠正、零工作和外推。ASan/UBSan、默认回归分别执行，性能不用 sanitizer。

本轮结果只回答预测是否更接近、代价是否足够小；自动调度、冷启动、KV 恢复时间和一般程序成本模型都留到下一轮。后续复杂度分析提供增长趋势，实际常数、分支／I/O 和硬件利用率仍需采样，不能由 Big-O 或机型推得任意程序的精确耗时。

## 实测与剩余边界

固定吞吐／冻结模型／在线修正的平均绝对误差依次为 68.594 / 15.991 / 10.440 ms，WAPE 为 65.87% / 15.36% / 10.03%。在线最大绝对误差仍为 75.717 ms，并非每条都优于固定吞吐。不能将误差改进写成任务提速。

状态 3120 bytes；十组计时的 predict 中位数 0.086 µs、observe/refit 3.365 µs。该开销单独测量，不包含模型执行；窗口 64 条，不分配堆内存。21 种形状从头 greedy 对照、准确 token/batch 计数、C 单测、ASan/UBSan 和默认回归通过。

实际模型 key/value head_dim 均为 128，不能直接以 embedding / heads 猜测；28 层、8 KV heads、FP16 给出 112 KiB/token 的 KV payload。物理 buffer、权重、scratch 和 RSS 仍需分开计量。profile 登记硬件、OS、模型、runtime 库哈希与配置；温度、背景负载和利用率变化仍可带来误差。

执行记录：首次 sandbox Metal 初始化失败无有效样本；第二轮 wrapper 后处理失败且与回归重叠，不纳入正式数据。E13 使用独立第三轮，默认回归已结束、完整 GPU offload 经日志核对；没有因误差更大而改用开发轮的更好读数。

## E13 误差诊断（基于已有样本，未新增测量）

当前实现是三个工作量特征的经验拟合，不是由硬件 FLOP／带宽参数构建的完整性能模型。63 条标定只覆盖 9 种工作形状；小 batch 的固定开销、不同形状的 kernel／padding 成本没有单独建模，误差不能全部归因于硬件资源争用。

| 已观察事实 | 可得结论 / 不可得结论 |
| --- | --- |
| 最大在线误差：prefix4096、新768，实际433.952 ms、预测509.669353 ms，差75.717353 ms（约17.45%） | 这是较长任务的最大绝对误差，不是典型任务都差75 ms；对临近提交调度仍太大 |
| 同一4096＋768形状七次实际433.952–508.856 ms | 静态工作量之外确有运行波动；缺少资源遥测，不能指认是GPU降频、其他程序抢占或某个同步阶段 |
| 标定6144＋32七次40.547–204.079 ms，中位52.8 ms | 标定存在尖峰，普通平方损失容易受大残差影响；不能把尖峰一律当作可删除的“错误样本” |
| 原标定新token上限512，评价有768 | 冻结模型在此为外推；仅有调用数／attention-pair特征不保证跨批量形状拟合准确 |
| 最大误差样本的online_extrapolated=false | 在线先前看过其他前缀的768；逐维min/max不等于该组合已充分覆盖，当前标记不能当置信保证 |

计时起点包住 cxf prefill，decode 中明确调用 `llama_synchronize`；因此是 CPU 提交、引擎工作和同步等待共同组成的墙钟服务时间，不是单独的 GPU 算术时间。仅从这些记录不能分离资源争用、热状态与模型失配的贡献。

诊断优先顺序：先补可观测的 CPU／native 调用／同步阶段与条件记录，再比较按 batch/context 分段和稳健拟合；分层保存正常波动与尖峰，不把删除异常后更好的读数替换旧成绩。GPU 执行时间若未独立暴露就保持未测，不能用 wall−CPU 冒充。调度前还需组合覆盖／不确定性与安全余量，新方法将独立追加证据验证。

## 结构量与硬件约束模型的原提议（历史）

实施更新：下面的原提议保留为背景；下一节契约现已实现并完成 E14 对照，结果及边界见文末。

用户指出带宽／算力已有推理公式。这个依据应纳入下一步，不能把机型只用作 profile 标签后就忽略可计算的物理约束。E13 是经验模型；当时以下方案尚无代码或性能成绩，现由下一节的 E14 实施接续。

对指定设备、精度／指令路径及内存层级的工作，Roofline 给出理论下界：

```text
T_lower = max(FLOPs / peak_FLOPs_per_second,
              actual_memory_traffic_bytes / peak_bandwidth_bytes_per_second)

T_kernel_estimate = max(FLOPs / effective_compute_rate,
                        memory_traffic_bytes / effective_bandwidth) + fixed_kernel_overhead
```

来源：[NVIDIA Roofline profiling guide](https://docs.nvidia.com/nsight-compute/ProfilingGuide/index.html#roofline-charts)。这是性能边界／估算结构，不保证任意 workload 达到峰值；不能把另一执行单元或不匹配精度的宣传 TOPS 代入。多个融合 kernel／算子的依赖按关键路径组合，不能对整段模型取一次 max 就假定所有计算／传输都重叠。

结构量由模型层、矩阵形状、GQA、量化、context 和 batch 计算，带宽和算力用可核验规格／同一后端微测给初始先验。微测有效带宽也要标操作类型、字节范围和缓存冷热；一项 memcpy 吞吐不自动等于 attention 或量化矩阵乘法的有效吞吐。

需要区分：KV 容量公式得到存储量，不等于某次 prefill 的 DRAM 流量；权重重用、query tiling、缓存命中、重复读取和融合会改变实际 traffic。量化权重的解码／反量化与元数据也应计入相应指令和字节路径。原文／token／KV 的容量估算不能直接当搬运量。

后续实现以“结构量与硬件先验＋少量有效算力／带宽／固定开销校准＋在线误差修正”为候选，与 E13 经验模型使用同一新测量集比较。先核对 F/M 的推导及计时边界，再诊断分段与尖峰。并列报告理论下界、实际预测和误差区间；不以理论上限替代真实验收。

## Roofline 接入契约（已实现，2026-10-30）

- 保留原经验估算器，新增兼容的纯 C Roofline 入口。当前限均匀层 dense Qwen3、GQA、FP16 KV；矩阵形状及每组量化字节从 GGUF 元数据核对，不从参数量或 embedding/head 猜测。
- 每次 decode chunk 计算 Q/K/V/O、SwiGLU 三个线性层的 GEMM FLOPs=`2MNK`，加 attention 的 QK/AV 运算量；末尾 logits 只按实际一次输出计算，不把整个词表投影乘以所有 prefill token。
- 单组时间=`max(F/effective_compute,B/effective_bandwidth)`，按依赖组／层求和；attention 使用每 chunk 一次 KV 读取的 operand-traffic 代理，尚未测真实 DRAM 流量，不能称严格硬件下界。RMSNorm/RoPE/激活及 fusion、tile 重读差异由误差／校准反映，明确列为模型限制。
- 使用相同 GGML/Metal 后端独立探测 Q8_0 GEMM、F16 GEMM 和 64 MiB copy；每项预热后 7 次，不用完整推理标签选择硬件参数。copy 按读＋写两倍字节计；探测包含同步墙钟开销，只作该操作的有效吞吐代理。
- 校准版本使用三个非负系数，分别纠正 linear 组、attention 组、decode 调用数；与旧三参数模型保持相同标定标签量和参数个数。共享私有数值拟合实现，不改旧 public API 或原模型的工作量公式。
- E14 在同一次新运行中重测63条标定、84条未见形状评价；同时预测 fixed、原 frozen／online、raw Roofline、calibrated Roofline。新公式校准只用标定数据，评价不回灌；无收益／变差仍保留。
- 独立测试覆盖已知矩阵 FLOPs/字节与 roofline 聚合、GQA、最后 logits、零工作、缺失／非法速率、溢出／状态不变、profile 隔离及共享拟合回归。真实模型仍做21形状从头 greedy 和 token/batch 计数；单测／sanitizer／默认回归与性能分开。
- 新接口只加入估算工具，不改变实际推理、自动预热或调度。缺乏硬件有效参数返回 EMPTY，不将假定值写成实测；E13 与更早证据保持不变。

公式来源：[NVIDIA GEMM 性能说明](https://docs.nvidia.com/deeplearning/performance/dl-performance-matrix-multiplication/index.html)、[Roofline](https://docs.nvidia.com/nsight-compute/ProfilingGuide/index.html#roofline-charts)；[FlashAttention](https://arxiv.org/abs/2205.14135) 说明 IO 随分块／缓存策略改变，因此本轮 operand bytes 仅是明确假设的代理。

## Roofline 接线、生命周期与计算口径

新增 `include/cxf/roofline.h` / `src/roofline.c`；`tests/test_roofline.c` 已进默认回归。GGUF 解析和 GGML/Metal 微测仅在测试 helper `tests/roofline_probe.h`，核心保持无引擎依赖，宿主也可传入其他后端已测参数。测试 runner 为 `tools/bench_roofline_estimator.py`；当前该 runner 只验收 macOS/Metal，不能视作 Windows/CUDA 已通过。

`cxf_roofline_estimate(spec, device, work, out)` 返回原始时间、linear/attention 分量、算术时间和访存时间之和、主导 FLOPs 与 operand bytes。后两项不是 profiler 实测流量／总指令量。`init/predict/observe` 提供可选校准，特征为 `[linear_ms, attention_ms, decode_calls]`；与原经验模型复用私有三变量非负拟合，无 API 替换。宿主自行选择估算器，本轮不默认切换策略。

每个 chunk 的 token 数 n，已驻留上下文 c，query heads Hq，KV heads Hkv，K/V head dim dk/dv：

```text
每个线性矩阵 (K,N): F = 2*n*K*N
                         B = quantized_weight_bytes + activation_bytes*n*(K+N)
attention causal_pairs = n*c + n*(n+1)/2
attention F = 2*Hq*(dk+dv)*causal_pairs
attention B_proxy = kv_bytes*(c+n)*Hkv*(dk+dv)
                  + activation_bytes*n*Hq*(dk+dv)
logits F = 2*embedding*vocabulary   # 当前 native decode 每 chunk 仅一个位置
每组时间 = max(1000*F/effective_FLOP_per_s, 1000*B/effective_bytes_per_s)
```

线性七组和 attention 组按层累加，再加每 chunk 的 logits／embedding 查表代理。attention 不物化完整 NxL score；一次 KV pass 是简化，query 分块／缓存可能使真实 traffic 不同；dense FLOPs 不计反量化、norm、RoPE 等实际指令。Q8 GEMM 微测和 F16 GEMM 微测也不能精确代表所有线性形状与 FlashAttention kernel。

输入是借用的只读数值结构，调用结束即不再使用；不保存原文、token 或 KV。raw 接口无内部状态／取消操作；每份校准状态 3120 B，最多64个观测，调用者串行 observe/predict。profile 必须区分算法、机型、模型结构／量化、引擎／配置与有效速率；实验基准以基础 profile 加实际 rate 位模式生成专用校准 tag，禁止换速率后复用。没有持久化、网络、后台线程或新资源所有权。

缺失/零速率返回 EMPTY；非法结构、速率、溢出或超过4096个 chunk 返回 RANGE；失败不写输出／修改校准状态。零 token 返回零时间；缺少标定的非零校准预测返回 EMPTY；profile 不匹配拒绝。外推标记仍只是逐维范围检查。

## E14 验收与负结果

[E14 原始证据](../evidence/roofline-estimator-20261030.json) 195条记录：147个标定／评价任务、21条微测、两类开销各10条及元数据／模型／正确性记录。每种预测用同一个实测任务；仅校准63条标签，84条评价不用来修改两个冻结模型，原在线模型先预测后学习。各形状预热、模型加载和前缀建立排除；21形状从头 KV greedy、canonical ID、token/batch 计数通过。C 单测、ASan/UBSan、默认回归均通过且先于 GPU 性能运行完成。

| E14 预测 | MAE ms | WAPE | 最大绝对误差 ms |
| --- | --- | --- | --- |
| fixed token/s | 51.115 | 58.63% | 195.662 |
| 原 frozen | 5.526 | 6.34% | 18.811 |
| 原 online | 3.931 | 4.51% | 11.377 |
| 原始 Roofline | 236.315 | 271.04% | 1124.549 |
| 校准 Roofline | 5.281 | 6.06% | 18.808 |

同后端微测有效 Q8 GEMM=1.785 TFLOP/s、F16 GEMM=0.705 TFLOP/s、64MiB copy读写=37.387 GB/s（十进制单位）。不是 M5 峰值或完整推理吞吐。原始公式的大幅高估表明当前速率和流量／算子近似的组合不能直接预测；尚未拆分各误差来源。校准系数分别为0.167690301、0.281370605、11.10336185，不能解释为通用硬件利用率。

校准 Roofline 相对原 frozen 的 MAE 低约4.4%，差0.245ms，单轮不足以声称稳定优势；原 online 仍更好，学习口径也不同。raw 单 chunk 估算十组中位开销0.0144µs，无堆分配；完整校准 predict/refit 开销本轮未单独计时。E13 的旧数值保持不变，本轮整体波动较小不能归功于新公式，因为新公式没有改变执行路径。所有结果是预测质量／估算开销，不是模型推理速度提升。

后续可按真实 kernel/形状细分有效速率、测流量或比较稳健拟合，再以独立新证据验收；本轮先交付可调用且可复核的标准公式基线，不据此承诺精确就绪时刻。

## E15 排查与修正规约（已完成）

本轮用户要求先定位 E14 原始公式的大误差，再优化复测；E14 不覆盖。冻结以下验收：

1. 核对 GGUF 参数、2MNK、GQA causal pairs、FP16 KV 和实际 decode/logits 路径；从固定版本 Qwen3 图确认最后一层 `get_rows` 后 FFN 只处理输出位置。新增可选 `last_layer_ffn_tokens`（0保留旧全token口径，当前 native 显式设1）；只纠正算量，不改变推理。
2. 对旧三类 primitive 做单图深度1与连续多算子深度16对照，均同步、独立buffer、输出校验、预热与重复；用差分斜率区分每图固定开销与每算子服务时间。差分非正或深度1/16的观测范围相交则拒绝生成有效速率；范围分离只证明斜率可辨识，不是统计稳定性保证。单算子 wall-rate 保留对照，不能直接标为硬件算力／带宽。
3. attention 新参数改用实际 F16 KV、F32 Q、GQA16/8、dk=dv128、causal mask 的 `ggml_flash_attn_ext` 连续依赖图；不再以普通F16 GEMM替代。使用固定的prefix2048/new512微测点，与后续评价分开。以逻辑causal FLOPs归一化的有效速率不是物理峰值。
4. E15仍用63条标定／84条评价网格；比较旧raw（本轮重新微测）、仅FFN算量修正、仅连续图速率、完整修正、完整修正＋原三系数校准，以及原frozen/online。相同实测任务、冻结策略先于评价，校准只用标定。报告compute/memory分量、速率与计时差分，明确未测真实DRAM流量及纯GPU执行时间。
5. 独立小矩阵手算／无效输入原子性回归、ASan/UBSan、默认回归先执行；新数据保留21形状greedy/token/batch oracle。若连续图仍有形状／缓存误差，如实保留，不按评价数据挑速率或重跑取最好结果。

本轮只扩展可选估算器和测量工具；校准窗口、上下文所有权和默认调度保持原契约。结构体新增字段须重编译调用者；旧C源码的零初始化继续保留旧口径，不承诺二进制ABI兼容。

E15 开发探测补充：首次探测误设 `n_kv_max=span`，选中 sparse vec kernel；固定版本 dense llama 路径实际传0，且 Q/K/V 为 head-interleaved stride。本次试跑只作排查，正式微测对齐传0与stride，并记录上游选择依据。连续图后追加完全相同的旧三项探针，单列 warm-order 对照；缺少频率遥测时不将顺序差异指认为降频。正式 E15 增加仅bandwidth、仅linear、仅attention速率三项消融，其他方案不变。

## E15 定位结果与当前实现

[E15证据](../evidence/roofline-diagnosis-20261030.json) 与 [统一报告](../benchmarks/README.zh-CN.md#e15--原公式偏差排查与修正) 为本轮结果。278条记录：147个标定／评价任务、42条初始／后续孤立探针、56条连续图探针、4个斜率、1份连续图速率、1份后续孤立速率及原有元数据／开销／正确性记录。每次实测有11种预测／消融，不将它们算作11次独立推理。

**结论：主要是算力参数的测量口径与算子不匹配，带宽不是本轮大偏差主因；另有真实的最后FFN算量遗漏。** 独立复算147条E14记录与其FLOPs、字节、Roofline时间吻合；复算确认的是旧定义的算术一致性，不意味着其访存代理或总算量等于真实指令／DRAM流量。`2MNK` 未发现算术或单位错误；Qwen3最后一层在FFN前按输出位置 `get_rows`，当前只要每chunk一个logits，因此新增 `last_layer_ffn_tokens=1`，零值继续旧行为。小矩阵一／两层手算及分批回归通过；该数值字段不改变推理执行。

| E15速率／诊断 | 初始孤立图 | 连续图后重复同一旧探针 | 连续依赖图差分 |
| --- | --- | --- | --- |
| Q8 GEMM TFLOP/s | 1.758 | 6.628 | 10.305 |
| F16 GEMM TFLOP/s | 0.685 | 2.011 | 3.217 |
| 64MiB copy读写 GB/s | 62.427 | 64.311 | 70.537 |
| dense FlashAttention TFLOP/s | 未用作旧参数 | 不混同F16 GEMM | 2.765 |

单位十进制。Q8深度1／16的中位时间0.514／5.203ms，差分给每算子0.3126ms及图截距0.2014ms；把孤立图同步墙钟直接折成FLOP/s会混入这部分固定成本。初始旧Q8探针耗时约1.832ms，后续重复约0.486ms，顺序／预热状态也明显影响测量，不能全归因于固定图成本。没有频率／温度／GPU时间遥测，不能断言某种降频原因。普通F16 GEMM与FlashAttention也不是同一工作；连续F16速率仍不能直接替代FA。

当前新速率取深度16减深度1的中位时间差，Q8用交替1024↔3072的依赖矩阵链，FA用prefix2048/new512、GQA16/8、dk=dv128、FP16 KV、F32 Q／累积、与上游一致的stride和dense提示0。每级独立权重／KV，copy源为伪随机；其他算子用可验证的合成载荷。深度范围必须分离，否则拒绝速率。探测与标定／评价分离，两个深度仍不能证明全形状线性或稳定性。初次开发探针的sparse提示错误及第二次纯探针验收都保留在运行清单，未混入E15；正式运行没有根据评价调参／重跑选成绩。

| E15同84个任务的预测／单项修正 | MAE ms | WAPE |
| --- | --- | --- |
| legacy（重新测旧式参数） | 240.283 | 270.53% |
| 仅FFN算量 | 237.527 | 267.42% |
| 仅带宽 | 240.035 | 270.25% |
| 仅linear速率 | 129.148 | 145.40% |
| 仅attention速率 | 103.888 | 116.96% |
| 全部速率、保留旧算量 | 13.845 | 15.59% |
| 全部速率＋FFN算量 | 14.220 | 16.01% |
| 完整修正＋三系数校准 | 9.133 | 10.28% |
| 原frozen | 10.117 | 11.39% |
| 原online（先预测后学习） | 6.434 | 7.24% |

固定token/s基线MAE54.064ms。单项差不能相加；聚合有max等非线性。完整修正把原始预测MAE降低约94.1%，不是实际推理提速。正确FFN计数反而把速率修正版MAE从13.845增加到14.220，表明旧近似间有补偿，不删除该负结果。校准对原frozen约9.7%的单轮MAE改善不能声称稳定优势；原online仍较好。E15不同于E14的基线波动，不能跨轮累计。

新校准系数0.963581364、1.267476874、5.260797108；仍只用63条标定。新raw／calibrated predict／observe的十组中位开销分别0.0187／0.0753／1.9475µs，最后一项在评价完成后的校准副本上计时；每份状态3120B、无上下文副本。C单测、ASan/UBSan及默认回归先于GPU性能运行通过，21形状从头KV greedy、规范token和精确token/batch计数通过。

当前残差边界：修正版raw最大误差54.856ms，校准后54.200ms，仍不可保证刚好就绪。只用了一个速率形状；小batch的vec路径、padding、真实缓存／流量、遗漏norm/RoPE/激活、CPU调度及争用未分解。后续可细分形状与kernel速率、记录预测区间或稳健拟合，再以新证据验收。E13/E14保持当时源码哈希，当前实测源码看E15；自动预热和调度仍待实施。

## E16 batch／context拆分与形状模型（已完成，旧插值为历史）

本轮先纠正比较口径：E15冻结Roofline与原online训练信息不同；新实验同时测经验与Roofline的冻结／先预测后observe版本。E15数据开发回放得到Roofline online约5.641ms MAE，只作历史分析，不替代新实测。

研究依据（仅借鉴思路，不直接移植实现或外推其准确率）：[Vidur](https://www.microsoft.com/en-us/research/wp-content/uploads/2024/05/vidur_mlsys24.pdf)分别建模token相关算子与同时依赖query/context的attention，并以稀疏实测预测未测形状；[FlashAttention-2](https://arxiv.org/abs/2307.08691)解释工作划分、并行度与非矩阵操作影响效率；[IBM Roofline＋回归](https://research.ibm.com/publications/predicting-llm-inference-latency-a-roofline-driven-ml-method)将结构模型与运行时开销校准结合。均不能证明本机Metal的具体误差原因，后者仍以固定上游分支与本机探测核实。

冻结本轮实现／验收：

- 分别使用batch上限128和512，保留前缀0/2048/6144、新token32/128/512、7次标定；评价前缀0/1024/4096、新token8/16/19/20/64/256/768、7次。每配置63条标定／147条评价，各自独立profile／校准；评价形状不用于拟合冻结模型。
- 报告逐prefix×new×batch上限、内部分chunk宽度／次数、vec(<20)/dense路由及误差方向／组内波动；单序列并发数仍1，batch指decode token宽度。19/20针对固定Metal FA分支，8针对线性小batch分支；只按源码声明路由，不冒充实际GPU计数器。
- 新增可选数值形状表：width节点8/16/19/20/128/512，context节点0/2048/6144；Q8依赖链测有效linear速率，实际dense提示FA测每层时间，copy带宽保持独立。均depth1/16、2暖＋7次、独立KV/权重、正确性检查及可辨识斜率。FA KV跨度按上游padding256对齐，mask未来位置-inf；Q/K/V stride与上游一致。全部微测先于模型标定／评价。
- 核心形状表只存数值，无heap／上下文；同kernel分区（<=8、9..19、>=20）内对linear倒速率与FA时间插值，context线性插值；跨边界不混插，越界取最近点并标extrapolated。逐chunk推进context并调用已有结构算量；未覆盖小宽度明确标外推。
- 同任务比较原经验frozen/online、E15式单形状Roofline raw/frozen/online、形状表raw/frozen/online，八种。两种在线公平使用相同数值窗口64／先预测后更新。形状表额外微测成本、内存和开销单列，不以更多探针的收益冒充免费改进。
- 独立手算插值、kernel边界、分chunk、无效表／失败原子性单测；ASan/UBSan与默认回归先于模型计时。60种形状从头KV greedy/token/batch对照。残差、尖峰和新方法变差均保留；正式评价后不调参数或换运行。

本轮仅增加可选估算与宿主测量接线，原API／推理路径不变。纯GPU时间、温度、频率、DRAM流量依旧未测，不能用模型残差指认具体硬件争用。

### E16开发结论与E17冻结验证（已完成）

E16的916条记录与60形状oracle已完成，后处理遗漏`re`导入在native成功后报错；已从原manifest／raw恢复元数据，没有重跑推理替换样本。原测量runner哈希与修复后processor哈希分开保存。E16八方案完整保留：batch512原online／单形状Roofline online／形状online的MAE为5.127／4.748／6.589ms；batch128为3.893／4.094／4.508ms。形状raw显著改善，但校准／online没有一致优势。

已定位形状表64-token回退的一处建模错误：对`1/P(n)`线性插值后再乘`F(n)∝n`，会在端点间引入额外的二次项；应先插值该固定矩阵族的算子时间，即`n_i/P_i`，再换算当前宽度速率`P(n)=n/interpolate(n_i/P_i)`。这不保证真实性能线性，但避免插值自身制造曲率。FA表已经插值时间，不受此错误影响。新代码修正现有可选shape接口的插值语义；E16为历史实现，不保持未发布实验API的二进制ABI承诺。

E17在看过E16后实施这一项修正，不重调节点／系数／窗口。先改手算单测，再独立使用相同标定及微测节点，新评价prefix512/3072/5120、new12/18/24/48/192/384/640、batch上限512/128、各7次，评价形状与E16不同。八方案仍同任务／相同在线信息。报告E16回退、E17新形状结果和外推标记，禁止把跨运行差异写成同任务提速。默认回归／sanitizer先于E17，60形状oracle继续核对。

## E17 当前形状估算契约与结果

当前实现为 `include/cxf/roofline.h` / `src/roofline.c` 的 `cxf_roofline_shape`、`shape_estimate/predict/observe`，测试／实测入口 `tests/test_roofline.c` / `tools/bench_cost_shapes.py`。上面E16规约保留原始插值背景，linear倒速率插值已由本节的时间插值取代；E16证据与原源码哈希不修改。新实测 [E17](../evidence/cost-shapes-v2-20261030.json)，完整分组见 [报告](../benchmarks/README.zh-CN.md#e1617--batch上下文拆分与公平在线对照) 和逐格CSV。

### 输入、边界与所有权

- 固定表最多12个width和8个context节点，本轮使用6×3；表占960B，存linear有效速率和每层FA时间，不存第二套上下文。表借用至同步调用结束；一份校准3120B，总4080B为表＋一份数值模型，不是整个benchmark进程内存。调用者串行更新模型，修改模型／backend／配置／表数值或插值算法必须换profile。
- 归一化linear服务时间为`width_i/rate_i`，按width线性插值，再令`rate(n)=n/interpolated_time`。FA直接插值时间；在<=8、9..19、>=20三个固定Metal区间内插，不跨kernel边界混插。表未覆盖的区间／端点用最近时间节点并标extrapolated。context按相邻节点插值／端点裁定；逐chunk推进prefix，分别计算原结构量并合计。分区是本profile选择，不是任意GPU通用常量。
- 非法／未排序节点、零／NaN速率或时间、不完整表、work溢出／超过4096 chunks返回RANGE，输出和模型不变；未标定预测返回EMPTY；profile不符拒绝。`shape_predict`合并表外推和观测范围标记，均不代表置信区间。零work仍验证表／spec，合法输入返回零。
- 实测helper对齐上游KV最少256、按256对齐的跨度与mask未来-inf、head-interleaved stride、dense提示0；分层时间不是纯GPU时间。额外探针只提供固定profile的先验，不自动接预热／调度。

### 定位了哪些误差

1. **训练信息不同**：E15先前9.133ms冻结Roofline与6.434ms经验online不能直接判断结构特征孰优；等信息的历史回放Roofline online为5.641ms。E16／E17给全部online相同先前标签，CPU独立回放复核先预测后更新与64窗口。
2. **kernel边界是真实的**：E16在prefix4096、cap512，19→20token实际中位39.726→27.479ms；cap128为39.500→27.133ms。独立单层FA在prefix2048为0.4678→0.1978ms，固定源码query<20选择vec路径。少一个token反而慢，平滑FLOPs/pairs公式无法拟合这个台阶。源码路由不是GPU计数器，但独立探针和完整任务互相支持。缓存跨度padding也会使简单连续工作量不等于执行形状。
3. **初次插值制造了额外曲率**：E16 width64的linear代理约29.951ms，即使prefix0完整任务中位仅18.617ms；先插值倒速率再乘n是错误的时间模型。E17只改成上述服务时间插值，节点／窗口不调，使用未参与E16评价的新形状验收。不能把节点间实际kernel时间都假定线性，仍需看新样本。
4. **chunk次数和上下文必须分开**：E16同768新token、prefix0，cap512两次decode中位129.811ms，cap128六次为147.615ms；prefix4096为419.848／438.958ms。配置顺序512后128，不能把所有跨配置差异只归因于chunk开销；单次kernel宽度和顺序状态也不同。

### 新数据与负结果

两轮各916条，合计新增1832条原始记录；每轮420个完整工作测量（126标定／294评价）、336条形状深度探针、24条shape rate、原base探针和元数据／开销／oracle。每个配置63标定＋147评价，8种预测共享同一任务，不能把预测个数乘作推理样本。每轮60形状对照通过，单测／ASan/UBSan／默认回归均在GPU计时前完成。

| E17（147条／配置） | cap512 MAE ms | cap128 MAE ms |
| --- | --- | --- |
| 经验frozen | 6.148 | 4.369 |
| 经验online | 3.753 | 3.335 |
| 单形状Roofline raw | 13.989 | 17.877 |
| 单形状Roofline frozen | 4.320 | 3.932 |
| 单形状Roofline online | 3.059 | 2.892 |
| 时间插值shape raw | 4.165 | 3.374 |
| 时间插值shape frozen | 5.620 | 4.961 |
| 时间插值shape online | 2.793 | 1.926 |

形状online相对经验online的同任务MAE降低约25.6%／42.2%；相对单形状Roofline online亦改善。形状online最大绝对误差8.869／8.484ms，signed bias为+2.105／+1.736ms，仍非精确时钟。E17 raw也改善，但冻结形状校准在两个cap都比raw及单形状冻结差，cap128还差于经验frozen。不能只保留online胜出的表格；训练集只含dense形状，三个全局系数的跨kernel／新形状迁移与运行漂移仍未充分建模。

E16形状online MAE6.589／4.508ms，差于经验online5.127／3.893及单形状online4.748／4.094；64-token回退保留。E17换了评价网格，只能在本轮同任务比较，不把E16→E17差异全归功于代码。两轮固定微测节点，但首次标定及工作顺序状态并未隔离，不能指认频率／温度／争用。

E17形状表构建4797.034ms，包含分配、初始化、预热及测量，前置成本单列；不在每个预测调用中执行。cap512/cap128的predict中位0.1131／0.1044µs，observe/refit均1.9200µs。bench同时保留多种基线的内存不属于单模型4080B。模型加载、prefix建立、形状预热、fresh-KV oracle均在服务时间之外。

当前证据范围仍是单机M5／Metal、Qwen3-0.6B-Q8_0、b10938。本轮只改善估算，没有修改推理kernel、KV所有权或默认调度。真实DRAM流量、纯GPU时间、频率／温度未测；残差中硬件波动与未建模算子不能从wall时间唯一分离。后续若要分离，需要独立遥测与更广配置／模型复测，不能凭误差给硬件原因定论。

## E18 冻结回退排查（原计划，首次运行未完成）

本轮先以E17作开发证据，比较原128轮坐标下降与精确3变量NNLS、各形状标定残差／损失占比；检查是否优化未收敛。再比较保持原basis的加权拟合、保留raw先验的残差拟合，区分样本覆盖与模型／目标函数。本轮将E17明确作为开发数据，不当作新验证。

新实测计划：batch上限256／64，均未在先前实验验收；评价prefix1280/3328/5376，新token10/15/21/72/176/352/672、各7次，完整形状与E16／E17不同。两套等量63条标定：原dense网格0/2048/6144×32/128/512；多样网格9个点覆盖small/vec/dense与上下文，固定为(0,8/96/448)、(2048,17/96/448)、(6144,8/17/448)，均7次。两套数据在每个prefix中交错、轮转采样，不让一种标定方案总是先跑。锚点(0,32)、(2048,17)、(6144,512)在标定前／后及评价后各重复3次，不纳入拟合，以观察阶段漂移。

正式测量前冻结最终拟合方案及超参数；相同评价任务同时给出raw、原NNLS sparse/diverse、新方法sparse/diverse、原在线与形状在线。冻结方案不使用评价标签。报告训练误差、等覆盖对比、模型变化对比、anchor阶段差异及多chunk／外推；源模型与调度不变。若覆盖改善但模型仍回退或新方法失败，照常保留。单测／sanitizer／默认回归在GPU前，真实形状及token/batch核对继续。不能凭wall波动判定GPU频率或争用。

### E18最终策略（正式评价前冻结）

开发回放确认原NNLS与精确解系数最大差约6.5e-9，未收敛不是主因。固定采用3变量的raw残差模型，不引入更多参数：`prediction=max(0, raw_ms + beta0*calls + beta1*new_tokens/512 + beta2*raw_attention_ms/100)`。目标为`actual-raw`，权重`1/max(actual_ms,20)`，按加权RMS缩放各特征，ridge=0.01、先验beta=0；signed系数允许纠正raw偏高，最终时间截0。保留原NNLS API；新类型固定64数值窗口、无heap／上下文。相同basis的加权NNLS作为训练标签限定的离线对照，以分离改变权重和改变模型的影响；不使用评价标签拟合。

正式评价同时比较raw、legacy sparse/diverse、weighted-NNLS sparse/diverse、residual sparse/diverse、经验online sparse与shape online sparse。每套标定都63条；在线版本只从sparse初始化，之后先预测后观察当前评价，冻结方法不观察。新方案超参数不随E18评价调整。两种coverage交错采样，anchor独立排除；比较coverage和模型各自的改善／回退。

输入是`work/raw_ms/raw_attention_ms`数值，profile包含模型／引擎／形状表与新算法身份。非法work、NaN／负raw／attention大于raw、超界ms拒绝且状态／输出不变；零work返回0，未标定非零预测EMPTY，profile隔离。预测不改状态，调用者串行；新observed样本仅成功完整工作，失败／取消不拟合。主机接口仍未自动调度。

### E18表覆盖消融（评价前补充冻结）

另一个已确认输入缺口：旧表context最大6144，但标定prefix6144的多chunk会推进到6592等位置，原API按端点裁定并标外推。为避免冻结系数补偿此低估，正式E18微测新增context7168（仍同6个width），覆盖所有标定chunk；保留context_count=3的旧端点表及`legacy_clipped_sparse`控制。新评价所有chunk起点<=6144，所以扩展表／旧端点表的评价raw完全相同，仅训练features不同。这样可隔离表覆盖，而不是把它混作更多训练标签。其他coverage／残差策略不变，未改变评价网格。

E18首次native运行完成cap256后，在打开cap64时因默认`prefill_min_tokens=128 > batch64`返回RANGE，尚未产生cap64样本。完整849条记录单独归档E18 incomplete，不纳入正式准确率结论。测试入口显式设置`min(128,batch)`；flush=true的服务路径不变。采用新编号E19、独立新目录完成同一冻结策略／新网格验证，不按准确率挑运行。算法与超参数不调整，E18负结果及源码哈希保留。

## E19 当前残差校准契约与验证

E18未完成记录 [单列归档](../evidence/frozen-calibration-incomplete-20261030.json)，E19为修复测试设置后的 [正式验证](../evidence/frozen-calibration-20261030.json)。原NNLS／shape API保留，新增可选 `include/cxf/cost_residual.h` / `src/cost_residual.c`；不自动替换原估算器或接管Agent调度。完整对照见 [报告](../benchmarks/README.zh-CN.md#e1819--冻结校准回退排查) 及其中训练、评价、anchor三个CSV。

### 残差模型、状态与失败行为

```text
x = [ceil(new/batch), new/512, raw_attention_ms/100]
r = actual_ms - raw_ms
weight = 1/max(actual_ms,20)
scale_j = sqrt(sum(weight*x_j^2)/sum(weight)), 零列取1
min_beta sum(weight*(r - (x/scale)*beta)^2) + .01*sum(weight)*||beta||^2
prediction = max(0, raw_ms + x * (beta/scale))
```

这是带raw先验的3参数校准，不是对三个物理部件逐项计时，也不能把beta解释为norm／RoPE的真实耗时。使用标准 [ridge正则化结构](https://scikit-learn.org/stable/modules/generated/sklearn.linear_model.Ridge.html)，独立C的3×3 Cholesky直接求解；没有新增Python／sklearn运行依赖。固定64条数值观测，状态3632B（含权重及work），表960B＋一份残差模型合计4592B；宿主负责选择profile、串行调用及表／参数生命周期，不存原文／token／KV，不启动线程或动态分配。

`init/predict/observe`借用本次数值输入；predict只读。仅成功且完整的工作observe，实际ms非负有限且<=1e12；raw同范围、attention<=raw，work合法且无prefix+new溢出。非法值、NaN／Inf、负值或profile错拒绝且状态／输出不变。observe用临时数值副本拟合成功后提交；零work合法时预测0，不纳入训练；未标定非零预测EMPTY。signed系数允许负修正，最终截0；fit_mae只描述数值窗口的残差，不是概率区间。observations外推标记仍按逐维范围／batch检查，不能保证新组合准确。

### 判定回退原因的证据

1. **不是128次迭代未收敛**：E17开发审计与精确active-set NNLS的系数差最大6.50e-9，两者训练SSE一致。原形状缩放模型确实在它的约束／目标下找到了该解。
2. **loss／basis有影响**：E17拟合前raw残差平方量92.4%／94.6%来自512-token任务；整体放大linear容易高估小任务。保持同basis的加权NNLS开发回放有改善，但E19权重改变没有一致收益，不能把问题只归因于平方损失。raw先验＋token数的残差basis在cap256有额外收益。
3. **覆盖有两种**：原表上限6144小于多chunk标定实际起点6592等；新表增加7168，所有标定chunk入表。同时以等量63条diverse标定替代63条dense标定，交错测量。评价raw两表完全相同，因此旧端点／扩表对照只改变训练features；coverage方案只改变所测工作形状，参数数目不变。
4. **阶段漂移不能忽略**：cap256长anchor(6144,512)的中位从标定前363.472ms升到标定后377.057ms，短anchor(2048,17)为27.253／27.331ms；cap64长anchor446.056／444.944ms变化较小。每阶段3次、未进任何拟合。这说明profile内仍有随工作量变化的阶段波动，不能仅从wall判断频率／温度／争用。

### 正式新形状验证与负结果

E19使用cap256／64，prefix1280/3328/5376，新token10/15/21/72/176/352/672，每格7次、每cap147评价。sparse为旧0/2048/6144×32/128/512，diverse为(0,8/96/448)、(2048,17/96/448)、(6144,8/17/448)，各63标签。kernel／表、raw先验、ridge与权重参数都先于评价固定。10种预测共享同294个评价任务，weighted NNLS为训练限定的离线对照，294条结果以raw索引引用，未增加native样本。

| E19 MAE ms | cap256 | cap64 |
| --- | --- | --- |
| raw | 7.439 | 2.183 |
| legacy clipped／sparse | 4.399 | 4.728 |
| legacy extended／sparse | 5.331 | 2.678 |
| legacy extended／diverse | 4.486 | 2.631 |
| weighted sparse | 5.256 | 2.754 |
| weighted diverse | 4.519 | 2.749 |
| residual sparse | 4.809 | 2.664 |
| residual diverse | 3.452 | 2.645 |
| empirical online（sparse初始化） | 3.529 | 3.321 |
| shape online（sparse初始化） | 3.930 | 1.683 |

cap256：等量扩覆盖使legacy改善约15.8%；在diverse上换residual再改善约23.1%。但cap64的扩覆盖收益很小，而且全部冻结校准均差于raw。扩表在cap64把4.728降至2.678，却在cap256使4.399升至5.331，旧近似的误差补偿仍存在。因果效果不能相加，不能选一个cap宣传通用优势。

训练拟合变好也不保证泛化：cap64/sparse的raw训练MAE1.792ms，residual为1.045ms；新评价却是2.183对2.664ms。结果不支持“只有样本覆盖不足”，也不支持“换成带正则的三个全局系数就能总改善”。后续若研究自动选择raw／校准或分层模型，应只用标定内的独立留形状验证选择，再用新网格验收；本轮未实现自动选择，不根据E19评价改参数。

E19保留1162条native记录（252标定／294评价／54anchor及探针／状态等）、294条单独引用的离线weighted对照；E18的849条未完成记录完整保留且不入正式表。E18在cap256后因prefill_min128>batch64被拒绝，E19测试入口明确min(128,batch)，flush=true服务路径及算法不变。没有按精度挑运行；E19一次完整通过，78个独立fresh-KV工作形状、规范ID、greedy及实际token/batch核对通过。新模块手算共线／零列、signed修正、64窗口／漂移／profile／失败原子性、ASan/UBSan及默认回归先于GPU通过。

扩表构建8695.356ms（该轮分配／初始化／预热／微测总成本），额外成本单列，不在每次预测执行。cap256／64的组合shape lookup＋residual predict中位0.1629／0.1301µs，residual observe/refit为0.4525／0.2750µs；bench同时持有多个对照模型，不能把4592B说成整个进程内存。当前源码／验证哈希以E19为准，旧阶段保持原测量版本。仍未改变模型推理或默认调度，仍未验收Windows/CUDA、纯GPU时间／DRAM流量／频率／温度。

## E20 残差Kalman（2026-10-31，原冻结方案与验收）

用户提出叠加Kalman。本轮做可选、常数状态的数值残差过滤，不接默认调度。状态`bias_next=bias+w`，观测`actual-base=bias+v`；Q按一次成功观测计，不等同真实秒级漂移率。Gaussian是模型假设，不是给原始误差套正态就能提升；MAE与Kalman的平方误差目标也不同。

公式与边界参考 [MathWorks估计滤波文档](https://www.mathworks.com/help/fusion/ug/introduction-to-estimation-filters.html)；[NASA非Gaussian观测研究](https://ntrs.nasa.gov/citations/20230006018)区分最佳线性估计和一般最优估计。固定Q/R稳态scalar Kalman等价EWMA；本轮明确将两者对照，不把等价算法冒充额外收益。

冻结方案：

- 新纯C bias/P/Q状态，profile隔离，predict只读；成功完整工作才observe并推进一次Q，重复predict不增长状态。观测必须使用**未叠加过滤的base预测**。R每次由调用者给定；P更新用scalar Joseph形式。非法值、溢出、profile错拒绝且状态／输出不变。零token返回0、不更新；无heap／原文／token／KV。新profile/取消未完成工作不继承样本；关闭为丢弃数值状态。
- EWMA gain由Q/R稳态公式决定；fixed Kalman从稳态P初始化，验证它与EWMA等价。scaled Kalman用同Q/P初值，只令R随预测任务量变化。
- 噪声仅从本轮diverse标定9形状×7的组内variance估计：常数R=max(.25ms²,median组内方差)；非负拟合R(base)=max(.25,c0+c1*(base/100)²)。Q=.01*常数R，稳态gain=(sqrt(.01²+4*.01)-.01)/2，初始P=gain*常数R，bias0。所有参数先于评价固定；滤波仅从评价开始学习，各层相同过去标签，不用当前值预测当前值。
- 新测量沿用E19标定／扩表和cap256/64，但评价prefix1792/3840/5504、new11/18/26/88/208/400/688。原native base仍raw、residual diverse frozen、shape online；之后以C过滤层严格顺序回放，每个base各EWMA/fixed/scaled三层。层回放不算额外模型调用，source索引与native记录分列；不改变推理计时。
- 验收报告MAE/RMSE/max/方向偏差、早期冷状态、各形状、标准化innovation的偏度／峰度／lag1及±1.96sigma实测覆盖率。区间覆盖只是诊断，不称95%保证；序列相关时不把简单正态检验p值当独立证据。尖峰、切换后回退与新层变差保留，不按E20标签调Q/R。
- 手算gain/Joseph、常量gain等价、零Q/列、step与单尖峰传播、profile／失败原子性测试，ASan/UBSan及默认回归先于性能；新实测保持78形状oracle。新日期追加E20，不覆盖旧20份证据；发布脚本同时支持2026-10-31快照与旧日期。

### E20 当前接口与验收结果（已完成）

实现 `include/cxf/cost_kalman.h` / `src/cost_kalman.c`，`init(profile,Q,P0)`、只读`predict(profile,work,unfiltered_base,R)`、`observe(profile,work,unfiltered_base,R,actual)`。状态40B，predict返回非负ms、bias、观测预测方差S=Pposterior+Q+R、gain、先前样本数。观测后P用Joseph公式(1−K)²Pprior+K²R，非法输入不发布部分状态。每个profile/base独立持有；取消／失败任务不observe。Q按观测步数而非真实时间推进。该层只在宿主显式调用时生效，未接默认Agent调度。

E20一次新目录测量完成1162条native、78种fresh-KV token/greedy/batch oracle。三base×两个cap各147标签形成882条C数值回放预测，另有元数据／10层CPU开销记录；引用同294评价任务，不重复计作模型样本。C严格先预测后更新；Python独立复算标定noise、EWMA、gain/Joseph、prediction及summary。新证据 [E20](../evidence/kalman-cost-20261031.json)、[报告](../benchmarks/README.zh-CN.md#e20--可选残差-kalman)、[逐格误差](../benchmarks/data/kalman-errors.csv)。

| E20 MAE ms，n147／格 | 原base | EWMA＝fixed Kalman | scaled R Kalman |
| --- | --- | --- | --- |
| cap256 raw | 5.056 | 4.813 | 4.588 |
| cap256 residual/diverse | 2.913 | 2.790 | 2.834 |
| cap256 shape-online | 3.173 | 3.073 | 3.147 |
| cap64 raw | 4.748 | 3.290 | 3.944 |
| cap64 residual/diverse | 4.687 | 3.440 | 4.024 |
| cap64 shape-online | 1.975 | 1.922 | 1.951 |

当前主力shape-online上的改善约3.2%／2.7%，只支持小幅修正。fixed／EWMA在六组中的五组优于scaled；scaled在cap256/shape-online的最大误差9.582→9.778ms略恶化，fixed每cap的21形状中也有10格平均误差变差。Q/R没有用评价重调。同形状连续七次的测量次序使在线层能学习先前重复标签，不能外推为真实异构Agent序列收益或独立分布泛化保证。

噪声估计暴露失配：两组constant R都为floor.25ms²，scaled c0/c1在256为[1.9193,0]（退化为另一常数R），在64为[0,5.5676]。名义±1.96sqrt(S)覆盖只有15.65–70.07%；标准化innovation SD为2.64–11.85而非1。组内抖动估计没有覆盖跨形状的结构残差，scalar漂移状态也表达不了所有形状误差。这说明协方差被低估／模型失配；覆盖率本身不能证明“非正态分布”。当前P/R不能当95%就绪保证。若进一步改Q/R或分层bias，必须先用标定内独立验证选择，再换新序列验收，不回头用E20改成“冻结”成绩。

状态40B；层predict／observe中位0.00919／0.01360µs，编译器／平台／短循环开销限制单列，非推理加速。手算Joseph/gain、零Q、只读／EWMA等价、step、单尖峰传播、profile／边界／失败原子性、ASan/UBSan及默认完整回归在GPU前通过。新层保持可选，旧20份证据保持不变，默认调度与Windows/CUDA未扩展。当前实测源码哈希看E20；E19仍为对应历史阶段。
