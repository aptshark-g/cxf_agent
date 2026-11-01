# 适配与成果变更记录

## 首次 GitHub 发布（2026-11-01）

仓库所有者已明确授权将源码、文档、历史数据和图表上传至 `aptshark-g/cxf_agent`。本次发布按 438 文件源码白名单提交，保留远端只含 README 的初始化历史；密钥、本地配置、私有日志、模型／运行时、KV 快照、构建产物及独立项目继续排除。范围及校验见[发布指南](../RELEASING.md)。

推理实现与策略不变；50 份 evidence、35,520 主记录和 37 图保留原测量版本及负结果。上传前重新检查清单与暂存树哈希，不重写旧数据、不把发布当成新模型／性能验收。以下“GitHub 发布准备”保留为本轮上传授权之前的阶段记录。

## GitHub 发布准备（2026-11-01）

中英文首页改为目的、使用、实测与限制的导航；MIT 已按所有者选择加入。公开源码中的本机模型默认目录改为仓库内 `local-model/`，三份脚本显式 SDK／runtime／model 参数不变。旧设计里的实际端点和凭据来源已脱敏；示例只保留空 key、通用回环地址／占位符。

新增源码白名单、内容／链接／哈希校验和确定性导出；忽略本地配置、会话历史、模型／运行时、日志、快照与独立项目。详细范围和验收见[发布指南](../RELEASING.md)。本轮未改推理调度或实验策略，不新增 E 编号；50 份证据与原测量版本保持不变，35,520 主记录／37 图继续按历史实测解释，不标为当前源码重新测量。暂不提交或推送。

## E47–E49 归档结果（2026-10-31）

E47固定640容量、相同最终cap512／尾批合并，对关闭／仅线程／仅分词／真实growth做同负载控制（47/54词、0/1200万循环）。工具等待与GPU工作分开：仅线程没有工具期间GPU预填充，仍出现整任务多1868.877ms，其中工具多1817.950ms、producer墙钟多1761.856ms、自身CPU反少22.205ms、ready代理多1784.891ms；growth最差多392.829ms，工具多522.869ms、最终prefill反少334.860ms。这证明并发推理不是这些停顿的必要条件，不将其全归kernel效率或某个竞争PID。旧217ms bare仍不能追溯精确设备／通知归因。

私有逐批callback已接专用Metal harness，关联decode position／tokens与buffer，生产默认空hook，稳定C API和预算／同步语义不变。M5实测只支持stage timestamp、不支持dispatch sampling，也无公开带宽／占用counter；未采GPU频率或外部全设备调度。GPU区间仍含访存、内部停顿与抢占，不能称纯kernel忙时。

E48固定prefix768／2048、increment128／224／512／582，同shape比较空闲／算术／64MiB内存线程。582-token空闲合并GPU中位快7.869／13.458ms，负对保留；内存对算术压力使224／582-token合并的GPU跨度中位多约6.5–10.2ms，提交残差一般仅多0.02–0.19ms。说明内存路径竞争可体现在设备执行阶段，不是纯提交排队；不足以证明旧217ms全由内存或解释全部106ms设备尾部。root物理保留2048 cells，即使逻辑prefix768，不能从这组数据外推单会话上下文曲线；逻辑读写量不等于DRAM带宽。

E49先冻结更大输出发布量子1536字节（旧512），再测新49/55词、0/1000万循环。该优化尝试失败：新coarse相对旧growth，突发-3.567ms（2/7；最差-34.783ms）／-5.492ms（3/7；最差-47.290ms），持续-54.667ms（0/7；最差-75.754ms）／-3.009ms（1/7；最差-59.735ms）。带marker持续样本旧／新均42批，token从9240增至11970，GPU累计2518.927→3274.296ms，没有减少批次。较晚启动使每三工具任务join逐对中位多82.889／73.485ms，最终prefill虽少25.716／63.708ms，仍未抵消。默认保留旧门槛，coarse仅实验选项，不按这批评价继续调参。

旧growth对关闭在持续新任务仍快104.276ms（7/7；最差92.344ms）／67.290ms（7/7；最差56.796ms）；coarse对关闭仍快45.908ms（7/7；最差26.977ms）／65.197ms（7/7；最差6.830ms），这不能包装为对旧growth的改进。容量640的373.44MiB计算预留、896MiB物理KV保持，E49不额外施压、不做快照／逐出；默认容量仍512、预填充off。

E47为63个原生SAN场景／48真实CLI任务，E48为96个SAN样本／8个fresh shape，E49为72场景／48 CLI任务及最终640容量14,357-token Native、strict Stream、默认C/HTTP/CLI回归。三轮主记录1539／387／1539，现50份证据／35520主记录／37图，Metal附加记录共23465条（command／wait／batch分列）；主记录含warm／状态。旧47份证据保持原样，失败试验、负对与不能归因部分全部保留。[D-PREP §33](../design/prewarm-v1.md)、[完整数据](README.zh-CN.md)。未推送GitHub。


## E45/E46 归档结果（2026-10-31）

旧E44的86.283ms bare负对无法追溯GPU／通知起止；多93.325ms prefill分散在四次prepare（22.717／11.694／32.119／26.795ms），每次owner CPU反而下降。E45同负载split／coalesce×bare／无marker／marker重测，没有复现86ms。持续bare最差慢1.908ms（工具＋3.871ms、prefill－2.058ms）；无marker trace最差慢11.248ms（工具＋14.212ms、prefill－3.054ms）；marker最差慢1.157ms，通知交付／marker后返回反少0.180／0.043ms。新负对可定位到对应阶段，但旧尾延迟仍未精确归因，不宣称已消除系统回退。45词持续的GPU跨度收益只有约2.5ms，合批不是各节奏都稳定获益。

计算缓冲区膨胀来自既有n_batch／n_ubatch耦合，llama.cpp按微批容量预留最大图。E46复用现有`--llama-batch 640`，较大初始输入继续分块、约600-token工具增量可合并，不改推理核心／固定runtime、不新增context／动态扩容。Metal计算预留373.44MiB，对比1024容量597.50MiB减少224.06MiB（37.5%）；只比默认512多74.69MiB，消除了原额外预留的75%。CPU计算24.03→15.02MiB，物理KV仍896MiB。这是初始化预留，不能称峰值RSS或KV下降。

