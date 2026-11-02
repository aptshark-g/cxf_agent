# Measurement and reproduction

## E62/E63：云端请求对照与时间分层

公开记录仅保留模型标识`deepseek-v4-flash-0731`和可复核测量。实际服务地址／IP简称／key／账号／个人配置路径不进入文档、数据或发布包；逐轮与任务CSV补充model列，不改原始数值和历史证据。

当前真实云采集是非流式，端点由部署者指定的私有配置在内存中读取，经可信回环relay到明确配置的HTTP上游，不承诺TLS或后台模型身份。2只读任务各1预热＋4评价对，逐block倒序，共20任务50请求；同一不可变文件让版本回执一致，两分支均用相同application/json; charset=utf-8头，真实模型独立选择工具。所有输出长度／缓存／网络漂移保留；n=4不是置信区间或尾延迟保证。

每轮prompt/completion/total与cached_tokens直接来自usage；专用hit/miss缺失为null，bool／负数／字符串为非法，字段矛盾只标记、不修造数值。缓存比例按评价轮次sum(cached_tokens)/sum(prompt_tokens)，不推算缺失miss或计费。无远端清缓存能力，不能强制冷／热。首轮输入少18token是提示清理实测，UTF-8同对象回放不是token节省证据。

任务墙钟截止子进程退出，剔除relay.shutdown轮询。C provider跨度覆盖请求构造、HTTP、解析；relay跨度覆盖API，二者不可相加。API到头／读体是网络＋队列＋推理合计，不是TTFT。relay本地=完整relay−API；C残差=provider−relay（含本机传输和C协议，允许小负测量残差）。工具外层前后策略与内部exec分开，后者仍含文件策略和I/O；新trace自身开销在两个分支一致，未声称它零成本。工具事件附到刚完成的模型轮次，不把交错时间间隔全部冒充工具时间。

E63使用perf_counter_ns墙钟；未单独测线程CPU。归档限制中“CPU may increase”仅为可能性，不是已测CPU结论。E63的60条行均为1000次序列化的每次均值，每格1预热＋9评价，分别旧／新提示×ASCII空白／ASCII紧凑／UTF-8紧凑；同一解析对象，首轮2513→1840B。JSON正文字节不含头／TCP／TLS，序列化us来自perf_counter_ns墙钟而非线程CPU，不能与云ms混淆，也不等于模型token／KV／RSS。保留序列化墙钟略增、4/8任务回退与异常API等待；历史工具编辑和推理成绩不随本轮源码自动更新。

E59 363条记录为每进程8次操作均值（read64次），2预热＋9评价block、baseline/current顺序轮换，baseline basic来自公开ad5ac5a；重置、已知版本和输入准备／bytes oracle不计入handler，workflow计入真实读取和patch组装。大文件whole只调用C handler，CLI仍限1KiB参数。E61 153条包含150定时和3回执字节诊断，1预热＋9评价；metadata/lock迭代256次、policy8次，profile0/1/16仅合成秘密。独立中位不能相加。E60三例用真实用户指定云模型，thinking disabled，经实际CLI和独立源码字节／编译校验；网络／模型耗时不作为工具开销。

E58为功能准备：24项无模型状态／安全、8项Native（6项KV／模板及2项注入完整流程）、原14K兼容回归与真实0.6B预算耗尽冒烟分开记录。没有新正式性能样本。异机runner按case/block轮换off/on，不丢失败或负对，区分全部任务与完整且输出一致配对，率值保留计数／分母，probe成本及加载时间单列。短fs.read可能没有足够重叠窗口，不能用人为sleep制造收益。见[交接](../self-speculation-handoff.md)。

E56/E57当前：270条固定目标API阶段记录＋180条新CLI主记录，均2预热／7评价block，58份证据／36,664主记录／41图；中止预热独立CSV，不混入性能。E55绑定off26.0／on10.4为已发现混杂，旧数不改；新轮次显式同target。PCH冷准备／全部TUs／链接／运行／清理全计，临时逻辑文件bytes不等于RSS／KV。最终基准父进程仅在存活期阻止空闲休眠，双时钟差异阈值100ms，超限中止整轮，实际最大0.029ms；之前约274s／0.8s差异原因未定。

E54/E55当前测量补充：E54 126条API主记录（2预热＋5评价），另70条直接系统API探针，各1000调用；目标拒绝日志／空栈采样只作诊断，不能混成精确IPC等待。E55 108条真实CLI主记录（3新任务×4模式×9block，2预热＋7评价），序列旋转／倒序，无SAN、无并行验证负载。所有任务全新私有HOME／TMPDIR，off的创建／清理也在计时CLI内，与E53已热宿主基线不同。源归档、实际同Xcode／SDK哈希、程序结果和全部配对保留；绑定是显式命令环境优化，默认权限／日志未改，无推理性能结果。

## E53最终宿主保护与重复开销（2026-11-01）

E52文件／网络／fd验收后，额外纯合成检查发现kern.procargs2仍能读取父环境，收窄Seatbelt sysctl规则也未解决。E53只标记cxf自身CS_RESTRICT、检查SIP及可信跨进程环境省略；不操作系统设置或其他程序，不声称限制其他同UID进程、可见argv或VM隔离。off正对照必须看到标记，on及显式network allow看不到；查询可能仍返回argv而非被拒绝。542项C、12组OS、原12组数据策略／8组基础SAN和Native14,357-token／16项Stream／72项输出通过，原6条编译警告保留。

用同一runner独立重测230条、44预热、93组评价配对，raw配对／算法与E52相同。API进程在预热中初始化粘性宿主保护，后续off/on共享该状态，冷预热记录保留；CLI每个任务都是新进程，包含初始化成本。当前CLI中位多15.724ms（空工具，5/7更慢）、3.565ms（显式CLT，4/7）、2467.548ms（标准cc，7/7，最差2534.841ms）。负对和范围完整保留，不将E52→E53差值当作修复带来的加速；各阶段中位也不可相加。

原73,880B任务结构仍共用；有效性探针另最多256KiB短期堆／一次可信fork，初始化读回poll最多1000ms，不含fork／调度／回收硬保证。CLI在模型前初始化；直接API晚于大型引擎加载的冷开销未独立测量。计时后仅改启动器帮助文字，当前运行时／Native／计时源码与实测一致，差异哈希另记。[E53](../evidence/test-sandbox-host-20261101.json)与E52及旧数据不覆盖，当前54份证据／35,980主记录／39组图。新图独立由tools/render_sandbox_final.py产生，基底renderer和全部旧38组图保持原字节。


## E52 OS隔离与开销（2026-11-01）

功能以合成文件和真实OS调用验收：542项C检查、11组OS安全、原12组策略和8组基础套件在SAN下通过；Native14,357-token／16项Stream／72项输出通过。保留localhost规则放行同机非回环接口、单条超长regex解析失败、系统启动缺根目录读取及Xcode诊断导致Stream不确定的负结果；最终仅deny/allow，确定性Stream显式CLT而不改变生产命令。OS限制契约为[D-EXEC](../design/test-sandbox-v1.md)。

独占、未插桩的[runner](../../tools/bench_test_sandbox.py)产生230条记录：API四负载×8/256个额外文件×(2预热+9评价)×off/on=176；完整CLI三负载×(2预热+7评价)×off/on=54。44条预热不进评价，93组配对全部保留；off/on顺序按rep/case交替。文件数组先8后256，未随机化，不单凭跨组差异推导扫描规模因果。实际扫描entries、profile实填字节另记；显式写同根会重复扫描。

C probe在实际子shell命令前后取monotonic，外层C API从进入到返回计时，CLI另包含进程启动、两次本地HTTP脚本选择、真实工具回执与收敛。命令外差值包含规则应用、spawn/exec、轮询与清理，不是纯Seatbelt编译。先逐对计算开启−关闭，再求中位／最小／最大；各阶段中位不可相加。标准cc CLI中位多2591.134ms、最差5524.787ms，命令内部逐对中位多2561.604ms，外层差值中位17.541ms；缓存/IPC影响尚无独立因果对照。CLT与标准cc是不同负载，保持各自off/on同命令，不包装为自动编译优化。

73,880字节是当前off/on共同的C任务栈结构容量，不是新增RSS、子进程峰值或KV。模型仅用于独立兼容验收，计时不做真实LLM推理或云端调用。实测源码私有归档及哈希在[E52](../evidence/test-sandbox-20261101.json)；随后只有Stream测试命令改为CLT，最终验收哈希另列，运行时和计时程序相同。旧52份证据与37组图不改写，新增1组图，53份证据／35,750主记录／38图。

复现：`make build/agentctl build/bench_test_sandbox build/sandbox_probe`；`python3 tools/bench_test_sandbox.py --out build/NEW-DIR`。OS套件：`python3 tests/test_agent_sandbox.py --binary PATH --probe PATH --report build/report.json`。新图单独用ReportLab4.4.9运行`tools/render_sandbox_benchmark.py`，不重画旧37组图。模型SDK／runtime／GGUF仍须显式提供，不写入发布包。


## E51 数据策略功能验收（2026-11-01）

D-DATA实施前冻结，验证仅用临时目录、合成秘密和独立回环桩。C矩阵445项检查覆盖26类默认敏感名称、全部文件工具、普通文件、非规范路径、符号链接／硬链接、动态配置inode、列表／grep跳过、编码秘密、策略表容量与范围矩阵。12组CLI安全套件覆盖默认零请求、cloud task只文本且拒绝CALL、显式workspace读—改—测、活动trace／任意配置名、工具stdout与主／辅助模型响应泄露、独立HTTP准入及JSON提权拒绝。

同样12组在ASan/UBSan CLI上通过；原8组基础套件在新策略下复验，不再把继承环境中的认证值回传模型，而用存在性标记验证继承行为。单独的泄露套件验证这些值确实被阻断。完整默认回归、真实14,357-token Native全文／fresh-greedy oracle、16项Stream与72项工具输出检查通过；固定上游库未插桩，6条已有编译警告保留。初次新增Native测试改变检查点后引用旧view，失败日志哈希单列；修正测试基准并保留原状态不变断言后全量通过。

[E51证据](../evidence/agent-data-policy-20261101.json)登记当前代码、二进制、最终测试报告和日志哈希；原始配置／引擎日志留本地。52份证据仍是35,520条历史主记录和37图；E51为0性能记录的functional数据集。未测策略扫描时延／内存增量，不把功能拒绝当安全完备、模型能力或性能收益。明确保留OS沙箱、任意未知秘密／编码、直接HTTPS、真实端云路由质量和Windows/CUDA等未验证边界。

复现：`make test`；`python3 tests/test_agent_data_policy.py --binary PATH --report build/policy-report.json`；Native按显式SDK／runtime／model路径运行`tools/build_agent_native.py --test --test-long-context --test-stream --test-tool-output --sanitize`到全新目录。全程无真实云端请求。

## E50 基础能力功能验收（2026-11-01）

本轮不跑性能试验。`make test` 包含新 `tests/test_agent_foundation.py`，以两个独立回环 HTTP 桩控制模型选择，真实执行文件读写、C 编译及进程。8组覆盖唯一编辑／拒绝、最小和继承环境、本地文本回流／注入标记无执行权限、认证分离、每任务两次预算／重置、11种响应错误／截断／超时、未挂载权限拒绝和8种启动配置拒绝。桩的文本不是模型质量结果，也不将预期拒绝计作运行时通过率或性能样本。

`tests/agent_basic.c + tests/test_agent_basic.c` 另以ASan/UBSan运行编辑及fork／可用spawn环境检查；固定b10938的sanitizer CLI再运行同样8组HTTP套件。最终默认完整回归通过。Native使用既有Qwen3-0.6B-Q8_0／Metal，运行14,357-token全文／greedy oracle、16项Stream和72项工具输出检查；预编译上游库未插桩。保留Native harness的6条已有编译警告；没有新的sanitizer诊断。