E46新47／54词、0／1200万循环，7个block旋转三容量与模式、奇数block倒转词数顺序；21次模型加载、252评价＋252warm实际CLI任务。640关闭流式对默认512的任务收益：突发15.664ms（6/7；最差-9.195ms）／2.217ms（4/7；最差-176.227ms）；持续35.721ms（6/7；最差-12.543ms）／-2.180ms（3/7；最差-62.071ms）。640增长流式对默认512增长策略：突发27.527ms（5/7；最差-2.404ms）／-1.849ms（3/7；最差-168.364ms）；持续-24.934ms（1/7；最差-48.957ms）／-12.957ms（2/7；最差-217.550ms）。640关闭流式对旧1024：突发-6.090ms（3/7；最差-121.219ms）／-45.389ms（2/7；最差-156.310ms）；持续-8.158ms（3/7；最差-56.075ms）／-17.900ms（2/7；最差-111.176ms）。正值为640更快，负值为回退，全部保留；不能把重开模型的加载成本当在线换容量免费，容量不是只影响分块的单变量。

默认容量仍512、预填充仍off，640为显式可选配置。E45有47原生sanitizer场景／48实际CLI sanitizer任务，E46三容量72实际CLI sanitizer任务、640容量14,357-token Native、strict Stream和默认C/HTTP/CLI回归通过；性能无SAN且顺序独占。该阶段47份证据／32055主记录／34图，Metal附加记录共16477条；主记录含warm／状态，不能当独立延迟样本。旧45份证据保持原样，负结果和未复现均保留。[D-PREP §32](../design/prewarm-v1.md)、[完整数据](README.zh-CN.md)。未推送GitHub。


E46新负结果进一步区分：640增长持续组对默认512中位反慢24.934／12.957ms，最差217.550ms中工具多190.983ms、producer墙钟多150.844ms、自身CPU多12.507ms、ready代理多153.688ms，最终prefill多30.848ms。marker最差640对512慢136.408ms：最终prefill多107.952ms，GPU并集多106.174ms，通知交付／marker后返回反少0.320／0.073ms，未解释提交延迟仅多0.148ms。可将这次新prefill尾部定位到设备执行跨度；GPU核效率、内部停顿、设备调度或频率状态尚未进一步识别，不能倒推旧86ms。640只作为显式内存／速度取舍，不升为推荐默认。

## E42–E44 归档结果（2026-10-31）

专用测试harness补Metal逐command／wait时间线，固定b10938运行时不改。同步等待主要与GPU命令执行重叠，commit→GPUStart的大部分区间与自己前序GPU执行重叠，不能全叫设备排队；CPU driver调度、GPU执行跨度、通知交付和host返回分列。E42 strict关闭组单wait GPU结束→返回最高14.006ms，全部带trace模式最高57.657ms；E43另加可关闭completion marker，53词突发累计GPU重叠中位655.283ms、通知交付最高8.584ms、marker退出→wait返回最高1.329ms。E43另一个43词增长突发单wait的16.376ms尾部可分为通知交付10.728ms和marker后返回5.648ms。后两段仍含驱动／handler／系统调度，不称纯CPU唤醒；未定位其他GPU队列或竞争PID，不能追溯精确分解旧44.039ms负对。

按实际设备工作优化有限尾批：E42看到588 token拆512＋76，76-token尾批仍占约29–37ms。E44在新45／51词、0／1400万循环任务比较同一1024容量下split512与可选合并<=1024尾批，不超预算、strict同步、单canonical／resident KV。关闭流式的整任务合并收益（突发45／51、持续45／51）为36.407ms（7/7，最差31.282ms）／28.445ms（7/7，最差11.714ms）／-3.341ms（3/7，最差-86.283ms）／22.623ms（6/7，最差-14.723ms）；已开启增长流式时为42.533ms（7/7，最差21.956ms）／29.019ms（7/7，最差0.537ms）／9.051ms（4/7，最差-20.868ms）／35.338ms（6/7，最差-56.910ms），负值即回退。合并后增长流式相对关闭，持续为152.803ms（7/7，最差125.405ms）／181.509ms（7/7，最差118.886ms），突发为1.718ms（4/7，最差-20.275ms）／-2.125ms（2/7，最差-14.450ms），全部配对保留。

`--llama-batch`默认512；尾批合并仍显式诊断策略。容量1024的Metal计算缓冲区597.50MiB，对比旧512容量298.75MiB，CPU计算缓冲区24.03对12.01MiB；物理KV均896MiB。当前对照两组都用1024，隔离分块效果，不证明扩容本身划算，也不是KV内存减少。observer／marker只在测试harness，bare性能组关闭采集；observer开销及E42数秒级producer／host异常全部保留。

三轮各1539主记录，该阶段45份证据／26421主记录／32图；另有9575条Metal附加记录（7179 command、2393 wait和3条metadata），以及此前500退出探针与2条忙／睡计量探针。E42/E43各30原生sanitizer场景、E44为47，每轮48实际CLI sanitizer任务；最终E44源码通过14,357-token合并Native、strict Stream与默认C/HTTP/CLI回归。当前实现／历史源码哈希分别归档，旧42份证据逐字节未变。[D-PREP §31](../design/prewarm-v1.md)、[统一数据和负结果](README.zh-CN.md)。未推送GitHub。

E44剩余负对补充：45词持续关闭组最差反慢86.283ms，最终prefill多93.325ms（sync多84.062ms），工具墙钟反少20.539ms。该bare对没有设备trace，不能精确分配GPU／host原因。带probe的51词持续合并组GPU跨度少23.360ms、最终prefill少18.263ms，整任务却中位反慢11.240ms，工具墙钟多32.426ms；证明设备优化与工具／系统波动可以并存，不能宣布剩余回退已解决。


开发负结果另登记：初次ObjC Method别名冲突；后续重命名误改runtime方法名；weak_import未能链接普通CLI，改为每引擎显式空hook；E43 metadata转义编译失败。均保留原build目录，正式E42为final5、E43为final2；成功开发试跑和SAN也不混入性能。运行目录清单明确标记失败／被替代／验收。


## E40/E41 竞争源补充结果（2026-10-31）

增加模型加载前、任务完成后立即、100ms静置后、模型退出后的四状态控制；E40以关闭预填充任务初始化，E41以增长流式任务初始化。每轮192个producer控制、16个真实模型fixture和3条状态，共211条记录。32个fixture的canonical／greedy、96次实际工具调用及32个模型进程正常关闭均通过；不是额外自主模型能力成绩。