公开[E50证据](../evidence/agent-foundation-20261101.json)仅保存通过项、限制、源／二进制／日志／私有源码归档哈希。配置、请求体、端点、模型路径和原始引擎日志不公开。旧50份证据逐字节不变；新工具／CLI源码不继承旧性能成绩。51份证据仍只有35,520条历史主记录与37图，E50是0条性能记录的functional数据集，无新图。

复现：`make test`；`python3 tests/test_agent_foundation.py --binary PATH --report build/report.json` 可选指定已构建CLI。Native依赖与模型传显式路径，使用 `tools/build_agent_native.py --test --test-long-context --test-stream --test-tool-output --sanitize` 到新输出目录，然后将该目录的 `agentctl-native` 传给上述8组测试。`llm.local` 是独立HTTP文本调用，不是共享KV的云SSE或自动模型路由。

This release records experiments from 2026-10-20/30/31, including separately measured E13–E20 cost estimation and E21–E41 tool/resident scheduling. Report generation reads archived JSON and does not rerun inference or replace old trials. [catalog.json](data/catalog.json) identifies 42 evidence files and SHA-256 hashes; [run-inventory.json](data/run-inventory.json) separates local official, development and superseded runs.

## Evidence classes

| Class | Meaning | Permitted use |
| --- | --- | --- |
| raw | Original per-trial records, summary and recorded source hashes | Recalculate within-experiment comparisons, with stated limitations |
| raw_source_unrecorded | E02 has raw records, but no historical benchmark source hash/snapshot | Descriptive supporting result; do not imply exact source provenance |
| raw_incomplete | E18 retains a native run stopped during second-configuration setup | Diagnose setup and preserve partial measurements; excluded from formal E19 accuracy claims |
| functional | Recorded test outputs, CLI outcomes and source hashes | Correctness/protocol acceptance; not a latency sample set |
| sanitized_summary | E12 preserves failed probe outcomes and private artifact hashes | Historical capability limitation; private logs are not publicly reproducible |
| reported_only | L01 was transcribed from historical documentation; raw samples missing | Historical context only, excluded from headline claims |

Hashes detect artifact changes; they do not establish that an experiment is independently replicated. Each source-hash map describes its measured revision. Some files have since changed, and not all historical revisions are available as Git commits.

## Common configuration

Apple M5, 16 GiB unified memory, macOS arm64, Metal; 4 CPU threads, batch 512, FP16 KV. The pinned llama.cpp build is b10938, commit `f1e44dcc11d8802d107bd7331a3d3fd3e6f57b93`. Model and accepted template hashes are in the [report](README.md). GGUF file size (654,302,880 bytes) differs from measured weight buffers (633,495,552 bytes).

Model load and the initial warm prefix are excluded from the preparation experiments. Warmup and rotating-order details are preserved in each evidence file. These are small local trials, without randomized machine assignment, CPU isolation, or cross-device replication. Mac unified-memory RSS is not discrete GPU VRAM.

| Experiment | Workload / matched baseline | Samples per mode | KV allocation / sessions | Important exclusions |
| --- | --- | --- | --- | --- |
| E01 | Same 2048-token warm prefix, six 128-token appends; cached_scan vs explicit_delta; cold upper-bound control | 5 | 3328 tokens / 364 MiB / 2 | No HTTP/chat template/generation; initial prefix excluded |
| E01 stream | 512 new pretokenized IDs, 32-token arrivals; wait vs eager32 vs coalesced128, gaps 0/20 ms | 5 | Same | Arrival schedule simulated; source duration is 16×gap |
| E02 | Same token replay, 200 ms gap | 3 | Same | Historical benchmark source unrecorded |
| E03–04 | Context/chat/token/tool/parser/CLI functionality | Suite outcomes; capability probes individually n=1 | See per-run evidence | Scripted tool selection is not autonomous model success |
| E05 | Six tool-result appends; full cached transcript vs native checkpoint; 345/1225-token prefixes | 5 | 8192 / 896 MiB | Checkpoint replays 3 boundary tokens per round |
| E06 | Same chat fixture; preparation cache vs uncached checkpoint, plus two full-transcript controls | 5 | 8192 / 896 MiB | Warm verified template; not full generation TTFT |
| E07 | old_lifetimes vs early_release; 6 text sizes, 3 tool sizes in raw records including rejected inputs | 3 | 16384 / 1792 MiB / 9 | 9 distinct shapes: text 1024/4096/7168/14336/16384/65536 words, tools 16/128/512; memory/timing trials separate |
| E08 | Prior early-release request_json vs direct_messages | 3 | Same as E07 | Not the retained-all-temporaries baseline |
| E09 | normalized_json vs direct Jinja values + output gathering + bounded tokenizer scratch | 3 | Same as E07 | Combined optimization; cannot attribute savings to one component |
| E10 | Same evicted prefix/file: late restore vs asynchronous hint; three real-assembly cases and controlled 25/100/250 ms windows | 7 | 8192 / 896 MiB | Warm OS file cache; snapshot save excluded; no fsync |
| E11 | 6 cases × wait_all/coalesced/eager; complete semantic units, same final events/token counts | 7 | 8192 / 896 MiB / 4 | Model/538-token initial prefix excluded; no KV-file restore in this experiment |
| E12 | Two earlier HTTP repair probes | 1 each | Earlier HTTP configuration | Raw configs/traces stay private; protocol guards subsequently changed |
| E13 | Warm-prefill work-feature estimator: 63 calibration / 84 evaluation rows; fixed token/s, frozen and prequential online predictions on the same jobs | 7 per shape | 8192 / 896 MiB / 2 | Model load, prefix setup and shape warmup excluded; overhead separate; no scheduling change |
| E14 | Same-grid 63/84 jobs; original three predictors, raw and calibrated Roofline, same measured job | 7 per shape; probes 7 per primitive | 8192 / 896 MiB / 2 | Warm prefill only; two microprobe warmups; no held-out formula tuning or scheduler change |
| E15 | Same 63/84 jobs; eleven same-job predictions/ablations; original versus dependent-graph/FA rates and final FFN work | 7 per shape and per primitive/depth | 8192 / 896 MiB / 2 | Synthetic primitive payloads; one rate shape; no pure GPU/DRAM/frequency profiling; no scheduler change |
| E16–17 | Independent caps512/128, 63 calibration+147 evaluation jobs each, eight same-job frozen/online predictors; kernel/time interpolation diagnosis | 7 per shape; six widths/three contexts microprofile | 8192 / 896 MiB / 2 | Extra profiling cost, synthetic operator family; no GPU clocks/DRAM; E16 negative result preserved |
| E18 | Incomplete first frozen study; cap256 completed, cap64 setup rejected before measurements | 849 records | First cap256 only | Not formal accuracy control; all original rows retained |
| E19 | New caps256/64; interleaved equal-label sparse/diverse, extended/clipped table, old/weighted/residual fits and online baselines | 63 per calibration/cap;147 evaluations/cap; anchors3 per stage | 8192 / 896 MiB / 2 | 1162 native rows;294 offline references separate; no tuning on held-out labels |
| L01 | External-service capacity, marker-budget and memcpy observations | Original raw unavailable | Different historical setup | Reported-only, not mixed with local engine results |

E07 has six text sizes and three tool sizes, nine shapes in total. Successful and `FULL` capacity-rejection rows stay distinct; rejected inputs cannot establish usable model context length.

## Metric definitions

| Metric | Definition / unit |
| --- | --- |
| Median [min–max] | Descriptive statistics over original samples, per mode and workload; min/max are not confidence intervals |
| Frontend (E01) | Repeated text materialization, tokenization and/or prefix checking before evaluation; direct API only |
| Frontend (E05–06) | Full chat preparation including render/tokenize/guards/commit; six rounds per trial |
| Prepare wall (E07–09) | Warm repeated frontend preparation, separate runs from malloc sampling |
| Temporary heap | `max(0, sampled_heap_peak − heap_before)`; all-zone macOS malloc sampler every 200 µs plus phase probes |
| cxf bytes / token capacity | Allocator/accounted canonical storage, not total heap/RSS/KV |
| Logical / stored tokens | Logical references across sessions versus physical IDs retained in a shared block pool |
| Restore (E10) | Sequence-file read/native restore on owner thread; same live engine/session/revision |
| Actual overlap (E10) | Intersection of assembly/wait interval and measured preparation interval; not the nominal window |
| After-submit first token (E10) | First-token timestamp minus submission; includes remaining preparation and suffix decode |
| Total first token (E10) | First-token timestamp minus assembly start; used for end-to-end restoration-stage comparison |
| Total ready (E11) | Model-ready timestamp minus source start; full source + valid receipts + assistant header in KV |
| After-input ready (E11) | Model-ready timestamp minus final input/end timestamp |
| Prefill (E11) | Cumulative measured input-prefill execution wall time; not FLOPs, energy, or the total critical path |
| First token / output (E11) | Bounded preview callback; includes test-only independent full-tokenization oracle overhead |

`metrics.csv` uses bytes, ms, µs, tokens, counts or explicit scalar units and preserves experiment/dimension/source columns. Dimension `size` is a fixture size, not necessarily model token count. Absolute monotonic timestamps and nested phase probes remain in `raw-records.jsonl`, not the aggregate CSV. `record_index` is zero-based within the source evidence `rows` array. The 5191 records include 63 file timelines, metadata/status and 147 E13 calibration/evaluation records; three predictions per E13 evaluation row and five per E14 row and eleven per E15 row share one measured job.

E13 uses 3 nonnegative work coefficients, a 64-observation rolling window and range-based extrapolation warnings. Fixed token/s uses all 63 calibration samples, frozen uses no evaluation labels, and online always predicts before observing the current row. Calibration and evaluation token-size grids are disjoint; repeated online evaluation rows can learn from earlier rows. MAE/WAPE/MAPE and maximum errors stay distinct. Overhead is 10 repeated batches with 5000 predict / 200 observe calls per batch; reported µs are per call. Model/runtime/hardware/config fingerprints identify calibration applicability, not a universal hardware-only performance formula. See [D-COST](../design/cost-estimator-v1.md) for limits.

Medians of components need not add to the median total. A difference of medians is not the median paired difference: E11 paired results are retained separately in its original evidence. Run order rotates but pairs are not simultaneous. Do not claim statistical significance, p95/p99, a general stability improvement, or accumulate percentages across phases.

## E14 Roofline method

GEMM uses 2MNK FLOPs from actual GGUF Q/K/V/O/gate/up/down matrix shapes and Q8_0 storage bytes. Attention uses GQA dimensions (dk=dv=128 here), causal pairs including the diagonal, one KV pass per chunk and no materialized score matrix. The logits projection runs once per chunk, matching native cxf. Time sums per-group max(F/P, B/BW), not a global max. Norm/RoPE/activation, quantization instruction work and actual tiling/cache traffic are omitted; operand bytes are not measured DRAM traffic or a verified lower bound. See [D-COST](../design/cost-estimator-v1.md) for equations and primary references.

Same GGML/Metal Q8_0 GEMM K1024/N3072/M512, F16 GEMM K128/N4096/M512 and 64MiB copy provide effective synchronized wall-clock rates, each with 2 warmups and 7 retained samples. Copy counts both reads and writes. F16 GEMM is an imperfect proxy for fused attention; none is a peak hardware specification. Raw Roofline is retained even though it substantially overestimates. The calibrated variant fits linear time, attention time and decode-call count using exactly the same 63 labels and parameter count as the original frozen model; no evaluation labels. Online learns only after each prediction. New run rates enter the local Roofline calibration tag. Core estimator has no model loader, platform probing or automatic scheduling.