E40未复现大等待，模型CPU最高0.1324ms；E41复现：53词静置后producer ready代理70.635ms、墙钟184.853ms、自身CPU108.246ms、compute184.680ms、非自愿切换312次，同子进程窗口内模型CPU仅0.4223ms、采集器1.015ms。模型加载前也出现27.228ms ready代理。因此明显延迟可在模型不存在或几乎不消耗CPU时发生，系统侧CPU供给因素有交叉证据；这些样本不支持“模型后台线程忙转抢CPU”解释。没有定位其他竞争PID，GPU／驱动、缓存、热状态及CPU运行速率的历史影响仍未全面排除；固定顺序和跨轮背景漂移的限制保留。

该阶段42份证据／21804条主记录／29图；另有此前500退出探针及两条忙／睡计量探针。E38/E39的合并同步只有小幅端到端收益，绝大部分等待转移进decode，默认仍strict。完整报告与CSV保留所有负结果、第一轮未复现及raw负ready代理；未推送GitHub。

## E38/E39 归档结果（2026-10-31）

已把llama_decode调用和显式同步的wall／owner CPU拆开，加入producer compute/setup/output、Mach runnable／CPU计数、非自愿切换和同期独立producer对照。E38突发关闭组的最终同步中位519／669ms，ownerCPU不足0.4ms；本轮prefill最大减最小约22ms，主要随同步等待增加，旧71–110ms范围未复现。计时定位到等待已提交工作的阶段，尚未测Metal核函数起止，不能把全部等待称为GPU计算。

部分工具延迟已证实与系统背景CPU供给有关：模型空闲时，42词持续独立producer仍有56ms ready代理、非自愿切换中位790次。ready代理与非自愿切换相关，但runnable含运行态、计数有滞后，负代理值保留。55词独立背景较低，仍有较小模式相关变化；没有定位竞争PID，也未排除全部间接争用。M5原始proc计数须乘125/3，100ms忙／睡校验已单列。

新增显式off-drain／progress-drain，仅合并同次prefill内部屏障，公开返回前仍排空。E39新43/53词中位整任务收益：关闭2.3–3.9ms、增长1.3–11.8ms，每项4–5/7获益，最差仍慢44ms。53词突发sync虽少497ms，decode却多491ms，主要是等待转移。默认仍逐块同步；不将几百毫秒的计时搬移宣传为提速。

E38/E39各1123主记录，包含每实验96条producer-alone；旧38份证据不改，现40证据／21382主记录／28图，另保留此前500退出探针与2条忙／睡计量探针。22／30个sanitizer场景、32／32个真实CLI sanitizer任务及24／24个独立producer控制通过；E39源码的14,357-token合并同步Native、完整strict Stream和默认C/HTTP/CLI回归通过。[D-PREP §30](../design/prewarm-v1.md)、[完整数据与负结果](README.zh-CN.md)。未推送GitHub。

## E36/E37 归档结果（2026-10-31）

完成同负载off／休眠线程／owner空操作／仅分词／真实预填充隔离，新增逐次prepare进程CPU、owner CPU与decode次数。相同token／8次decode、无消费者时，累计prefill仍有71–110ms范围波动；后续prepare发生在消费者join后，不能把这些负对全归为直接抢owner。工具本身的CPU／非CPU时间也会突增；间接争用与设备／系统调度仍未完全细分。

新增显式`lazy-race`，首次合格输出增长才创建消费者。新42/55词任务，42次评价突发调用的消费者由42个降到0个，但每三工具任务原构造仅约0.049–0.079ms；按需版相对原增长策略，突发反慢2.556／1.157ms、持续反慢8.057／1.786ms，全部保留为负结果。按需持续输出相对关闭仍快141.552／186.168ms（6/7、7/7），没有证明比原增长策略更快。默认不切换，lazy仅可选资源实验，不将其包装为已解决大回退。

E36/E37各1283／771主记录；13／22个原生sanitizer场景和40／24个真实CLI sanitizer任务、最终默认回归通过。旧36份证据保持原样，当前38证据／19136主记录／26图，另有此前500条诊断探针。细节见[D-PREP §29](../design/prewarm-v1.md)及[完整报告](README.zh-CN.md)；本轮仍未验收Windows/CUDA、GPU硬件计数器、云SSE或自主模型任务能力，未推送GitHub。

## E33–E35：本轮结论（2026-10-31）

已完成同负载归因、两版退出等待优化及两组新任务验证，新增1027＋1539＋1283条主记录；历史33份证据保持原样。当前合计36份证据／17082条主记录／24张图。另外500条退出探针诊断分为首次、开发期与最终独占三组，不混作LLM任务性能样本。

- **剩余30ms不是单一原因。** E33的36词最差回退30.529ms中，EOF轮询多12.008ms；52词最差44.648ms主要是启动与工具后推理波动，轮询差近零。旧36词持续增长策略慢32.246ms没有稳定复现：E33反快16.361ms。worker排队／回调远小于这些差异，但无法排除间接争用或硬件调度因素。
- **发现并修正退出竞态。** 首版事件等待仍有约11ms回退；独立探针发现kevent注册ESRCH与waitpid暂不可回收的窗口。仅在ESRCH或收到NOTE_EXIT后使用1ms有界重试。E35每组84次工具调用中，原poll／event最大等待12.015／12.013ms，off-race／progress-race最大1.262／1.263ms。这是本机观测最大值，不是尾延迟保证。
- **执行器收益与流式收益分开。** E35持续任务off-race相对原off中位快40.677／48.196ms，均7/7；在同样修正的执行器上，progress-race相对off-race快152.085／138.815ms，分别7/7、6/7。突发同对照为−0.730／+7.770ms，没有稳定收益。
- **负结果仍明显。** progress-race相对首版progress-event，持续组分别慢8.112ms／快6.640ms；不能说修正让所有任务更快。E35最差突发off-race相对off仍慢144.433ms：工具后多115.438ms、累计prefill多138.381ms，EOF等待和推测更新均为零。54词持续的最差progress-race相对progress-event慢110.100ms，producer墙钟多127.289ms（CPU多29.921ms、非CPU多97.368ms），owner排队仅多0.004ms，prefill反而少14.954ms；说明完整任务仍受工具与运行环境波动影响，不能硬归为直接线程排队。

默认不切换。显式Native输出profile下，空闲REPL使用`/tool-output-policy progress-race`＋`/tool-output-prefill on`；关闭控制为`off-race`＋prefill off。原on/progress/event/thread/token控制保留，KV一直常驻，不新增快照或逐出。仅macOS本机验收，Windows/CUDA与云SSE仍未完成。

两轮优化及观测版各13个sanitizer单元场景，真实CLI sanitizer任务32／48／40全部通过；event/race的EOF先到、退出码、超时进程组、执行一次与容量拒绝测试通过，最终默认C/HTTP/CLI回归通过。Native14K与完整Stream仍是E29历史验收，未冒称本轮重跑。初次Darwin时钟声明编译失败、开发探针与全部负对保留，测量源码按实验独立归档。

详细配对与观测口径见[报告](README.zh-CN.md)与[方法](METHODOLOGY.md)。

## E31/E32 results (2026-10-31)

Matched off/old/on/thread/token controls completed: E31 1283 records. Frozen output-growth gate tested on new tasks: E32 771 records. Forty-word burst old versus token costs74.059ms more task time and70.183ms more prefill.48-word controls do not reproduce the same ordering; all negatives retained. Token-only frontend medians2.027/2.707ms; whole-task noise is larger, so negative token-control task differences are not free tokenization.

|Words|Iterations|Baseline|Candidate|Task saving median [min,max] ms|Faster /7|
|---|---|---|---|---|---|
|36|0|off|on|3.065 [-23.854,10.703]|4|
|36|0|off|progress|1.538 [-24.572,40.460]|5|
|36|0|on|progress|7.859 [-35.276,64.314]|4|
|36|25000000|off|on|111.044 [97.078,169.465]|7|
|36|25000000|off|progress|91.036 [37.602,185.376]|7|
|36|25000000|on|progress|-32.246 [-78.429,18.784]|2|
|52|0|off|on|5.539 [-11.603,36.464]|5|
|52|0|off|progress|5.337 [-30.462,15.519]|4|
|52|0|on|progress|-13.203 [-28.601,6.676]|3|
|52|25000000|off|on|173.037 [86.070,324.526]|7|
|52|25000000|off|progress|210.939 [140.327,298.986]|7|
|52|25000000|on|progress|2.832 [-31.805,124.869]|4|

Progress performs zero speculative updates/evaluations across42 evaluated burst calls. It is not universally better than timer:36-word paced tasks regress32.246ms (2/7 faster),52-word burst regresses13.203ms. Worst burst versus off still regresses30.462ms:23.740ms extra tool wall,5.691ms post-tool, zero streaming prefill/tokenization,0.391ms wrapper and0.058ms join. No OS scheduler/child-CPU trace distinguishes external variation from indirect contention. Keep progress optional; do not replace E30 default or claim stable tail reduction.

Idle Native REPL: `/tool-output-policy progress`, then `/tool-output-prefill on`; old/on retain controls, thread/token are diagnostics. Resident KV, identical receipts/canonical/greedy, once-only tools. ASan13 unit cases each and40/24 real CLI tasks pass; final default C/HTTP/CLI regression passes. Native14K/Stream regressions remain historical E29 runs. Previous31 evidence files byte-identical; now33 evidence/13233 raw records/21 figures. Source archives verified; no Git push.

## 2026-10-31 E30：输出观察窗与新任务验证

E30新任务实测（逐对off减on，ms）：

|词/段|循环/段|任务节省中位[min,max]|更快/7|额外prefill|
|---|---|---|---|---|
|40|0|-9.710 [-54.902,15.563]|2|15.157|
|40|30000000|188.140 [36.308,222.913]|7|89.427|
|48|0|11.535 [-22.266,28.884]|5|0.387|
|48|30000000|282.308 [243.066,293.666]|7|32.678|

13项ASan、16任务真实CLI验收通过；515正式记录、负对和源码ZIP保留。旧30证据与上一发布ZIP逐字节一致。突发组工具墙钟波动会贡献任务差异，不能将正中位全归给策略；同负载旧on及仅分词／仅线程消融尚未测。新数据支持继续保留显式策略，不保证所有短任务更快。

当前实现看 [适配清单](../adaptation-status.md)，当前展示看 [中文报告](README.zh-CN.md) / [English report](README.md)。本记录按实际阶段补充；旧证据保持不可变，后续同一天的新轮次使用新的标题／版本和实验 ID。

## 2026-10-31 · E29 agentctl真实工具输出预填充开关对照

- 先冻结D-PREP§25／D-CHAT，再接实际Native CLI REPL、registry、AgentStore和test.run stdout/stderr；同canonical session／单owner、4KiB latest-only队列和有界后缀LCP，既有历史KV常驻，不保存／逐出／恢复。
- 显式`--tool-output-prefill off|on`共同采用正文优先／最终状态尾收据及入站补缺失LF，保证最终收据／token相同；默认不改变。取消排空只退回本次推测tail，准备失败不重跑真实工具，recompute/restore组合拒绝。
- 真实CLI三工具＋四Native准备／首greedy任务；固定动作选择、相同预构建helper／命令，无sleep／任务内编译。32／56词持续产出（4000万真实循环／段）逐对任务节省95.254／183.960ms，均7/7；突发反慢43.004／20.590ms（0/7、2/7）。额外prefill46.853–169.079ms，持续产出工具墙钟增加21.597／97.364ms；收益来自重叠，负对完整保留，默认off。
- 515记录：64任务（8预热／56评价）、192真实工具（168评价）、256全文oracle和3状态。原始CLI wall及逐次排除oracle成本同留；13项fresh-greedy／UTF8／BPE／超时／截断／取消等ASan、16任务CLI ASan、Native14K／原Stream／默认回归通过。初次Metal沙箱失败／工具计数断言／首轮LF导致的正确回退保留，不进性能。
- 单独生产0.6B探测Native格式错误rc7、零工具执行：能力失败与实际执行链通过分开。旧29证据与resident发布ZIP逐字节一致；追加E29、5份CSV、1张图及其PNG，当前30证据／10664记录／18图。源码／运行时哈希和私有精确源码ZIP齐全，不推送GitHub，不叠加E26构建或E28不同负载收益。

## 2026-10-31 · E27/E28 正常常驻、多轮工具与真实进程流式