E14 contains 195 records, including 147 measured jobs, 21 primitive samples, 10 raw-estimate overhead rows, 10 original-estimator overhead rows and metadata/checks. Raw estimate overhead is one chunk at prefix2048/new128/batch512; 5000 calls in each of 10 batches. Calibrated predict/refit overhead was not separately measured. Both C unit suites, ASan/UBSan and default regression completed before inference timing. The 21 shape oracles passed. E13 remains a distinct historical run; lower E14 baseline error cannot be attributed to the new estimator because inference did not change. Current source hashes are in E14; refactoring shared fitting code does not retroactively update E13 hashes.

Rerun from the complete project with a matching local installation (current runner: macOS/Metal only):

```sh
python3 tools/bench_roofline_estimator.py --sdk "$CXF_SDK" --runtime "$CXF_RUNTIME" --model "$CXF_MODEL" --out build/recheck-roofline
```

## E15 diagnosis method

E14 arithmetic is independently recomputed from its archived GGUF/rate/work fields for all147 rows. Agreement confirms old arithmetic, not the accuracy of its traffic or physical-work model. Pinned Qwen3 selects the requested output rows before the final FFN; new optional `last_layer_ffn_tokens=1` corrects this, zero retains legacy full-token FFN, and callers must recompile. Source findings/hashes are recorded in E15.

Rates use dependent graphs of depth1 and16, distinct per-stage weights/KV, two warmups and7 retained synchronous wall times. Q8 matmuls alternate K1024/N3072 and K3072/N1024; F16 alternates K128/N4096 and K4096/N128, M512. FA uses actual interleaved strides, FP16 KV/F32 Q/accumulation, Hq16/Hkv8, dk=dv128, prefix2048/new512 and causal mask. Dense n_kv_max is0, matching upstream; the first development probe mistakenly selected a sparse kernel and is excluded. Copy uses64MiB pseudorandom values, counts reads+writes; other arithmetic uses simple oracle-verifiable synthetic payloads. Extra work divided by the depth-median difference gives the rate. Nonpositive difference or overlapping observed depth ranges rejects it. Range separation is not a confidence or stability guarantee. Record wall rates and graph intercepts; none is a hardware peak or pure GPU timestamp.

Order is initial legacy probes, Q8/F16/FA/copy depth1/16, repeated exact legacy probes, then inference calibration/evaluation. Later legacy probes show order effects; clock/temperature are unmeasured, so no throttling attribution. New rates and policy freeze before model labels. Same63 calibration and84 evaluation grid as E13/E14; final FFN work, bandwidth, linear rate and attention rate have separate ablations. Full corrected calibration uses the same63 labels and three parameters as old frozen; old online predicts before learning each current label. Never sum ablation effects or treat multiple predictors as independent inference jobs.

E15 has278 records:147 measured jobs,42 isolated samples,56 depth samples,4 rate rows, graph/warm profile rows and the original metadata/calibration/overhead/check rows. Both C unit suites, ASan/UBSan and default regression finish before timing;21 shape oracles pass. Raw/calibrated prediction overhead uses5000 calls per batch; calibrated refit uses200 calls on a cloned calibration after evaluation,10 batches. Core inference is unchanged. Old evidence/source hashes remain historical; E15 identifies the current measured estimator.

The FFN fix worsens the rate-corrected raw MAE slightly, showing compensating errors. Full corrected raw still has54.856ms max error. A single rate shape does not cover small-batch vec kernels, padding/cache, omitted operators, contention or cold startup; DRAM traffic, pure GPU timing and GPU clocks remain unmeasured. No inference-speedup or broad stability claim.

```sh
python3 tools/bench_roofline_diagnose.py --sdk "$CXF_SDK" --runtime "$CXF_RUNTIME" --model "$CXF_MODEL" --out build/recheck-roofline-diagnosis
```

## E16/E17 batch/context and shape method