- 先冻结D-PREP §23及D-STREAM §7。生产API／默认未变，任务内保留KV，不save、主动逐出或restore；构建命令固定，使用同一预构建helper，首次执行暖机和初始历史单列，不累加E26构建收益。
- E27六轮真实read→FAIL→write→PASS→read→PASS，512/3072词、直接工具／resident包装，各7任务。199条＝168调用＋28任务＋3状态，12fresh参考核对。两模式都只新增397token；短／长包装逐对节省−14.293／+5.014ms，仅2/7／4/7更快，无稳定额外收益，不造清空KV基线。
- E28真实进程读12个语义单元，执行0或4000万CPU循环，无sleep／网络gap；80/240词，wait-all／合批／急切各7任务。87条／84完整任务／4fresh参考。持续产出时完整任务逐对收益合批136.054/213.587ms、急切159.388/234.151ms，各7/7更快；突发中位反慢.975–8.827ms，负对全留。
- 额外工作计费：80词慢源合批预填充多71.886ms、producer墙钟多17.769ms，任务仍快136.054ms；不相加各项独立中位数。两CPU场景digest和token数不同，只做同场景对照。首greedy在oracle前，完整任务计正常close，fresh参考时长单列，不藏收尾成本。
- 初次ASan collector同时保留接收KV和大fresh参考，超共享8192容量；9个完成验收任务及失败日志保留、不入性能。改为正常Stream close后独立参考，不改工作量／容量；最终collector、Native14K／既有Stream及默认回归通过，本轮不跑主动快照／逐出测试。
- 两新证据、两图、八CSV，源码／依赖哈希及精确私有源码归档齐全，旧27证据保持不变。累计29证据／10149记录／17图。真实本地完整语义单元流水线，未接厂商云SSE或Agent默认自动调度；未推送。

## 2026-10-31 · E25/E26 删除资源隔离、程序首次执行与增量构建

- 先冻结D-PREP §20，再按E25结果冻结§21的新任务。旧25证据字节不变，E25/E26各自追加并私有留存精确源码；不把旧313ms粗粒度追溯成已测OS机制。
- 新worker unlink-hold诊断：无ticket引用、身份验证后open/unlink，fd不逃逸，保留原slot／used额度至最后close；prepare/finish拒绝已unlink恢复，取消／worker关闭排空。新增可选父进程rusage／Mach footprint和全机VM计数；真实工具C驱动器分cc/run并记wait4 CPU/fault/IO操作，IO单位不混为字节。
- E25五组正反Latin10任务对照、227native／168调用／3fresh参考。early/return任务中位节省14.258ms、5/10更快，未稳定复现313ms；hold/early仅1.146ms、5/10。所有模式含resident的run墙钟约284–313ms，child CPU仅10.6–11.1ms；全机压缩／swapin在resident也有，不专属归因删除。
- E26新768词／12M／3*x−2修复任务，167native／123调用／3fresh参考；首次run、同binary rerun都真实执行并计费，共246次check。新artifact首次pre-main中位278.015ms、spawn调用.179ms、计算7.978ms；立即rerun pre-main1.788ms。逐条spawn返回→main为277.827ms（272.097–335.395），没有采样Gatekeeper／AMFI，不指定内部原因。
- 固定显式增量构建命令仅第三轮源未变时复用binary，修正后实际重建、测试从不跳过；完整任务逐对中位少327.722ms（283.323–395.514）、7/7更快。这是部署工具流程对照，不是自动Agent工具结果缓存或llama提速。early／hold仍有负对，默认resident／timed／return不变。
- E25 ASan/UBSan、Native14K/KV/Stream、占用／重复unlink、retained槽位、失效恢复、三阶段取消、默认回归及实际CLI一次通过。E26新驱动／main边界／双次退出一致／源变更与增量通过ASan；生产源码哈希未变，明确复用E25完整运行时回归。
- 正常resident不需要snapshot，不为预热主动逐出；restore文件仅同一live engine/session/revision，不是冷恢复或物理KV缩容。新增两证据／两图／九个CSV；累计27证据／9863记录／15图，中英报告、方法、设计与白名单包同步；无推送。

## 2026-10-31 · E24 完整任务归因与可选提前释放

- 先冻结D-PREP §18，离线逐对拆旧E23，旧24证据字节不变。15.058ms对应工具+8.354、工具前+8.207、工具后−3.517ms，其余修正／记账；各项同对可加，不能加独立中位数。
- report追加线程创建／tool函数入口；实际回调直接计时。新五策略含timed有／无事件，直接回调均值2–3μs、最大6.458μs，线程创建均值21–29μs，不能解释15ms；间接争用与OS／缓存／频率未完全分开。
- 可选release_snapshot_early在ticket finish排空后由同准备线程drop，不另建后台线程；返回join，失败留句柄供返回兜底，取消／工具一次语义不变。CLI新增--tool-snapshot-release early|return，默认return／timed不变。未注入OS删除失败，报告边界明确。
- 新512／2304词、18M rotate/xor及两输入乘法修复任务；实际FAIL→write→PASS→PASS和快读，各组7任务、3次调用，共659native／492调用，12fresh参考。提前释放暴露清理均值短2.308→.648、长4.004→3.346ms/调用；长等待逐对少20.711ms，但短／长完整任务逐对分别慢313.434／4.653ms，仅1/7／3/7更快，负结果全留，不升默认。
- ASan/UBSan、提前释放等待／运行／ready取消、工具异常、额度复用／关闭无私有KV、Native/14K/KV/Stream、默认回归及真实CLI一次通过。实测／验收共同源码哈希一致，源码快照私有保存。
- 新增E24、三个含源索引的任务／逐对／历史审计CSV、13号图；中英报告、方法、设计与现状同步。累计25证据／9469记录／13图；白名单发布仍不含模型／KV／构建和私密配置，未推送。

## 2026-10-31 · E22／E23 恢复分段归因、spawn与父进度ETA

- D-PREP先冻结E22观测，正式547条保留旧alpha／5ms及策略；E21不可变数据另做逐条索引审计。新增父进程启动／首次读输出／reap、timer、worker queue／verify／native／finish／返回、join／drop和最终前端／后缀时间。同一Darwin raw时钟，阶段恒等式和过去标签预测独立复算；旧C++数据没有与新增C时钟混用，不改旧记录。
- E22立即恢复的before-native约111／148ms，与fork117／162ms重叠；queue／finish小。旧timed锚点平均偏差.006／.007ms，prefix正等待4.978／5.202ms均值；ETA早晚误差混合，不能只看signed均值或全说成KV带宽不足。未采到libc锁内部，旧E21残差不事后逐条归因。
- E23单独冻结新320／1792 words和2500万真实运算；macOS26+默认test.run改posix_spawn，保持shell／cwd／env／stdio／独立进程组／超时。旧fork为部署对照，较旧macOS／Linux暂保留fork；启动错误不自动切backend重跑。标准addchdir核对SDK／Apple文档，未引入弃用_np首选。
- 新progress策略用epoch绑定父进程spawn返回：ETA=实际spawn返回＋过去EWMA后段，dispatch=max(spawn返回,ETA−过去准备−5ms)；未知／无进度回退当前事件／工具结束。相同完整标定标签，不看未来完成值，不复制历史。--tool-prepare progress显式可选，timed默认不变。
- 新979条：192标定／784评价＋3状态，七策略同轮转序。长历史立即before-native260.299→0.227ms；但progress未稳定同时降等待／空等。相对spawn/timed，短／长逐对等待节省3.334／8.243ms，空等−2.180／+0.089ms，双降2/7／4/7；长任务逐对节省−15.058ms反慢。不同组中位数相减与逐对中位数并列，全部负例保留，不用评价挑新默认。
- 进度旧epoch／重复／越界／取消，默认进程组超时／容量拒绝／一次执行、Native/14K/KV/Stream、ASan/UBSan及实际CLI脚本路由通过。初次观测collector误用固定SDK未支持的JSON.update，仅构建失败，无native样本，原目录保留。E23当前测量源码另存私有measured-source.zip，数据发布包不带构建产物。
- 两份新证据，旧22份字节不变；累计24份／8810条／12图。新增5个归因／阶段／逐对／任务CSV，中英报告、现状、契约、源码／依赖哈希与白名单包同步。物理KV buffer、CUDA/Windows、冷SSD、多消费者和模型自主能力边界未扩张。

## 2026-10-31 · E21 真实工具窗口、EWMA与KV恢复

- 先冻结D-PREP §13与D-KV借用边界。新增Native bind借用模式／checkpoint，engine只由worker owner访问；新增有界串行`AgentToolPrepare`，最多32个数值key、一个工具和一个准备线程，无第二个engine／持久历史。取消接受截止与最终检查同锁；等待／运行／就绪后取消都排空并回滚推测KV。
- 原生CLI显式`--tool-prepare late|immediate|timed`与`--tool-kv resident|recompute|restore`包装已有授权test.run。默认HTTP保持兼容。EWMA alpha=.0951249，工具耗时和准备ms/token分别更新；未知立即启动，提前结束唤醒timer，固定5ms余量，无Gaussian就绪保证。复杂Roofline/shape API仍独立。
- 实际fs.read／C编译+运算测试，三轮fail→真实写修正→pass→pass；两档历史、7策略、每组7完整任务，rep轮转策略顺序；三次标定初值同源，之后只用各自过去标签。保存／逐出计入总成本，不用sleep制造性能窗口。931native，144标定／784评价＋3元数据／验收／关闭；12独立fresh-KV参考逐行核对696调用，非自主模型能力分数。
- 长历史编译工具后等待三轮合计：重算1759.905→631.885ms、恢复242.040→150.610ms；完整任务3242.604→2091.362ms、2058.327→1878.756ms。恢复逐对等待节省94.834ms／任务214.356ms中位，均7/7为正；resident1582.892ms仍最快。快速读无实质重叠，cold-state代价不能说成普通Agent必需。
- 长恢复立即等待119.848ms，比定时150.610ms更少；定时却使PREFIX_READY提前空等1077.405→18.483ms。短重算定时比立即使任务1555.155→1609.734ms变差。立即恢复总准备在native恢复之外多282–293ms，队列／fork／唤醒未独立计时，未归因项单列，不能全说成磁盘／带宽变慢。所有范围、逐对负值、保存波动保留。
- 模型CLI单次探测正常退出却输出裸JSON、实际工具0次，保留失败；确定性Native-parser动作经过原CLI注册表／AgentStore，真实编译命令恰好1次，ASan/UBSan通过。默认回归、Native/14K/KV/Stream通过；取消边界修复后另做当前模块ASan验收。初次开发构建缺C linkage未链接，原目录保留；开发与qualified目录不混入正式性能。
- 追加E21，旧21份证据不变。累计22份／7284原始记录／11图，新增任务／概览／逐对／恢复组件四CSV，中英报告、设计／现状、源码与依赖哈希、白名单包同步。通用P0–P4、私有候选ADOPT、容量／公平性、冷模型启动及Windows/CUDA均未因此完成。

## 2026-10-31 · E20 可选Kalman残差层与新网格回放

- 先在D-COST冻结模型／噪声／评价口径，再新增纯C `cost_kalman.h` / `cost_kalman.c`：40B无heap／上下文；只读predict，成功observe才推进一次Q，使用未过滤base；profile、有限值、溢出与失败原子性边界；Joseph协方差更新。
- 同diverse标定内估计常数R和非负耗时平方R，冻结Q/R和初值。固定稳态Kalman严格等价EWMA，明确对照；三种base各做严格先预测后观测的C回放，Python独立核对gain／covariance／预测和参数来源。
- 新prefix1792/3840/5504、new11/18/26/88/208/400/688，cap256/64。当前shape-online从3.173/1.975降至3.073/1.922ms MAE，仅3.2%/2.7%；simple EWMA在六组中的五组胜过scaled R。scaled在cap256/shape-online的max9.582→9.778ms，fixed每cap10/21格变差，负结果保留。
- 名义95%覆盖仅15.65–70.07%，标准化innovation SD2.64–11.85。组内重复抖动无法代表跨形状模型误差；不确定度低估不等于直接证明分布非正态。保持可选，不据此保证调度准时，不在评价上重调Q/R；连续七次同形状次序未代表真实Agent任务混合。
- 手算／step／spike／等价及atomicity单测、ASan/UBSan和默认回归先于计时通过，78fresh-KV形状oracle通过。层predict/observe中位约0.0092/0.0136µs，与模型推理耗时分开。
- 新E20含1162native记录；882层预测引用同294评价任务，另加元数据／10开销记录，不虚增原始推理样本。归档累计21份／6353原始记录／10图；新增四个Kalman CSV，中英报告／日期／发布清单同步。旧20份证据保持不变，默认Agent、Windows/CUDA边界未扩张。

## 2026-10-30 · E18／E19 冻结回退分因、残差校准与新batch