See [D-COST current contract](../design/cost-estimator-v1.md#e17-当前形状估算契约与结果) for exact interpolation, ownership, validation and source limits; [per-cell CSV](data/cost-shape-errors.csv) gives all42 prefix×query×cap groups per run with eight modes, signed bias and actual median/min/max/MAD. Each experiment has916 raw records, including420 actual jobs (126 calibration+294 evaluation),336 shape probes and24 shape rates. Multiple predictors share each job; do not multiply inference samples by eight.

Both batch caps use separate calibration/window identities; allonline models see the same earlier labels, predict before observing current rows, and use64 samples. Frozen models use only63 calibration rows. Calibration is0/2048/6144×32/128/512×7. E16 evaluation is0/1024/4096×8/16/19/20/64/256/768×7; E17 changes to512/3072/5120×12/18/24/48/192/384/640×7 after diagnosing E16. E17 used E16 as development evidence, not as untouched validation. No tuning or rerun selection on E17 labels.

Shape knots are widths8/16/19/20/128/512 andcontexts0/2048/6144. Linear Q8 profiles use the alternating matrix family fromE15; FA uses actual interleaved layout, F32 Q/accumulation, FP16 KV, causal mask, dense hint0 and KV span padded to256. Each point usesdepth1/16,2 warmups+7 samples, distinct dependent-stage weight/KV buffers and arithmetic oracle; a nonpositive slope or overlapping depth ranges rejects it. Extra profiling precedes model labels. Kernel regions<=8/9..19/>=20 follow pinned source; route columns are inferred, not hardware counters. The table is960B and holds numbers only.

E16 interpolated inverse linear rate and then scaled token work, creating artificial time curvature; E17 interpolateswidth/rate service time before converting back. FA always interpolates time. Within-region interpolation and endpoint clamps produce explicit extrapolation flags. Calibration includes some later chunks beyond the6144 context endpoint; these are flagged, not silently treated as in-domain. Evaluation width12 is below the16/19 narrow-region endpoints and is also explicitly extrapolated. One shared table plus one numeric calibration is4080B; benchmark baseline copies cost additional memory.

Hand interpolation/kernel-boundary/chunk/extrapolation/atomicity units, ASan/UBSan and default regression finish before model timing. Each run passes60 fresh-KV shape oracles. Prefix setup, warmups, load and oracle work remain outside service time. Cap512 runs before128; cross-cap differences are not simultaneous pairs and cannot uniquely isolate chunk overhead. GPU time, clocks/temperature and actual DRAM traffic remain unmeasured. Overhead uses10 batches of5000 predictions and200 observations on a post-evaluation clone.

E16 wrapper missed the re import only after native success; metadata recovery used the original manifest and unchanged916 raw records without inference rerun. Its measurement runner hash and processor hash remain separate historical identities. E17 source hashes identify the current interpolation. Current runner can finalize a completed raw run once using --finalize-existing; it rejects an existing evidence file. Sources have evolved, so rerunning an old grid requires its measured revision for exact historical code.

```sh
python3 tools/bench_cost_shapes.py --sdk "$CXF_SDK" --runtime "$CXF_RUNTIME" --model "$CXF_MODEL" --new-grid --out build/recheck-cost-shapes-v2
```

## E18/E19 frozen calibration study

Current numerical contract and results are [D-COST E19](../design/cost-estimator-v1.md#e19-当前残差校准契约与验证). E17 is development evidence only: exact3-variable active-set NNLS reproduces the old fit; weighted/residual candidates informed the fixed method before E19. New caps256/64 use new prefixes1280/3328/5376 and query sizes10/15/21/72/176/352/672,7 repeats each. Sparse0/2048/6144×32/128/512 and nine diverse points each provide63 labels per cap; samples interleave per prefix. Expanded context7168 covers calibration chunk starts. Clipped-table control retains6144; all evaluation raw features agree exactly across the two tables.

Residual prediction is raw plus signed corrections for calls, tokens/512 and raw_attention_ms/100; weighted-RMS normalization, weight1/max(actual_ms,20), ridge.01 around zero correction, clamp final duration>=0. Same3 coefficients as old NNLS but different basis/objective/prior; no physical attribution of coefficients. State3632B, table960B, caller serialized, no heap/context. The same-basis weighted NNLS controls are offline, fitted strictly to calibration labels and stored with source_record_index, outside the native rows. They are legitimate same-job prediction controls, not new inference samples.

Anchors(0,32)/(2048,17)/(6144,512) each have3 repetitions before calibration, after calibration and after evaluation; all excluded from fitting. Native totals:252 calibration+294 evaluation+54 anchor jobs,420 shape microprobes/30 rates and base probes/metadata/etc=1162 records. Three stages describe phase variation, not hardware clock or temperature attribution. Sparse-initialized online baselines predict before each current label; all frozen methods remain frozen.

E18 first run preserves849 native records; cap256 finished39 shape oracles before cap64 initialization returned RANGE (defaultprefill_min128>batch64). No cap64 sample exists there. E19 changes only the fixture threshold to min(128,batch), preserving flush=true behavior and the frozen strategy; no accuracy-based selection. Sources/hashes for both stages are retained; old artifacts remain private except safe numerical evidence.

E19 residual/shape units, ASan/UBSan and default regression completed before measurement.78 fresh-KV workload shapes passed canonical/greedy/exact token/batch checks. Mathematical coefficient checks and training-only frozen predictions are recomputed offline. Extra profiling8.695s includes initialization/warmups, not each prediction. Overhead10 batches:5000 composed shape+residual predictions,200 residual refits on a post-evaluation clone. Full validation/training/anchor CSV are in this directory's data folder. New frozen calibration still regresses relative to raw at cap64; old18 evidence files are unchanged.

```sh
python3 tools/bench_frozen_calibration.py --sdk "$CXF_SDK" --runtime "$CXF_RUNTIME" --model "$CXF_MODEL" --out build/recheck-frozen-calibration
```

## E11 boundary and resource rules

The source contains 16 immutable complete semantic units and one end frame. Arbitrary wire splits (≤7 bytes in the trial) test framing; they do not provide safe arbitrary BPE append into a still-open message. End is delivered immediately after the last payload at the same target deadline. Earlier `stream-final-20261030` added an extra EOF gap and is excluded; `stream-final3-20261030` supplies the official evidence.

Text has 701 new input tokens (1239 total); file cases have 619 (1157 total). All modes evaluate exactly these input counts. The callback performs a test-only full-tokenization oracle and emits at most 16 preview tokens. File processing is deployment-fixed `cc unit.c -o unit && ./unit` in version-specific directories, only after complete file_end; it does not execute untrusted arbitrary shell supplied by the stream.

Limits: frame 16 KiB, source payload 64 KiB, 128 events, 8 file versions, backlog 16, receipt 4 KiB. Old receipts remain queryable as stale; current-version receipts alone enter the final prompt. Cancellation prevents publication at native/callback boundaries; it does not forcibly interrupt a running native call. Correctness covers incomplete files, stale versions, cancellation, bounded backpressure, receipt feedback before end and cleanup.

## Recompute the publication

From repository root, Python standard library only:

```sh
python3 tools/build_benchmark_report.py --check
python3 tools/build_benchmark_report.py
python3 tools/build_benchmark_report.py --check --export build/cxf-benchmark-results-20261030.zip
```

`--check` verifies archived summaries against raw values, input-token/time-order invariants, paired E11 statistics, generated reports/CSV, local release links, SHA manifest and scoped privacy patterns. It does not rerun model correctness. `--refresh-inventory` explicitly refreshes the local metadata inventory; ordinary rebuilds preserve the frozen inventory even on machines without `build/` artifacts.

For the vector charts, install/use ReportLab in a separate environment, then run `python3 tools/render_benchmark_figures.py`. PNG previews use macOS `sips`; SVG masters are standard ReportLab charts. Rebuilding charts requires regenerating the manifest afterwards. PNG rendering can differ across OS/font versions; recorded SVG/PNG hashes describe the shipped files.

## Rerun model experiments

Use your own matching SDK, runtime and model paths; no private endpoints or credentials are required. The following are current runner commands for a **new** trial, not a promise of byte-identical historical reruns. Build outputs are private/untracked; never replace an old evidence file with new measurements.

```sh
# shell variables point to your matching local installation
python3 tools/bench_llama_context.py --sdk "$CXF_SDK" --runtime "$CXF_RUNTIME" --model "$CXF_MODEL" --repeats 5 --out build/recheck-direct
python3 tools/bench_llama_context.py --sdk "$CXF_SDK" --runtime "$CXF_RUNTIME" --model "$CXF_MODEL" --repeats 3 --stream-gap-ms 200 --stream-only --out build/recheck-slow
python3 tools/build_agent_native.py --sdk "$CXF_SDK" --runtime "$CXF_RUNTIME" --model "$CXF_MODEL" --test --test-long-context --out build/recheck-native
python3 tools/build_agent_native.py --sdk "$CXF_SDK" --runtime "$CXF_RUNTIME" --model "$CXF_MODEL" --bench-chat --repeats 5 --out build/recheck-chat
python3 tools/build_agent_native.py --sdk "$CXF_SDK" --runtime "$CXF_RUNTIME" --model "$CXF_MODEL" --audit-context --audit-kv-tokens 16384 --out build/recheck-memory
python3 tools/build_agent_native.py --sdk "$CXF_SDK" --runtime "$CXF_RUNTIME" --model "$CXF_MODEL" --test-kv-worker --bench-kv-worker --repeats 7 --out build/recheck-kv
python3 tools/build_agent_native.py --sdk "$CXF_SDK" --runtime "$CXF_RUNTIME" --model "$CXF_MODEL" --test-stream --bench-stream --repeats 7 --out build/recheck-stream
python3 tools/build_agent_native.py --sdk "$CXF_SDK" --runtime "$CXF_RUNTIME" --model "$CXF_MODEL" --test-stream --test-kv-worker --test-long-context --sanitize --out build/recheck-sanitized
make test
```

Current runners include multiple comparison controls. E05/E07/E08 are earlier stages; exact historical paths require their recorded source revisions. No upstream headers/libraries or model weights are redistributed. Native Windows/CUDA, cold SSD, actual cloud transport, automatic agentctl integration, and broad model capability remain future acceptance work.

## E20 scalar residual Kalman (2026-10-31)

The optional `cost_kalman.h` layer stores bias, posterior variance, Q and profile/count in40 bytes, with no heap or token/KV storage. It uses a scalar random walk residual: observation=actual−unfiltered_base. Prediction is read-only and adds Q once provisionally; only an accepted nonzero observation commits that advance. Observe uses the original base and the same R, never the already corrected prediction. Covariance updates use Joseph form. Failed/cancelled work is excluded; changed profiles need separate/reset state. Q is per observation, not per elapsed second.

The strategy was written in D-COST before implementation. Both caps256/64 reuse E19 sparse/diverse training and the extended shape microtable. Evaluation changes to prefixes1792/3840/5504 and new tokens11/18/26/88/208/400/688,7 repeats per cell. All294 evaluation tasks retain raw, frozen residual/diverse and shape-online base predictions.78 fresh-KV workload shapes check canonical tokens, greedy and batch counts independently. No model or scheduler algorithm changes.

Noise comes only from the nine diverse calibration groups per cap, each with7 labels: Rconstant=max(.25ms²,median sample variance). Fit nonnegative c0/c1 by ordinary NNLS to group variance against [1,(group_mean_actual/100)²]; Rscaled(base)=max(.25,c0+c1*(base/100)²). Set Q=.01Rconstant, alpha=(sqrt(.01²+4*.01)−.01)/2, P0=alpha*Rconstant, bias0. These choices precede evaluation; no evaluation tuning or rerun selection. Fixed R with this steady-state initialization equals EWMA(alpha), checked in C and independent Python replay.

The C filter replayer consumes fresh native measurements in their original chronological order within each cap/base. All layers predict before seeing the current actual label; only then update. No filter warmup uses calibration residuals. Fixed/scaled Kalman and EWMA share the same past labels; the shape-online base also has its own pre-existing rolling fit.882 indexed layer traces refer to294 original evaluation jobs and are separate from1162 native records, so the combined raw-record count does not triple the inference samples. Layer overhead is CPU-only,10 batches of50000 predictions/10000 updates, measured separately from native timing.

Report MAE/RMSE/max/signed bias/WAPE, first14 cold-state errors, all42 cells ×3 bases ×4 modes, and normalized innovation mean/SD/skew/excess-kurtosis/lag1. Coverage of ±1.96sqrt(Pprior+R) is descriptive; noise need not be Gaussian, independent or unbiased, and this is not a validated95% guarantee. Fixed Kalman adds no accuracy over equivalent EWMA; scaled R may still regress. This is numerical prequential replay, not a live Agent scheduling speedup. Native/CPU overhead source and runtime hashes are archived; unit Joseph/gain/EWMA/step/spike/atomicity, ASan/UBSan and full default regression precede GPU timing.

```sh
python3 tools/bench_kalman_cost.py --sdk "$CXF_SDK" --runtime "$CXF_RUNTIME" --model "$CXF_MODEL" --out build/recheck-kalman
```

## E21 real tool windows and simple EWMA

The optional Native tool host is used by an explicit `agentctl-native` test.run wrapper and by the measured fixture. It owns one existing-style KV worker; Native borrows that same engine on its owner thread. One serial tool and at most one preparation thread run. No second model/context or persistent transcript is created by this module. The test oracle deliberately keeps reference IDs outside the production path; this trial is not a whole-process memory audit.

Seven modes: resident; recompute late/immediate/timed; private snapshot restore late/immediate/timed. Resident is the normal warm baseline. Recompute explicitly evicts KV while preserving canonical tokens. Restore actually saves a same-live-session checkpoint before eviction; save/checkpoint/eviction costs are charged to the task. Snapshots are removed after each call, with a1GiB budget. Physical FP16 KV allocation stays896MiB, OS file cache is warm, and no PCIe/cold-SSD claim is supported on unified memory.

Two history fixtures (128/2048 repeated words; actual token counts recorded), two actual tool workflows,7 task replicates per mode/case,3 measured calls per task. Read invokes real fs.read. Compile/test invokes a deployment-fixed C compiler plus a real deterministic compute/test executable, sees a failed check, writes an actual complete correction using fs.write_apply, then sees two passes. No sleep creates a performance window. Test actions are selected deterministically because model autonomy is a separate question. CLI smoke probing with the0.6B model is separate from performance. Intentional test failure is a valid receipt, not a preparation failure.

Each operation/case first gets3 actual calibration tasks. All policies start with the same median tool duration and preparation ms/token for their operation; keyed slots isolate modes/steps. Subsequent labels update simple EWMA with alpha=.09512492197250393. Delayed start=max(0,estimated_tool_ms−prefix_tokens×estimated_prepare_ms_per_token−5ms). Unknown estimates dispatch immediately; tool completion wakes a pending timer early. Evaluation labels affect future observations only. Mode order rotates by replicate; all negative paired differences remain, with no best-run selection or evaluation retuning.

After-tool latency includes the remaining prefix work, cleanup, real receipt assembly, full template rendering/guard, tokenization, suffix prefill and first greedy. Three-call task time additionally includes checkpoint/save/eviction, actual tools and correction/intermediate preparation. Initial history construction/prefill, model load and calibration are excluded and recorded. Independent full-upstream-render/tokenization/fresh-KV greedy references (12 case/step references) validate every exact canonical sequence and first token. Oracle validation occurs after each first-token timestamp; the first two oracle CPU/GPU durations are subtracted from raw task wall time, both values retained. No generation of a full answer is timed.

The931 native records comprise108 calibration call timings+36 calibration task timings,588 evaluation call timings+196 evaluation task timings, and3 metadata/correctness/closure records. Per-call metrics are correlated parts of a task, not588 independent full tasks. Pair comparisons align by replicate within each case and operation; observed min/max/sign counts are descriptive, not confidence or p95/p99. Token/receipt hashes and source/runtime hashes are public; raw prompts, process logs and private snapshots are excluded. EWMA predictions, prior-label counts, time differences/overlap and summed task metrics are recomputed offline by the report checker.

Qualification precedes timing: borrowed-engine survival and close, timer/in-flight/ready cancellation and rollback, environment change rebuild, key capacity, actual tool error/output capacity behavior, Native/14K/KV/Stream regression, ASan/UBSan and default tests. Only a serial Native test.run integration is delivered; general intent epochs, private candidate ADOPT, multi-consumer fairness, unique KV free-space accounting, asynchronous cold startup and real cloud SSE remain future D-PREP slices.

```sh
python3 tools/bench_tool_prepare.py --sdk "$CXF_SDK" --runtime "$CXF_RUNTIME" --model "$CXF_MODEL" --out build/recheck-tool-prepare
python3 tools/build_agent_native.py --test-tool-prepare --sanitize --out build/recheck-tool-prepare-sanitized
```

E21 interpretation: long-history timed recomputation reduces after-tool wait sum1759.905→631.885ms; timed restore242.040→150.610ms. Immediate restore exposes less wait than timed but holds the ready prefix longer. Always-resident KV remains fastest in complete task time; fast reads have essentially no overlap window. Native restore and total preparation must be distinguished: immediate restoration adds roughly282–293ms outside the native restore call across three tools. Queue/fork/wakeup were not independently emitted by this collector, so that residual remains unattributed; it is not evidence of disk/bus bandwidth loss. The component CSV preserves it without new inference samples. The production0.6B CLI smoke probe emitted bare JSON and executed no tool; a separate compile-time deterministic Native-parser action harness validated real CLI/registry/AgentStore routing and exactly one real test process under sanitizers.

E21 `tools_per_task` and task-row `calls` count the three measured wrapped invocations. Compile/test tasks additionally execute one actual fs.write_apply; its cost and following intermediate Native preparation are included in complete task time (`inter_tool_ms`), while it is not an extra after-tool latency sample. The controlled action source does not infer a repair autonomously.

The actual CLI routing can be checked separately from model generation:

```sh
python3 tools/build_agent_native.py --test-tool-prepare-cli --sanitize --out build/recheck-tool-cli
python3 tools/test_tool_prepare_cli.py --scripted-actions --binary build/recheck-tool-cli/test-tool-prepare-ctl --runtime "$CXF_RUNTIME" --model "$CXF_MODEL" --out build/recheck-tool-cli-run
```

## E22 attribution and E23 spawn/progress controls

E22 is observation-only:547 records (96 calibration,448 evaluation,3 status), resident/restore-late/immediate/timed, original128/2048 words and30M compute iterations. New trace timestamps partition preparation into admission, worker queue, before-native verification, native restore+sync, ready tail, finish queue/owner/return and coordinator tail. Their sum equals full preparation per record. Parent process trace records the fork-call span, first parent-read output, reap and return; no observer executes in the child. On Darwin, C trace uses CLOCK_MONOTONIC_RAW to match libc++ steady_clock; ordinary CLOCK_MONOTONIC absolute values are not interchangeable. Old E21 times were internally C++ steady_clock and remain unchanged.

For an observed call, signed ready lateness equals `(tool_hat−tool_actual)+(prepare_actual−prepare_hat)−margin+dispatch_offset+clamp`; clamp accounts for capped dispatch. Exposed prefix wait is its positive part and cannot be inferred from a near-zero signed mean. Post-prefix wait includes final real receipt/suffix work. The E21 audit references original row indices, adds no native inference samples, and does not retrospectively assign unrecorded phases.

E22 locates immediate extra wall time primarily before native restore, overlapping fork, while queue/finish are small. This is not a measurement of an internal libc lock, GPU frequency or actual memory/disk bandwidth. The subsequent E23 launch-method control tests the practical cause without changing the KV format or upstream kernel.

E23 is a separately frozen new fixture:320/1792 history words,25M compute iterations, seven modes (spawn resident; fork/spawn × immediate/timed/progress),7 repeats,979 records (192 calibration,784 evaluation,3 status). Each backend/state/case uses3 complete calibration tasks. All policies of one backend get identical median initial labels; post-spawn phase labels come from those same tasks. Then only each mode's previous complete observations update EWMA(alpha=.0951249). Margin remains5ms. Tool command/cwd/env/stdio/process-group/timeout and the actual repair are unchanged; a failed spawn does not retry via fork. macOS26+ uses standard posix_spawn addchdir; older macOS/Linux retain fork pending acceptance.

Progress observes only the current epoch's parent spawn-return event and predicts end=that observed event + prior EWMA(after-spawn duration). Dispatch=max(event,end−prior_prepare−5ms), on an absolute raw-monotonic deadline. Unknown remaining cost starts at the event; no event starts after tool completion. Old/stale/duplicate/future/out-of-range progress is rejected, and cancel/cleanup obey the same owner and acceptance boundary. The Native CLI can opt into --tool-prepare progress; original timed remains default.

The full ablation retains all policy/backend cases. Paired wait/holding/task differences align replicate IDs. `both_wait_and_hold_lower_pairs` requires both strictly lower in a pair; ties are not wins. Medians of paired differences differ from subtracting independently computed group medians. In the spawn/timed→progress comparison, short/long paired wait savings3.334/8.243ms contrast with holding savings−2.180/+0.089ms, joint wins2/7 and4/7, and long task saving−15.058ms. This does not support a stable Pareto improvement or a new default. Compiler remaining time drifts; final suffix prefill remains roughly28–38ms group mean per call in spawn cases.

All signed errors, mean/median/ranges, full stage partitions and negative pairs are in restore-estimate-audit / restore-stages / restore-stage-summary / restore-control-pairs / restore-task-summary CSV. Derived views reference native rows and are not added inference calls. Opaque epoch/key identifiers remain in raw records and are not averaged as performance metrics. Two rounds each use12 independent full-template/token/fresh-KV references, checking408/732 timed calls. Three measured wrapped calls plus the actual correction write contribute to each compiler task; initial model/history/calibration and oracle costs are excluded and recorded as before. Ready-prefix early holding is not a full cell-occupancy integral or physical KV-buffer release.

Qualification precedes performance: progress epoch/duplicate/bounds/cancel, tool once/capacity/process-group timeout, Native/14K/KV/Stream, ASan/UBSan and actual deterministic CLI routing. These remain M5/Metal/Qwen0.6B warm-OS-cache measurements with scripted action selection; no Windows/CUDA/cold-SSD/cloud/model-autonomy improvement claim. E23 measured source is additionally preserved privately with its hashes; source/build artifacts are excluded from the public results ZIP.

```sh
python3 tools/bench_restore_timing.py --sdk "$CXF_SDK" --runtime "$CXF_RUNTIME" --model "$CXF_MODEL" --out build/recheck-restore-timing
python3 tools/bench_restore_balance.py --sdk "$CXF_SDK" --runtime "$CXF_RUNTIME" --model "$CXF_MODEL" --out build/recheck-restore-balance
python3 tools/build_agent_native.py --test-tool-balance --test-tool-prepare-cli --test-kv-worker --test-stream --test-long-context --sanitize --out build/recheck-restore-qualified
python3 tools/test_tool_prepare_cli.py --scripted-actions --policy progress --binary build/recheck-restore-qualified/test-tool-prepare-ctl --runtime "$CXF_RUNTIME" --model "$CXF_MODEL" --out build/recheck-restore-cli
```

## E24 complete-task attribution and early snapshot cleanup

E24 first audits all14 E23 spawn/timed→progress compiler pairs without changing E23. Source hash and task/call indices are retained; these are derived references, not14 new native samples. At each task, task_ms = sum(pre-tool) + sum(tool_ms) + sum(post-tool-first) + inter_tool_ms + bookkeeping_ms. The residual includes measured collector/loop/owner-return gaps after excluding the first two oracle durations; it is not all production scheduler cost. The source pair corresponding to the15.058ms median is decomposed additively. Component medians cannot be summed.

New validation freezes512/2304 history words,18M rotate/xor iterations, affine repair checked at two inputs (calc(3)=7,calc(9)=19), actual compile FAIL→write→PASS→PASS and real fast read. It uses spawn throughout, five controls: resident, timed/no-event, timed/event, progress/return-cleanup, progress/early-cleanup. Per state/case three complete calibration tasks precede seven evaluation tasks per mode; every mode has independent keys and the same initial state labels. Mode order rotates by rep. Alpha=.09512492197250393 and margin5ms are unchanged; evaluation labels do not tune policy.659 native records =96 calibration +560 evaluation +3 status;492 call rows (72 calibration,420 evaluation),164 complete task rows. Twelve independent fresh-KV reference prompts validate every call's canonical token and greedy result.

Report fields add preparer thread creation start/end, actual tool-function entry, callback start/end, early-drop start/end and both cleanup return codes. Old tool_start remains unchanged, so progress's thread-creation interval inside that old tool clock is visible rather than silently redefined. Callback timing surrounds the actual parent progress API (including its checks, mutex and notify); it does not capture indirect system contention. Timed/no-event measures the complete observation pathway under the same scheduling algorithm, but seven serial repeats cannot isolate all cache/frequency/OS load effects. Tool wall time includes shell/compiler/compute/reaping and any interference; save before the current event cannot be its direct callback cost.

release_snapshot_early is optional/default false. After finish drains the restore ticket, the same preparation thread drops the no-longer-needed private snapshot. Prefix-ready remains the device-ready timestamp, not cleanup completion. Caller joins cleanup before return; tool completion/cancellation/exception and normal fallback cleanup keep once-only execution and ownership. A failed early drop retains the handle for the return cleanup retry and both results remain observable. This is not an unbounded background queue. Exposed deletion = sum(max(0,drop_end−max(drop_start,tool_end))) across early/return attempts; it measures deletion service after tool completion, not the entire join or disk occupancy integral. No shrinking of preallocated physical KV is implied. CLI opts in with --tool-snapshot-release early; default return/timed remains.

All pre-tool save/evict, repair, final rendering/prefill, greedy and cleanup remain in complete task cost. Initial history/load/calibration and measured oracle durations remain separately recorded. Every paired negative is retained. Attribution tables, pairwise results, source snapshots/hashes and functional qualification are separate. The E24 runner is tools/bench_tool_taskcost.py with the same --runtime/--model/--out arguments as E23; choose a fresh output directory. ASan/UBSan, early-release cancellation before/during/after native work, tool exception, snapshot budget reuse across tasks, Native/14K/KV/Stream, default regression and deterministic CLI actual process once are completed before unsanitized timing. Warm OS cache, one M5/Metal/model, no Windows/CUDA/cold-SSD or autonomous-model acceptance.

## E25 unlink / last-close resource isolation

This diagnostic keeps E24's short512-word/18M affine-repair workload and separates trusted compiler/run child phases. It is not a cross-run speedup versus E24: the small C driver and sparse resource observations add common overhead. Five controls (resident, restore-late, progress-return, progress-early, progress-unlink-hold), three calibration tasks per state, ten evaluation tasks per mode, three real tools per task. Forward/reverse cyclic Latin order puts each mode in each position twice.227 native records:24 calibration (18 calls+6 tasks),200 evaluation (150 calls+50 tasks),3 status. Three independent fresh-KV references validate168 call rows. Fixed alpha/margin, same state calibration medians, mode-local past observations only. Actual correction write and intermediate prepare remain charged.

The hold control opens+unlinks after restore finish, retaining an owner-only fd until tool completion. Its slot and disk budget remain charged until last close. This distinguishes unlink metadata from last-reference release; see [Apple unlink](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/unlink.2.html). It does not force exact timing of asynchronous kernel reclamation. Early/full versus early/hold may differ in descriptor-opening cost as well; direct operation timings remain recorded.

The fixed benchmark C driver spawns cc with the original arguments, then the actual check binary, using [wait4](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/wait4.2.html) CPU/resource totals including waited descendants. It writes numeric private phase metrics without altering stdout/test receipts; parsing is charged after the tool. Compile/run clocks are Darwin MONOTONIC_RAW, matching the parent steady clock. User/system CPU, fault counts, IO operations and context switches are separate from wall time. A parallel compiler's CPU can exceed wall time; their difference is not a pure IO duration.

Optional parent probes bracket tools and both deletion stages: getrusage SELF, Mach task resident/physical footprint gauges, and global VM pagein/out, compression/decompression, swapin/out. Gauges can decrease; global deltas include other processes and cannot be assigned exclusively to deletion. [Apple getrusage](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/getrusage.2.html) notes cache-served IO is not counted as new filesystem input/output, so zero IO counters do not prove no filesystem CPU or cache/memory contention. Units are operations/pages/bytes as named, not inferred SSD bandwidth. No ru_maxrss unit assumption is used. Resource-sampling wall cost is included rather than subtracted.

All source/runtime hashes and a private measured-source snapshot identify the diagnostic revision. Tool/Native cancellation, live-ticket unlink rejection, retained slot/budget accounting, duplicate hold/stale recovery, last close, worker-close cleanup, ASan/UBSan, Native14K/KV/Stream, default regression and real CLI tool-once precede unsanitized timing. Old evidence and every negative new pair remain. Snapshot necessity is policy dependent: normal resident does no save/restore; current restore files are same-live-engine only, not cold restart or universal per-tool overhead.

## E26 first-executable execution and incremental recipe

E25 located a large non-CPU interval in actual check launch/reaping. E26 freezes new768-word/12M tasks with calc(x)=3*x−2 at inputs3 and9. Five modes (resident always-build; progress return/early/hold always-build; resident incremental),3 calibration tasks per state,7 evaluation tasks per mode, rotating order.167 native records:24 calibration (18 calls+6 tasks),140 evaluation (105 calls+35 tasks),3 status. Three independent fresh-KV references check123 call rows. Mach/parent resource probes are disabled; child wait4 and private numeric phase labels remain charged common work.

The check records CLOCK_MONOTONIC_RAW on entry into main and after the actual calculation. Driver timestamps cover spawn start/return and wait4 completion. First-run wall = pre-main + compute + exit-tail per sample. Spawn-call duration is contained in pre-main and not added to it. A child can enter main before the parent resumes, so bounds assert spawn-end and main separately, without an unconditional cross-order. Every measured compute interval exceeds.5ms, confirming the actual loop is inside these boundaries on this pinned compiler/build. No conclusion is made about a specific loader/security subsystem.

Each test tool executes the same binary twice. The second run's stdout is redirected to/dev/null and status must match first; both executions are charged, including FAIL. This gives within-call first-versus-repeat launch control without recompilation between them. There are246 actual check executions across123 calls, including calibration. Incremental mode checks source mtimes/executable availability with fixed trusted filenames; each trial deletes the binary first. Step0 compiles, step1 performs the real correction then compiles, step2 reuses unchanged artifact. All tests still run twice per call. This is an explicit benchmark/deployment recipe, not an automatic cxf build cache or altered model permissions. No user test command is silently rewritten.

Launch strata separate98 evaluation calls with a new artifact from7 incremental reused calls, and their paired reruns. These are correlated stage observations of35 evaluation tasks, not98 independent end-to-end trials. E26 compares seven paired complete tasks, preserving all negatives and their indexed sources. Fixed EWMA/margin and past-only seeds remain; there is no retuning on held-out evaluation. Repeated execution adds work, so E25 and E26 task times are not subtracted as speedup. E24 lacks child-entry measurements; its old313ms regression is retained without retroactive kernel attribution.

E26's driver/workload/main boundaries, matching double status, real correction/rebuild and unchanged-source reuse passed ASan/UBSan. Production worker/Native/tool-host hashes match the earlier E25 full Native14K/KV/Stream/default/CLI acceptance, explicitly reused rather than described as rerun. Exact source snapshots are private and hashes public. Sources, workload and default-policy limitations remain distinct from the incremental-workflow result.

## E27 resident Native six-call flow

E27 returns to ordinary resident KV. Two modes use the same single-owner Native and template cache: directly execute original tools, or wrap them with AgentToolPrepare resident/timed. There is no save/forced eviction/restore/speculative preparation.512/3072-word history,7 tasks/mode/history, rotating order. Six actual calls per task: read config, test FAIL, write correction, test PASS, read back, test PASS. Native prepares actual receipts and samples first greedy after each call.199 records =168 calls+28 complete tasks+3 status;12 independent fresh-KV references validate every call. Evaluated prefix is checked before/after tools and no.kv files are created. Both modes evaluate397 new tokens/task, with existing history retained. Full prompt-token sums are work accounting, not timings against a cleared-KV baseline.

The same fixed prebuilt helper validates configuration; task timing never recompiles it or changes build commands. Its first-execution and warm timing are recorded outside tasks, as are model load/initial history. No evaluation labels tune policy. Lazy GPU variants may still initialize in early measured calls; all repeats and ranges stay visible. Task starts at first tool entry and ends at the sixth first-greedy sample. Per-call full-token/fresh-reference oracle work follows first greedy; first five intervals are subtracted from raw task wall, while the last is after task end. Both raw wall/exclusion and common collector bookkeeping remain recorded. Tools, correction, final suffix and wrapper costs are charged. Scripted tool choices measure execution/compatibility, not model autonomous repair.

## E28 resident streaming with an actual producer

E28 uses the existing exclusive-worker Stream API with1024-word resident history, tools empty. A prebuilt C process reads12 complete report units from a file, performs0 or40M actual iterations per unit, and flushes NDJSON immediately. Each unit is an immutable independent message, not an arbitrary cloud token fragment. Payload80/240 words×0/40M CPU work×wait_all/coalesced/eager×7 tasks =84 complete timing records;87 total with metadata/correctness/close.4 independent fresh references validate final token IDs/greedy. Mode order rotates by rep, batch512/min128, no synthetic sleep or network gap, end immediately after final payload. Same command/content/CPU work within a scenario; no snapshots, forced evictions or build optimization.

Capture producer start/spawn/reap and CPU, per-unit produced/received timestamps, actual input-end/model-ready/first-greedy, cumulative render/tokenize/prefill, input tokens/decode calls, backlog/backpressure. Task first-token time starts before producer launch. After-input waiting is separate. Complete task includes normal Stream close and common bookkeeping: task_complete_ms = task_complete_wall_ms − measured post-first-token oracle_ms. The first greedy is sampled before canonical/full-token checks. Fresh reference prefill runs after normal Stream close, so it neither requires a second simultaneous large KV nor pollutes TTFT/complete task; fresh_reference_ms is separately recorded. Model/initial-history and recorded helper cold/warm setup are excluded, not passed off as algorithm speedup. Producer/GPU contention and extra small batches remain charged.

The initial sanitizer collector attempted a fresh reference while the large receiving KV was still live, exceeding the shared8192 pool after9 completed sanitizer tasks. The failed artifacts/hashes are retained as a validation setup failure and excluded from performance. Moving the oracle after normal close changes no workload, capacity or active-task eviction policy. Final collector ASan passes; production Native/Stream code is unchanged from the full Native14K/existing Stream qualification, with shared source hashes checked; default regressions pass. Production defaults/APIs remain unchanged.

CPU work affects the emitted numeric digest:0/40M cases have1126/1222 new tokens at80 words and3046/3142 at240 words. Only within-scenario mode comparisons are controlled. Do not subtract CPU regimes or E27/E28/E26 runs from each other, or sum independent median stage differences. Seven pairs do not establish tail latency or energy/FLOP savings. All burst negatives, producer slowdowns, extra execution and indexed arrivals remain in the CSV/evidence. These are real local-process semantic-unit timings, not vendor cloud SSE, automatic agentctl routing, combined Native/Stream ownership or autonomous-model quality results.

## E29 actual agentctl output prefill (2026-10-31)

Actual `agentctl` REPL/parser/registry/AgentStore/test.run, resident Native session. The test harness only substitutes model **choices** with three Native-parser test calls then DONE; four real template/prefill/first-greedy operations run per task. Each tool executes the same prebuilt helper once, eight plain lines at32/56 words,0/40M rotate/xor iterations per line, no sleeps/builds. One REPL/model process switches off/on in rotating order. Each case/mode has one complete warmup followed by7 evaluation tasks:64 tasks total,8 warmup,56 evaluated,192 tool calls (168 evaluated).515 native rows =256 model-step +192 tool +64 task +3 status. First helper execution and warm execution are outside task timers and recorded.

Both explicit modes use identical body-first/status-tail receipts and identical input LF normalization. Legacy CLI without the flag is unchanged. Stdout/stderr retains at most4096 bytes, latest-only coalescing at512 new bytes, prefill budget512/min128/non-final flush=false. Same canonical session, old verified checkpoint remains resident; final full render/prefix hash and bounded suffix LCP validate adoption. No candidate fork, snapshot, eviction or restore; physical8192-token KV remains896MiB. Queue/worker copies and suffix tokenization are bounded scratch, not a second full history. There is no measured RSS win.

Task wall is the actual CLI tick loop through tools, receipt assembly, Native preparation/first greedy/actions, owner coordination and numeric instrumentation. Per-task ordinary session reset/admission, model load and helper first-exec warmup are excluded. Independent full template/tokenizer oracle runs after each sampled first token; its exact measured cost is subtracted from task wall, with raw wall and exclusions retained. Four oracles per task,256 total, are correctness checks rather than independent latency samples.13-case unit acceptance additionally checks fresh-KV greedy, UTF8/BPE/empty/truncated/NUL output, nonzero exit, timeout, capacity-before-execution, cancel/drain/reset, changed environment and legacy receipts. ASan CLI16 tasks and Native14357-token/original Stream/default C/HTTP regressions precede performance. Precompiled upstream libraries are uninstrumented.

CSV keeps all phase times, join, adopted/discarded/replayed tokens and paired differences. Positive saving = off−on. Paired medians must not be reconstructed by summing separate medians.32/56-word CPU-paced task savings95.254/183.960ms (7/7 each); burst−43.004/−20.590ms (0/7 and2/7). Added prefill execution46.853–169.079ms per three-tool task, and paced tool wall increases21.597/97.364ms. Large burst join time is mostly actual unfinished prefill, not evidence of pure scheduling overhead. More BPE-tail evaluations (median1–3/task) and repeated suffix tokenization are charged. These measurements do not identify a GPU kernel/frequency cause for the whole execution difference. No threshold tuning on evaluation; no change to default off behavior.

Initial qualification artifacts retain a sandbox Metal failure and a CLI collector assertion (executed includes4 model steps plus3 tools). The initial REPL also revealed first-user trailing LF invalidation; both final modes normalize before all timing. Single production0.6B probe returns Native format error rc7 with zero actual tools; keep this ability failure separate from the fixed-choice execution-chain test. Full stdout/private logs and measured-source ZIP remain local; public evidence has safe hashes/numbers. E29 does not claim full autonomous generation latency, cloud SSE, general tools/multi-consumer policy, Windows/CUDA or E26 build savings.

```sh
# Full source checkout and matching pinned SDK/runtime/model are needed for rerun.
python3 tools/build_agent_native.py --build-tool-output-cli --out build/tool-output-rerun
python3 tools/bench_tool_output.py --out build/tool-output-rerun --runtime /path/to/runtime --model /path/to/model.gguf
# Correctness first, separately from performance:
python3 tools/build_agent_native.py --test-tool-output --test-long-context --test-stream --sanitize --out build/output-qualification-rerun
python3 tools/bench_tool_output.py --out build/output-qualification-rerun --runtime /path/to/runtime --model /path/to/model.gguf --repeats 1 --correctness-only
```

## E30 observation gate and new workload

Same actual CLI collector as E29, with40/48 words per line and0/30M actual iterations per line. One warmup then7 off/on pairs per cell,515 rows. Freeze10ms first-output observation before evaluation; skip queued work after completion. ASan unit13 cases and16 actual CLI tasks precede unsanitized timing. Default C path and Native core are unchanged; E29 full regressions remain historical validation, not rerun claims. Measured source archive and runtime hashes retained. Full task, prefill, frontend, join and wrapper-minus-join reported; last quantity includes owner/setup work and cannot be called pure thread overhead. No tokenizer-only or thread-only ablation; different E29 workload precludes causal cross-version subtraction. Negative pairs are retained, no evaluation tuning.

## E31/E32 matched isolation and growth gate

E31:40/48 words and0/30M iterations per line, five modes off/old/on/thread/token. Same binary and producer, rotating modes per replicate,1 warmup +7 evaluated tasks/cell;160 tasks/1283 rows (20 warmup,140 evaluation). old reproduces E29 queue processing; on retains E30 first10ms wait and completion cutoff. thread processes the old queue and owner no-op; token adds suffix construction and tokenization without canonical/KV mutation. Effects on the whole task include interference and scheduling, so paired medians cannot be summed or named pure creation/tokenizer costs. ASan13 unit cases and40 real CLI tasks passed before timing.

E32:freeze progress gate after E31 diagnosis,36/52 words and0/25M iterations; off/on/progress each1 warmup+7 eval;96 tasks/771 rows (12 warmup,84 eval). Requires an output callback after at least10ms from first nonempty output, never a timer alone or final callback.512-byte coalescing and finished-tool queue cutoff remain. ASan13 unit cases and24 CLI tasks precede timing. Independent final tokens, receipts and greedy are identical for all modes; no evaluation tuning. Tools remain real, model choices scripted. Native core/previous Stream checks are historical unless explicitly rerun.

The exact E30 worst pair is retained:54.902ms regression=14.659ms additional tool wall+41.046ms post-tool wait plus a small remaining difference. Extra prefill37.933ms, streaming frontend0.918ms. Its third call had only one full-body observation and the final callback yet evaluated452 speculative tokens; timer expiration can therefore start useless burst work while the executor still waits for process reaping. Source uses10ms polling after EOF. This is a coordination race, not evidence that the repeated tokenizer took55ms. No OS scheduler/GPU-frequency trace was captured.

Offline matched contrasts are recomputed by `analyze_output_isolation.py`; exact source archives for both revisions are private. E31/E32 raw stages are also indexed in `data/tool-output-isolation.csv`. All old evidence remains immutable. Current full-task raw wall/oracle exclusions, identical command, no build optimization and resident KV definitions follow E29. No physical KV shrink or autonomous0.6B capability claim.

## E33/E34 · producer、回收与线程归因

E33在36/52词、0/2500万循环的同负载下轮换off/on/progress/thread，各1预热＋7评价，1027记录。E34冻结后换44/50词、0/2000万循环，比较off/on/progress及各自的`-event`版本，各1预热＋7评价，1539记录。全部为实际agentctl/registry/AgentStore/test.run与Native prepare/首token；模型动作固定，不是0.6B自主任务能力分数。模型常驻，不保存或逐出KV。

新增parent单调时钟：spawn、首次输出、EOF、waitpid回收、实际EOF等待、非最终输出回调、owner排队与执行。producer以同源CLOCK_MONOTONIC_RAW记录main开始、首次/末次flush、main结束；CLOCK_PROCESS_CPUTIME_ID记录自身CPU。数值sidecar在main结束时间戳之后写入，所有组相同；因此`after_producer_ms`含观测文件IO、进程退出、管道排空与父调度，不能视为纯OS退出开销。工具墙钟精确拆成启动至main、producer墙钟和main结束至工具返回；每条任务校验三部分之和。CPU与墙钟之差只描述非CPU时间，不把它直接称作IO；时钟量化可有微小负值，保留原值。

`eof_poll_ms`在原策略指10ms poll的实际等待；在event模式指EOF退出等待，包括kqueue等待或失败后的poll回退，名称为历史兼容保留。每次等待仍有10ms上限以检查timeout。macOS收到EOF且child未reap才惰性创建kqueue/NOTE_EXIT，waitpid仍是唯一回收者；无事件能力平台保持原poll，未做Windows/CUDA验收。默认策略未切换。

配对以同case、rep对齐，报告baseline−candidate，正数为节省；独立分量中位数不能相加重建任务中位差。thread控制含回调、线程启动、队列和owner往返；worker排队并不涵盖所有调度争用。无硬件频率、带宽或OS调度计数器，不能把每一毫秒归因到它们。E32没有这些新观测，E33只能验证机制与复现性，不能追溯证明旧单对的确切成因。保留启动、后续推理波动及全部负对；7次重复不估计p95/p99。

先执行sanitizer与CLI正确性，后单独执行unsanitized性能；测量期间不并发编译或测试。归因脚本为`tools/analyze_output_reap.py`，任务三阶段与逐对数据分别见`data/tool-output-process-stages.csv`和`data/tool-output-process-pairs.csv`；所有工具原始时间戳在证据rows中。每个正式目录保存独立measured-source.zip与源码/模型/运行库哈希，历史实测不重新标成当前源码。

复跑需新的空输出目录（不得覆盖正式目录），先构建，再顺序运行：

```sh
python3 tools/build_agent_native.py --sdk "$LLAMA_SDK" --runtime "$LLAMA_RUNTIME" --model "$MODEL_GGUF" --out build/output-event-new --build-tool-output-cli
python3 tools/bench_tool_output.py --out build/output-event-new --runtime "$LLAMA_RUNTIME" --model "$MODEL_GGUF" --words 44 50 --iterations 20000000 --modes off on progress off-event on-event progress-event
```

E33使用同命令、`--words 36 52 --iterations 25000000 --modes off on progress thread`；精确历史复现应使用对应私有源码归档，当前源码的其他验收不等于历史重跑。功能检查使用独立目录的`--test-tool-output --sanitize`构建，再收集`--repeats 1 --correctness-only`，不纳入性能汇总。

### E35退出竞态补充（保留E34）

E34的可选事件等待不是最终版：首次100次独立退出探针中10次长等待均记录注册ESRCH、未收到事件，随后原10ms回退。E35仅对ESRCH或已经收到NOTE_EXIT的退出阶段改为1ms有界重试，保留全部老策略和失败回退。最终独占200次交替探针两组都没有实际EOF等待，这个未复现结果也保留，不宣称探针证明总体提速；正式LLM对照另测。

E35使用46/54词、0/1500万循环，off/off-event/progress-event/off-race/progress-race五组各1预热＋7评价，共1283主记录；退出探针为另外的诊断附件，不混入主任务样本数。SAN功能组40任务，性能组160任务（20预热／140评价），每任务3次真实工具。命令沿用上文，改为`--words 46 54 --iterations 15000000 --modes off off-event progress-event off-race progress-race`。探针源码`tools/probe_output_exit.c`需本仓库C执行器；编译为独立程序，不调用LLM。初始100次、开发期200次、最终独占200次原记录分开保存，开发期有并发构建，不作为性能成绩。

## E36 · 同负载线程／工具／后续推理隔离

46/54词、0/1500万循环，off-race、idle-race（消费者休眠、不接输出）、thread-race（相同增长门槛、队列、owner空操作）、token-race（只分词）、progress-race（真实预填充）；各1预热＋7评价，1283主记录。全组使用同一退出竞态修正、同一预构建producer与相同收据。轮换顺序，KV常驻，真实CLI的四次Native准备／首greedy和三次工具执行。额外观测四次prepare的本进程CPU、owner线程CPU、decode调用数；现有token/render/prefill与producer CPU／wall继续保留。

进程CPU来自getrusage(RUSAGE_SELF)，含并行worker且可能大于wall；owner CPU来自CLOCK_THREAD_CPUTIME_ID。两者都不是GPU执行时间，不能用wall−processCPU称作阻塞。CPU观测共同计入正式任务，post-token独立oracle仍原样单列排除。未采GPU硬件计数器，时间变化、计算量变化、直接线程排队与间接争用分别表述；相同token/decode数下的prefill墙钟突增只能定位执行阶段，不直接证明特定GPU／频率原因。组间中位差不当作可相加的纯部件成本。

功能组使用独立sanitizer目录、每case每mode1预热＋1评价，共40个真实CLI任务。正式性能单独运行，不并发编译或验收。源码归档与负结果追加，E35等历史证据保持不变。统计脚本`tools/analyze_output_contention.py`从主记录重算任务阶段／CPU／decode配对，关闭基线选off-race。

## E37 · 新负载按需消费者验证

冻结42/55词、0/1800万循环，off-race/progress-race/lazy-race各1预热＋7评价，771主记录。门槛仍真实增长10ms＋512字节，不在评价阶段调参；lazy只在首次合格非最终输出触发线程构造。新增每工具consumer_threads和consumer_launch_ms；构造时间可能从工具前移入读取回调，不能只看pretool下降，须看全任务、callback和producer。

SAN22场景同时覆盖原／按需版非零退出、超时、截断、Unicode/NUL、最终fresh-KV greedy及取消前置路径，另24个真实CLI sanitizer任务；没有注入线程构造OOM。正式96任务（12预热／84评价），3次工具／任务。对照结果为负也保留：突发零消费者只证明减少资源操作，不表示性能改善。CPU与decode定义沿用E36；所有阶段、逐对数据在`tool-output-contention-*`三张CSV中，图示范围不当作置信区间。

复跑采用新的空目录构建`--build-tool-output-cli`后运行：

```sh
python3 tools/bench_tool_output.py --out build/output-contention-new --runtime "$LLAMA_RUNTIME" --model "$MODEL_GGUF" --words 46 54 --iterations 15000000 --modes off-race idle-race thread-race token-race progress-race
python3 tools/bench_tool_output.py --out build/output-lazy-new --runtime "$LLAMA_RUNTIME" --model "$MODEL_GGUF" --words 42 55 --iterations 18000000 --modes off-race progress-race lazy-race
```

两个目录应分别构建，不复用已有evidence／workspace；性能期间不并发构建／测试。sanitizer先用独立目录`--test-tool-output --sanitize`，收集`--repeats 1 --correctness-only`；历史源码按各自measured-source.zip和哈希识别，不将当前代码当作旧代码复测。

计数说明：token-race的`updates`计只分词诊断调用，`evaluated_tokens`和`prefill_ms`均为零，不表示canonical/KV改动。图表柱为独立组中位数；逐对差的中位数可能显示不同排序，结论与胜率按逐对数据，不能用柱高直接相减代替。

## E38/E39 · decode／同步、Mach计数与独立producer

E38：42/55词、0/1800万循环，off-race/idle-race/thread-race/progress-race。E39：新43/53词、0/1600万循环，off-race/progress-race/off-drain/progress-drain。均每case每mode1预热＋7评价，1027条CLI主记录；每case/rep在CLI等待REPL时另外执行3次producer-alone，96条记录，共1123条／实验。独立producer的stdout丢到/dev/null，CLI为管道，因此它用于观察同期计算／调度背景，不能据不同父进程或输出目标比较启动／IO收益。全部fixture预构建，常驻KV，无快照／逐出，正式性能不并发构建或测试。

private owner-only trace默认关闭，测试harness启用，分别累计llama_decode调用wall/owner CPU和显式llama_synchronize的wall/owner CPU及次数。decode可能内部等待，sync是等待剩余工作与host唤醒／调度的跨度，不是纯GPU核函数时长；最终prepare和流式工具窗口分列，decode＋sync须与整体prefill／任务共同看。E39只在同一次prefill循环内部延迟显式host屏障，循环完成、预算耗尽、不足min停止及错误返回前排空，greedy重放即时同步；公开API仍同步ready，不跨调用留pending工作。默认strict。

producer把setup、8次计算循环与输出flush分别计时；主结束仍在数值sidecar写入前，尾部IO不能误记作OS回收。CLOCK_PROCESS_CPUTIME_ID仍保留。macOS proc_pid_rusage返回runnable/CPU原始Mach计数，本机timebase125/3，须先换算；`tools/probe_schedule_clock.c`用100ms CPU忙循环和100ms sleep校验，两个实际探针作为诊断元数据，不重复计入主任务样本。runnable包含正在运行，ready代理=(runnable−procCPU)×timebase，blocked代理=wall−runnable×timebase；计数更新滞后可产生负值，原样保留，不强制归零或精确分配所有非CPU时间。上下文切换只计次数，不推断具体竞争PID；instruction/cycle为CPU计数，不是GPU时间或频率直接读数。算术producer不代表内存／带宽密集工具。

E38/E39分别22／30原生sanitizer场景、32／32真实CLI sanitizer任务，另24／24独立producer控制。当前E39源码以`CXF_TEST_DEFER_SYNC=1`验收14,357-token Native，完整Stream以strict默认回归；同时默认C/HTTP/CLI回归通过。这些是E39当前源码验收，不将长上下文成绩重新贴到E38实测源码。没有人为注入decode失败或线程构造OOM；CPU ready代理和GPU设备排队仍有因果边界，不估计p95/p99。

同负载按case/rep配对，正数为baseline−candidate；独立分量中位数不可相加。保留所有负对、producer-alone和计数误差。原始／衍生数据见`decode-schedule-{tasks,pairs,producers,phases}.csv`，`tools/analyze_decode_schedule.py`重算及校验；源代码ZIP、模型／库哈希按实验独立留存。CPU忙／睡计量probe两条在E38/E39元数据中重复引用同一实际采样，不算四次独立试验。

复跑先用新空目录构建`--build-tool-output-cli`，再顺序运行（须固定b10938 SDK／runtime和模型）：

```sh
python3 tools/bench_tool_output.py --out build/output-decode-new --runtime "$LLAMA_RUNTIME" --model "$MODEL_GGUF" --words 42 55 --iterations 18000000 --modes off-race idle-race thread-race progress-race --producer-alone
python3 tools/bench_tool_output.py --out build/output-drain-new --runtime "$LLAMA_RUNTIME" --model "$MODEL_GGUF" --words 43 53 --iterations 16000000 --modes off-race progress-race off-drain progress-drain --producer-alone
```

两个目录应分别构建；SAN使用独立目录`--test-tool-output --sanitize`，再用相同case/mode参数并加`--repeats 1 --correctness-only`，不纳入性能成绩。初次private头文件路径编译失败留在inventory，正式数据来自修正后的final2／qualified2目录。

解释边界参照固定版本[Metal同步代码](https://github.com/ggml-org/llama.cpp/blob/f1e44dcc11d8802d107bd7331a3d3fd3e6f57b93/ggml/src/ggml-metal/ggml-metal-context.m)、[XNU调度计时](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/kern/sched_prim.c)及[task计数汇总](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/kern/task.c)；系统背景调度存在是计数／独立控制的推断，不表示已找出所有历史负对的竞争源。

## E40/E41 · 竞争源四状态控制

E38的producer-alone仍有存活模型，所以不能直接排除引擎后台CPU。补43/53词、1600万循环的模型加载前、fixture完成后立即、100ms静置后、模型退出后四状态，每状态3次producer，各1预热＋7评价；E40 fixture关闭预填充，E41启用progress-race增长预填充。每轮192条producer、16条fixture验收与3条状态，共211条。fixture实际执行4次Native／3次工具并校验最终canonical／greedy；它的模型加载、任务耗时、100ms settle和退出不纳入producer窗口。

所有四状态producer stdout都/dev/null，同一预构建程序与字节输出。模型存活时，父采集器在producer子进程前后读取自有模型PID的proc user/system原始ticks，按相同Mach timebase换算；时间窗口包含spawn／reap，稍宽于producer main。公开记录没有PID，也没有其他进程列表；父采集器CPU另记，防止把采集开销称作模型CPU。模型不存活时model_cpu_ms＝0表示没有该模型进程，不是对未知PID取样。

固定顺序由before/after absent夹住resident；CSV也给出三次producer合计的resident相对前后absent平均差，但它不是已校正时间漂移的因果估计。E40／E41跨轮背景不同，不跨轮相减声称策略收益。模型CPU很少能排查自身残留CPU忙转，不能排除GPU／驱动、缓存／热状态历史效应。代码是`tools/probe_model_idle.py`，统计／检查为`tools/analyze_model_idle.py`，图及`model-idle-{controls,summary,pairs}.csv`同时保留第一次未复现、第二次复现和所有负ready代理。

复跑无需重新编译：使用已经过E39验收的二进制目录，输出选新的空目录。默认初始化off-race；`--warm-prefill`只改变fixture初始化为progress-race，控制程序不做新的运行时优化。

```sh
python3 tools/probe_model_idle.py --out build/model-idle-new --binary-dir build/output-drain-new --runtime "$LLAMA_RUNTIME" --model "$MODEL_GGUF"
python3 tools/probe_model_idle.py --out build/model-idle-stream-new --binary-dir build/output-drain-new --runtime "$LLAMA_RUNTIME" --model "$MODEL_GGUF" --warm-prefill
```

该二进制目录须有独立bench运行产生的manifest与measured-source.zip，collector记录其源码和二进制哈希。E40的旧collector在加入`--warm-prefill`参数前，原版本source ZIP独立留存；当前默认off语义兼容，精确历史复现仍应按归档哈希。两个诊断共32个模型fixture／96次工具／384个producer控制，不能当作384个额外自主任务或通用IO／内存工具基准。

## E42–E44 Metal时间线与有限尾批合并

E42在专用实际CLI harness中用进程内ObjC runtime拦截公开`commit`／`waitUntilCompleted`，生产CLI不链接observer，固定b10938运行时dylib不修改。每个Native准备和实际工具窗口有独立scope，独立tokenizer／fresh greedy参考放在scope外。取Mach绝对时间及相同系统基准的Metal时间戳。原始`metal_command`／`metal_wait`放在证据的`metal_commands`，不混入1539条主CLI记录；附加记录数单列。导出CSV保留scope、API phase、匿名本进程queue编号、commit入口／返回、CPU driver调度、GPU执行跨度及wait入口／返回。

Apple的[GPUStartTime](https://developer.apple.com/documentation/metal/mtlcommandbuffer/gpustarttime?language=objc)／GPUEndTime表示command buffer的GPU执行起止；[kernelStartTime](https://developer.apple.com/documentation/metal/mtlcommandbuffer/kernelstarttime?language=objc)是CPU驱动调度，不是shader核执行时间。[waitUntilCompleted](https://developer.apple.com/documentation/metal/mtlcommandbuffer/waituntilcompleted%28%29?language=objc)还等待completion handlers返回。GPU跨度含命令内同步／访存及潜在设备抢占，不称纯shader忙时。

多个buffer的GPU区间、commit→GPUStart区间均取并集；后一项再计算与本scope已观察GPU执行的交集，并列同queue可见前序执行。`queue_no_own_gpu_ms`只是排除已知本进程GPU跨度后的提交延迟，仍混合driver、未捕获工作、其他应用／设备排队和调度，不是精确的独占队列计数。driver交集与其他项可重叠，不能跨列相加。

逐wait相邻分解为：可见GPU重叠、GPU结束前未解释间隔、GPU结束后返回上界、入口时GPU已结束的晚检查，以及缺失时间戳。decode和显式sync分别记账，避免将等待转移误当加速。E43另加可关闭的最小completion marker，每buffer只写进入／退出时间；GPUend→callback进入是通知交付／callback调度上界，callback退出→wait返回仍混合Metal协调及等待线程恢复。不能把任意一个差值称纯CPU内核唤醒延迟。记录marker覆盖和缺失数，缺失不补零当观测。

E42保持43／53词、0／1600万producer循环，对比strict、deferred、growth及同二进制bare（关闭observer）组；E43同负载独立对比无marker、marker与bare。bare仍保留方法转调wrapper和共同的decode／sync CPU计量，不能称完全无插桩原始二进制。observer在scope末尾短时retain／读时间戳／写私有JSONL；完整task计量包含其开销。同组旋转次序、1预热＋7评价，48个sanitizer任务与30个单元场景分别验收后才做无sanitizer性能。禁止与构建、其他测试或我们启动的GPU任务并发；系统其他应用负载不受控，极端值全部保留，7对不产生p95／p99。

E44使用未测过45／51词、0／1400万循环，同一1024 batch容量引擎比较cap512分块与至多1024有限尾批合并，均strict。4个bare组比较关闭／增长工具输出×split／coalesce，2个observer组直接比较GPU跨度。预算不足以合并时继续512分块，工具流式预算512保持不变；`--llama-batch`默认512，策略默认关闭。1024容量的计算缓冲区代价单列，物理KV不变，不能把批量优化记作KV占用减少或与旧512容量实验相减。

重跑时使用新目录，先`tools/build_agent_native.py --sanitize --test-tool-output --build-tool-output-cli --metal-probe --out NEW_QUALIFIED`，再对该目录运行`tools/bench_tool_output.py --correctness-only --repeats 1 --metal-probe`及对应modes。正式构建去掉sanitize并使用另一个新目录；E43模式见D-PREP §31，E44另加`--batch-tokens 1024 --words 45 51 --iterations 14000000`。所有真实模型路径由本机参数提供，不进入公开包。

CSV中`notify_registered=false`／通知覆盖为0的记录没有marker观测；对应0占位不能计作零延迟。`metal-commands.csv`只放command，`metal-waits.csv`将wait关联到原主记录、mode／phase／rep与同buffer marker，并按该scope所有可见GPU区间分解；`metal-scopes.csv`保留每窗口完整并集与覆盖数。E42 strict关闭组单wait host返回尾部最高14.006ms，所有带trace模式最高57.657ms；E43 43词增长突发某单wait的16.376ms尾部中，10.728ms在GPU结束→marker进入，5.648ms在marker退出→wait返回。后者依然不是直接内核级wakeup计数。

## E45–E46 持续尾延迟与容量对照

E45用E44相同45／51词、0／1400万循环和1024容量重测split／coalesce，各有bare、无marker trace、通知marker三种路径，1539主记录。旧86ms bare不能追溯补出GPU时间，E45只解释新采集负对。按四次prepare分别计算GPU跨度、显式sync GPU重叠、GPU结束前未解释间隔、GPUend→marker、marker退出→wait返回、sync非Metal host以及decode／模板阶段。队列与GPU区间交集先扣除已知重叠，driver调度另列；这些是API跨度，不能代替系统内核wakeup／全设备队列追踪。

E46冻结候选640，另保留真正默认512和旧扩容1024，未用评价数据再选容量。依据[固定b10938源码](https://github.com/ggml-org/llama.cpp/blob/f1e44dcc11d8802d107bd7331a3d3fd3e6f57b93/src/llama-context.cpp#L596)，sched_reserve按n_ubatch建立最大图；cxf已有batch参数同时设置n_batch／n_ubatch。因此可直接减少已有启动参数，无新context、无动态重分配、无修改llama运行时。初始化Metal／CPU计算buffer及KV预留按每次加载日志提取，不称RSS或实时物理占用。

`tools/bench_batch_capacity.py`按7个block旋转512／640／1024容量，每容量一独立进程，子进程内关闭bare、增长bare、关闭notify三模式，47／54词×0／1200万循环，每负载／模式1warm＋1eval。模式顺序也按block旋转、奇数block倒转词数顺序，共21模型进程、252评价＋252warm任务、4095主记录。模型加载、helper首次执行、warm独立列出，不算评价延迟；每任务仍真实三工具／四Native并KV常驻。跨容量比较同block／同负载，须同canonical、transcript和首greedy；重开成本不等于零，不能将此方法当在线换容量方案。

SAN容量检查仅1 block，共72任务和585主记录；通过后独占无SAN运行7 block。工具输出相同、无构建／睡眠注入，负对及原始CPU runnable计数保留。notify是单独的有扰动诊断组；端到端结论用bare，不用它与bare的差当纯通知成本。512／640／1024的配置差包括分块与容量，不将它们误当同容量纯算法因果对照，E44已有同容量隔离。

重跑先用`tools/build_agent_native.py --build-tool-output-cli --metal-probe`建立新目录，再用`tools/bench_batch_capacity.py --build BUILD --out NEW --runtime RUNTIME --model MODEL`；SAN构建加`--sanitize`，容量验收加`--blocks 1 --correctness-only`。性能源文件冻结，build／测试／性能不得并发；`--resume`只复用同out已完成且校验通过的子实验，不覆盖旧正式证据。

E46中的512是默认容量控制组；各组仍处于同一显式Native工具输出profile，使用共同的退出race修正及状态尾收据。它不代表默认HTTP或旧收据路径的整体生产基线。evaluated token数量相同不代表FLOPs严格相等：分块还会改变中间logits输出等实际工作；GPU buffer跨度也不是纯shader忙时。

## E47–E49 同容量／同shape争用隔离

E47固定640容量、同cap512/尾批合并，将关闭、仅owner空操作、仅分词、真实growth在同一producer负载下比较；关闭与真实growth另有notify路径。47／54词、0／1200万循环，每模式1warm＋7eval，1539主记录。仅线程／仅分词的最终prepare也用相同分块，避免把旧控制组的容量／初始chunk差当线程成本。bare用于端到端，notify提供设备区间，不能跨这两种插桩路径作纯耗时相减。

私有batch observer记录decode position／tokens及buffer关联batch；生产hook为空，专用harness开启；原始`metal_batch`与`metal_command`／wait分列。batch ID表示一次cxf decode，不保证只有一个GPU buffer或kernel。新CSV逐批保留scope、主记录索引、逻辑token位置、数量、GPU区间并集、通知／返回及队列残差；主记录数与Metal附加记录数分别统计。

E48独立CAPI固定increment，容量640、context8192、只读root保留2048已计算token；fork的逻辑prefix为768／2048。**root在768条件仍保留2048物理prefix cells**，这是受控共享布局，不是生产单会话768-token的精确物理KV布局。只在同prefix／increment内解释压力与split／合并差；不把两prefix差拿去外推实际模型上下文缩放。每条件128／224／512／582新增token，split512或coalesce640，空闲线程／算术线程／64MiB缓存行读写线程，1warm＋7eval，384样本＋3状态。CPU压力是实验内部自有线程，其他自有测试／构建不并发。

所有条件都预分配同一64MiB buffer；线程创建、初始化、启动握手在计时外，计时只覆盖prefill到设备同步，停止／join与写trace在计时外。worker CPU/wall窗口稍宽；逻辑读写字节不等于实际DRAM流量。算术与内存压力指令也不同，所以这是内存路径敏感性对照，不能由差值直接推导物理带宽或证明旧217ms的原因。每shape独立fresh-KV greedy与全部canonical token验证在计时外，384样本均记录结果。

本M5的公开Metal能力探针仅stage-boundary timestamp，dispatch/draw/blit sampling均false，没有带宽／占用counter。参见[Apple counter sampling](https://developer.apple.com/documentation/metal/sampling-gpu-data-into-counter-sample-buffers)及[dispatch boundary](https://developer.apple.com/documentation/metal/mtlcountersamplingpoint/atdispatchboundary)。没有用GPUStart/End冒充纯shader忙时，也未采GPU频率、设备全队列或内核preemption。Instruments模板清单首次因sandbox缓存权限失败，提升权限后读取成功；未采其他进程或系统trace。

E49根据独立E47/E48观察，先冻结非final发布量子1536字节（旧512），再对新49／55词、0／1000万循环测关闭／旧growth／新coarse，各bare及notify共六模式、1＋7重复、1539主记录。观察窗10ms、预算512、最低128、容量640、BPE与最终校验不变。更晚启动可能损失重叠窗口，负结果不删除；默认保持512容量及预填充off。E49评价期间不再调整门槛。72原生SAN场景／48真实CLI SAN任务与最终14K／Stream／默认回归分别验收，功能样本不混入性能。