- 先写D-COST排查／冻结策略。E17开发审计：精确NNLS与旧拟合差<6.6e-9，原迭代已收敛；平方目标／全局basis、表外多chunk及阶段波动分别检验。新增独立C raw先验＋3变量signed残差ridge，权重1/max(ms,20)、RMS缩放、lambda.01、直接Cholesky，64窗口3632B，无heap／上下文；原API保留。
- 新batch256／64、prefix1280/3328/5376及新token网格验证。sparse/diverse各63标签、交错测量；扩表至7168并保留旧端点控制；54条anchor排除拟合，294条weighted离线结果引用raw而不增加native样本。
- cap256等量覆盖使legacy5.331→4.486ms，residual/diverse3.452ms；cap64新冻结2.645ms仍差于raw2.183ms；仅权重无一致收益，扩表在256回退。训练误差变好而泛化变差、长anchor阶段变化均保留。算法不随E19评价重调，不宣称通用优势或推理提速。
- 首次E18完成cap256后因默认prefill_min128>batch64被拒绝，未生成64样本；全部849条单列不完整证据。仅修测试入口min(128,batch)，E19独立新目录一次完成1162条，未按准确率挑运行。当前源码哈希看E19。
- 解析ridge／共线／零列、signed／clamp、窗口／漂移、profile／失败原子性测试、ASan/UBSan、默认回归在GPU前通过；78真实fresh-KV形状及token/greedy/batch通过。表960B＋残差3632B，组合predict0.1629/0.1301µs；额外微测8.695s单列。
- 统一20份证据／5191原始记录／9图，新增冻结训练／逐格误差／anchor三个CSV，中英报告和发布清单同步。旧18份证据保持不变；离线对照单列，默认调度与Windows/CUDA等边界未扩张。

## 2026-10-30 · E16／E17 batch/context拆分、研究与时间插值

- 在D-COST先冻结测试／接口，再补相同过去标签的经验／Roofline／shape冻结与online八方案；E15事后回放Roofline online5.641ms不作为新实测。
- 每轮两个batch上限，各63标定／147评价；独立FA探针及完整任务确认19/20边界，逐格bias、MAD、min/max保留。参考Vidur、FlashAttention-2和IBM Roofline＋回归的一手研究，明确CUDA／云端结论不能代替Metal验证。
- E16加960B数值形状表，raw改善但online在64-token回退。查到先插值倒速率再乘n制造曲率；E17改为服务时间插值，换未在E16评价的prefix/token网格。E17shape online相对原online的MAE改善25.6%／42.2%，冻结shape仍回退。负结果全部归档，不跨轮累计收益。
- 两轮每轮60形状fresh-KV greedy/token/batch对照通过，单测／ASan/UBSan／默认回归先于性能。E16 native成功后wrapper缺re导入，从原raw恢复元数据，没有重测；原测量runner／processor哈希分列。E17直接完成，当前源码哈希看E17。
- 新增两份916条证据、两张图和672行逐格误差CSV；统一18份／3180条／8图。额外微测约4.8秒、表＋一校准4080B、predict约0.11µs单列。仅改善估算，未接默认调度，未测纯GPU／DRAM／频率／温度或Windows/CUDA。

## 2026-10-30 · E15 原公式偏差排查、算量修正与依赖图速率

- 先扩展D-COST排查契约；独立复算E14的147条FLOPs／bytes／时间无算术或单位不一致。根据固定上游源码补最后一层FFN按输出位置裁剪，新增零值保留旧行为的spec字段，需重编译调用者。
- 深度1／16依赖图差分拆固定图开销，加入实际dense FlashAttention与head-interleaved stride，连续图后重复旧探针检验顺序影响。初次开发探针误设sparse提示，正式测量前修正；两轮开发探针均单列排除，未按评价结果选参数／重跑。
- E15同任务消融证明主因是compute-rate／算子口径，带宽影响小：旧raw240.283ms→完整修正14.220ms MAE，校准9.133ms；原frozen10.117ms、online6.434ms。只改速率13.845ms比完整修正稍好，保留误差补偿负结果；改善不等于推理提速。
- 一／两层FFN手算、原cost、ASan/UBSan、默认回归通过后独立运行性能；真实21形状greedy/token/batch通过。新的raw/predict/refit中位0.0187/0.0753/1.9475µs，状态3120B。
- 追加278条E15记录及第6张图，统一16份证据／1348条记录；中英报告、单位／depth维度、斜率及历史算术校验、CSV、清单和发布包同步。旧15份证据不变；当前实测源码看E15。未测真实DRAM流量、纯GPU时间、GPU频率／温度、Windows/CUDA，未接自动调度。

## 2026-10-30 · A16 Roofline 接入与 E14 同任务对照

- 先扩展 D-COST 契约，再新增纯 C Roofline API、GGUF 元数据适配和同 GGML/Metal 后端微测；公式来源及访存／融合假设写明。共享三变量拟合移入私有入口，原 cost public API／特征不变。
- 63条标定／84条评价，五种预测共享相同实测任务；原始 Roofline MAE236.315ms，校准后5.281ms，原frozen5.526ms、原online3.931ms。原公式负结果保留，单轮小差异不作稳定优势；未按评价集调参、未替换运行。
- 手算 FLOPs/bytes、GQA、chunk logits、缺失速率、失败原子性、profile和未见形状单测、ASan/UBSan、默认回归通过；之后独立运行真实模型21形状从头KV greedy与计数核对。推理和调度没有变化。
- 新增E14的195条记录、第5张图，统一15份证据／1070条记录；中英报告、CSV、单位、provenance、发布包同步。E13与全部旧证据不变；其源码哈希保持历史版本，当前实测哈希看E14。
- raw单chunk估算中位0.0144µs；完整校准预测／refit开销未另测。冷启动、KV恢复耗时模型、默认调度及Windows/CUDA继续待验收。

## 2026-10-30 · 硬件公式补入后续估算方案（未实施）

- D-COST 增加 Roofline 理论下界与有效算力／带宽的估算结构；明确 E13 当前尚未使用这层先验。
- 结构量与实际流量分别核对，KV 容量不直接等于 prefill 读写量；不同设备／精度／缓存路径的吞吐不得混用。
- 下一候选采用物理结构先验＋少量效率校准＋在线修正，与经验模型独立比较。仅更新设计和展示包，无新代码／实测数据。

## 2026-10-30 · E13 误差诊断（分析，未修改估算器）

- 按已有原始样本核对：最大在线绝对误差75.717 ms对应433.952 ms工作，约17.45%；同形状实际耗时约434–509 ms。
- 区分三参数经验模型失配、稀疏标定／768-token外推与同形状运行波动；当前数据不能把波动归因到具体硬件资源原因。
- 记录范围标记的缺口：在线见过短前缀768后，长前缀768也可能不再标extrapolated；范围覆盖不等于组合覆盖／置信保证。
- 下一轮先做阶段与条件诊断，再验证分段／稳健拟合及不确定性。旧证据与模型代码不变，本轮未重测性能。

## 2026-10-30 · A15 本机耗时估算与 E13

- 按用户调整顺序，先实现独立 [D-COST](../design/cost-estimator-v1.md)，保留 D-PREP 的 P0–P4 待实现状态。
- 新纯 C 有界 64 样本窗口、3 个非负工作量系数、profile 隔离、外推标记与在线纠正；状态 3120 B，无动态分配、不持有上下文。单测纳入默认 make test。
- E13：固定 Qwen3/Metal，63 标定＋84 评价，共 161 条原始记录。固定吞吐／冻结／在线 MAE 为 68.594／15.991／10.440 ms；最大在线误差仍 75.717 ms。所有预测比较同一实际工作，未声称推理提速。
- 10 组开销测量：predict／observe 中位 0.086／3.365 µs；21 种形状从头 KV greedy、canonical ID、准确 token／batch 数通过；C 单测、ASan/UBSan、默认回归通过。
- 首次 sandbox 无法初始化 Metal，无有效样本；第二轮 wrapper 后处理失败且与回归重叠，被排除。正式使用第三轮，核验完整 GPU offload；开发轮更好读数未替换正式结果。
- 追加第 14 份证据和第 4 张图；统一 875 条记录，新增预测误差复核，报告统计总数改为自动汇总。所有旧证据不变；未接 deadline 调度、冷启动、Agent ETA 或通用程序复杂度估算。

## 2026-10-30 · 预热完整契约与设计缺口审计（未实施）

- 新增 [设计登记](../design/README.md) 与 [D-PREP v1](../design/prewarm-v1.md)，明确单一维护位置、旧锚点迁移、当前／待实现／阶段记录状态。
- 正式设计覆盖环境／请求版本、异步源借用与释放、四种 ready、稳定前缀、候选 session、原子 ADOPT、时机估计、容量／公平性、失败／取消／关闭及异步冷启动；实施分 P0–P4。
- 根据源码补 [D-KV](../design/kv-worker-v1.md) / [D-STREAM](../design/stream-v1.md) 当前契约：完成不等于成功，Stream 旧源码保留但旧收据排除，已有独占／同步限制明确保留。
- [缺口审计](../design/audit-20261030.md) 区分文档已补与代码待做：Native 借用模式、不可变源借用、唯一 KV 余量观测、交接竞态和调度／冷启动仍未实现。
- 原上下文文档中的预热提纲迁为跳转；适配清单缩为状态索引。更新维护入口与展示白名单，旧 evidence、图表与全部性能数据不变；没有运行模型测试。

## 2026-10-30 · 提前意图与临近提交预热设计（未实现）

- 新增 system/tools 提前变更通知、分层准备、稳定前缀预填和输入开始信号的后续契约；尚未新增运行时接口。
- 对照当前代码：Stream 已在 open 中同步准备 system＋初始上下文，但 tools 固定为空；Native 在完整 prepare 时处理工具变化；模型/context 尚未拆分。
- 初始时机公式使用预计提交、owner 排队、准备关键路径与误差余量；优化目标同时考虑暴露等待、唯一 KV 占用时间、作废工作和其他任务延迟。
- 记录前缀依赖、epoch／修订校验、有限保留和三策略／冷热分组的验收计划；输入开始是宿主软信号。
- 更新当前状态与展示包。没有运行新的推理实验，E01–E12/L01、714 条原始记录及旧性能图保持原样。

## 2026-10-30 · 适配盘点与维护入口

- 新增 A01–A14 清单，逐项说明实现、调用入口、证据和未完成边界。
- 明确接入方式：cxf 调用上游原生库，固定 b10938；可选 CLI 已接入，KV worker / Stream 仍由宿主调用。
- 补充持久维护规则：设计先更新，实施后更新状态，新测量追加证据，旧数据保留实测版本，图表和中英报告同步。
- 根 README、状态索引和展示包增加当前适配入口；重建白名单及离线包。
- 验证范围：文档／统计／来源／链接／发布包；没有修改推理代码或重测模型性能。

## 2026-10-30 · 测试成果统一归档

- 汇集 13 份证据；9 份原始记录集共 714 条，包含元数据、正确性、计时与文件时间线。
- 中英报告、完整指标 CSV、验收矩阵、原始记录索引、运行清单与 3 张 SVG/PNG 图表统一归档。
- 最早的直接 API / 慢流实验从忽略的构建目录补为公开证据；慢流实验缺失的历史源码哈希明确保留为空。
- 两次早期真实模型失败只发布脱敏结果和私有日志哈希；早期仅文档读数标为 reported-only。
- 原始数据与正式本地运行逐条一致；汇总标量、流式工作量／时间顺序、成对统计、发布哈希及离线包通过校验。

## 2026-10-30 · 已有实现阶段与证据映射

下表按实现依赖顺序归纳既有阶段，不表示这些数字可叠加，也不把整理日期当作新的实验。

| 阶段 | 已完成内容 | 证据 |
| --- | --- | --- |
| 基本工具闭环 | 文件查看／读写／真实编译测试／结果回流；模型能力探测单列 | E04、E12 |
| 直接 API 验证 | 已有前缀缓存对照、显式后缀、预填充、分支、状态恢复 | E01–02 |
| 原生上下文 | 单份 token 块池、显式 revision、回退、预算、同引擎分支、容量失败恢复 | E03 |
| 原生聊天／工具 | 上游模板／解析器、稳定检查点、有界生成、可选 Native CLI | E04–05 |
| 模板准备缓存 | 已验收模板与工具解析元数据复用 | E06 |
| 单份上下文审计 | 14K token oracle、共享分支、临时对象提前释放 | E07 |
| 借用消息 | 跳过完整请求 JSON 往返；完整渲染仍在 | E08 |
| Jinja 与分词缓冲 | 暖态直接值、直接结果收集、有界缓冲重试 | E09 |
| KV 调度宿主 | 私有快照、逻辑逐出、单 owner、有界异步提示、取消与修订校验 | E10 |
| 流式宿主 | NDJSON、完整单元预填充、文件版本收据、输入结束前反馈、file/stdin 回放 | E11 |
| 历史背景 | 服务容量、标记预算和复制微测的文档读数；原始样本缺失 | L01 |

证据 ID 的文件与 SHA-256 见 [catalog.json](data/catalog.json)。历史设计与实现细节仍保存在各自契约文档；当前边界以适配清单为入口。

E44剩余负对补充：45词持续关闭组最差反慢86.283ms，最终prefill多93.325ms（sync多84.062ms），工具墙钟反少20.539ms。该bare对没有设备trace，不能精确分配GPU／host原因。带probe的51词持续合并组GPU跨度少23.360ms、最终prefill少18.263ms，整任务却中位反慢11.240ms，工具墙钟多32.426ms；证明设备优化与工具／系统波动可以并存，不能宣布剩余回退已解决。
