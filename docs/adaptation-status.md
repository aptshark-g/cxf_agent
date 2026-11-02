# llama.cpp 适配现状

## E58 只读自预测闭环与异机交接（2026-11-02）

实验宿主已实现同模型、同KV池的前缀分叉探针，提前执行单个fs.read；主分支正式调用的工具名／完整参数、任务epoch及文件／权限版本一致才采纳，否则丢弃并正常读取。所有推理调用仍由单owner串行执行，probe暂停主解码，其成本完整计入；普通工具权限不放宽，不做快照或逐出。默认agentctl未接入，唯一契约为[D-SPEC](design/self-speculation-v1.md)。

24项无模型状态／安全测试和8项Native验收通过ASan/UBSan，后者含2项注入token的完整采纳／回退流程；真实0.6B仅核对KV／模板／主greedy不变，原14,357-token Native回归及默认回归通过。开发期JSON流编译、thinking-header假设、外层Metal／回环限制及0.6B预算耗尽负例分别保留；没有从注入流程计算模型命中率。

off/on runner已准备任务轮换、完整参数命中与实际采纳率的明确分母、probe／工具／等待／回流／整任务阶段、输出哈希一致性，以及所有失败与负对。更大模型命中率、端到端收益和WSL2/CUDA仍待另一台机器实测。[交接操作](self-speculation-handoff.md)包含固定引擎构建、短验收、全量对照、自定义新任务与结果带回清单。当前59份证据／36,664主记录／41组图；旧58份证据和41组图不改，本轮不增加正式性能样本，未推送。

## E56/E57 固定目标的C++归因与私有PCH（2026-11-02）

旧E55的59ms残差带隐藏target差异：相同编译器／SDK和程序结果下，绑定off默认macOS26.0，绑定on为10.4。此前验收未检查这个条件，旧证据与40组图不改，报告新增限制，不能继续当同target成本。E56／E57全部显式-mmacosx-version-min=26.0并核对driver triple和部署宏，不开放版本查询权限，不替用户工程猜部署目标。

E56 270条正式阶段／缓存记录显示，额外sys CPU跟随头文件：预处理中位28.157→79.755ms、目标文件30.891→84.015ms；已预处理输入9.925→9.875ms。普通编译与重复编译没有PCM，1051条include trace不是syscall计数。父准备约1ms，EOF回收可能另有10ms轮询；新执行文件运行存在明显非CPU墙钟，未精确归因签名／调度／CPU簇。模块与SDK stat-cache冷成本高，单次PCH也会变慢，不发布免费预热收益。

交付可选tools/run_cpp_batch.sh plain|pch TARGET HEADER OUTPUT SOURCE...：同一公共前缀头，任务私有PCH，保持编译器验证，完整计入生成／编译／链接／运行／清理，不新增目录、IPC或网络授权，不自动修改agent命令。E57四组新任务180次CLI（2预热＋7评价，五模式轮转／倒序）：单文件PCH反慢158.765／118.314ms；3／5翻译单元分别逐对中位节省330.300／793.776ms，均7/7。PCH短期逻辑文件约14.56–15.51MB，未测RSS／KV；开启−关闭仍有宽幅正负对。

敏感规则72→6组候选通过1765路径差分与15组OS测试，但新任务性能失败：5翻译单元中位慢64.269ms、仅2/7更快，3翻译单元最差慢1299.522ms，主要在目标文件阶段（+1291.321ms，外层反少3.256ms）。不把全部波动归因于regex算法；默认继续原逐条规则及日志，未改生产C运行时。

6组新配方普通／ASan/UBSan测试通过，覆盖正常流程、失效／失败清理、头文件更新、敏感配置／网络拒绝与off负对照；默认561 C／15 OS／12策略／8基础回归通过。Native／推理源码与E55逐字节一致，仅Makefile增加新测试；E55的14K／Stream／输出是同运行时的历史验收，本轮未重跑模型。初次漏导入与预热双时钟差异超时的失败保留，正式轮次只用父进程存活期防空闲休眠及双时钟校验，最大差异0.029ms，不改系统电源或agent超时。

当前58份证据、36,664主记录、41组图；中止预热另列，旧56份证据及40组图逐字节保持。源码、数据和发布清单同步，只在本地工作，未推送。[E56](evidence/sandbox-cpp-diagnostic-20261102.json)、[E57](evidence/sandbox-cpp-tasks-20261102.json)、[完整报告](benchmarks/README.zh-CN.md)、[D-EXEC](design/test-sandbox-v1.md)。以下保留旧阶段背景。

## E54/E55 Xcode发现归因与显式工具链绑定（2026-11-01）

E54定位到每任务私有xcrun查找缓存为空后触发xcodebuild探测。126条正式记录中，空进程隔离配对开销约14ms；同一沙箱首次／热第二次编译中位2359.470／34.224ms，删除本任务自己的xcrun_db后第二次又到2393.318ms。off另用全新HOME／TMPDIR仍明显快于on，故不只空缓存本身；受限框架初始化／拒绝服务回退有额外成本。只设SDKROOT与no-log均未消除秒级回退，后者还有更慢配对，不发布、不扩大IPC／文件／网络权限。

精确目标有7305条拒绝日志，含2511条目录服务查找；重复日志不是精确系统调用次数。另70条API诊断（各1000调用）中，受限Mach查找合计中位约28–29ms、单次最差不足1ms；缓存目录／UID查询有累计成本。栈采样没有调用栈，尚不能把整个xcodebuild延迟逐函数精确分配，不能称一次IPC超时导致2.47秒。

新增显式`--test-toolchain BIN_DIR --test-sdk SDK_DIR`，只绑定PATH与SDKROOT，目录必须同时有效，不隐含读／写授权。默认profile及拒绝日志保持E53；无共享缓存、宿主外执行工作区代码、自动CLT替换或全局工具链修改。未指定绑定的标准shim仍有旧回退。详细契约为[D-EXEC](design/test-sandbox-v1.md)。

E55在三组未参与调参的新任务完成108次真实CLI执行，2预热＋7评价block、四模式旋转／倒序。相对未绑定on，完整任务逐对中位节省：校验和2367.142ms、静态库3447.935ms、C++2378.431ms，均7/7获益。绑定on对绑定off仍多20.951／14.789／59.075ms；C++范围−160.955至100.214ms，负对与波动保留。每任务全新私有HOME／TMPDIR，off的创建／清理也在计时CLI命令内，与E53通常已热宿主缓存不能跨阶段相减。编译器／SDK身份和程序结果一致，无LLM性能收益声明。

561项C、15组OS、12组策略／8组基础SAN套件、14,357-token Native／16 Stream／72输出及完整默认回归通过。原6条Native警告保留，上游库未插桩；初始两处测试预期错误保留。当前56份证据、36,214条主记录、40组图，70条直接API记录另列；旧54份证据与39组图逐字节保持。运行时／测试源码与E55计时归档一致，不把旧推理成绩升级为新源码。仅本地工作，未推送。

[E54诊断](evidence/sandbox-cc-diagnostic-20261101.json)、[E55新任务](evidence/sandbox-cc-tasks-20261101.json)和[完整结果](benchmarks/README.zh-CN.md)。下面保留旧阶段范围。

## E53 当前进程隔离与宿主环境保护（2026-11-01）

E52事后审查复现父环境可见，单靠收窄Seatbelt sysctl规则也未封住。当前只给cxf自身设置CS_RESTRICT、检查SIP门，并用可信子进程验证环境省略后才启用受限命令；CLI在配置／模型初始化前完成，标记对该进程粘性。不改系统SIP或其他应用，失败不裸执行。文件／网络默认仍是工作区只读、独立临时写、禁网；目录／联网由启动者分别授权。其他同UID进程环境、可见argv和VM隔离不在保证内，key不能放命令行。唯一契约[D-EXEC](design/test-sandbox-v1.md)。

542项C、12组OS权限（新增父环境正反对照）、原12组策略／8组基础SAN套件、14,357-token Native／16项Stream／72项输出全部通过。新进程off能找到合成父环境标记，默认及显式network allow都看不到；查询可能仍返回argv，不能写成整个sysctl接口已不可用。原6条Native警告、预编译库未插桩边界保留。

E53独立230条记录／93组评价配对，完整CLI的开启−关闭中位额外：空工具15.724ms（5/7更慢，−3.644至32.934ms），显式CLT3.565ms（4/7，−9.989至32.725ms），标准cc2467.548ms（7/7，最差2534.841ms）。没有按E52→E53差值声称优化；API评价为预热后的共享保护宿主，CLI每任务新进程含初始化，冷预热行保留。内存容量和一次可信fork另列，不冒充RSS／KV下降。

[E53证据](evidence/test-sandbox-host-20261101.json)与[完整报告](benchmarks/README.zh-CN.md)保留所有负对和失败。当前54份证据、35,980主记录、39组图；旧53份与38组图逐字节保持，E52明确是修复前阶段。计时后仅更新启动器帮助文本，运行时／Native／计时程序哈希一致。尚未实现Linux／Windows后端或整个宿主进程空间隔离；未推送。下面旧阶段保留原范围。


## E52 test.run文件／网络隔离与开销（2026-11-01）

当前macOS后端默认使用Seatbelt：工作区正文只读、独立临时目录可写、命令及后代禁网，关闭已有fd；读／写目录与完整联网需要启动者显式授权。敏感名称、真实配置／trace、链接别名与受保护祖先改名仍拒绝。`--test-isolation off`显式取消OS边界，不能从模型或JSON放宽。不支持后端／准备失败不降级。Linux／Windows后端尚待做。唯一契约为[D-EXEC](design/test-sandbox-v1.md)，敏感规则继续由D-DATA维护。

542项C检查、11组OS安全套件、原12组数据策略和8组基础套件在ASan/UBSan下通过；14,357-token Native、16项Stream和72项输出通过。候选localhost地址过滤放行本机非回环监听服务，已撤下，仅发布deny/allow；初次Stream比较因Xcode诊断时间／PID不同失败，仅确定性夹具显式用CLT后通过，原token／greedy检查保留。失败事实与日志哈希均保留；原6条Native警告、上游库未插桩边界不变。

新增230条独占无SAN记录、93组评价配对。完整CLI的开启−关闭逐对中位：空工具+13.925ms，显式CLT编译+19.042ms（5/7更慢，范围−33.950至48.483ms），标准Xcode cc+2591.134ms（7/7更慢，最差+5524.787ms）。大退化主要在命令内部，不能全归profile准备；具体缓存／IPC成因未分离。没有改变生产编译器或把CLT替换当优化。73,880字节固定任务栈容量在off/on均存在，不是新增RSS或KV指标。

[E52证据](evidence/test-sandbox-20261101.json)和[完整报告](benchmarks/README.zh-CN.md)保留全部样本、负对及分阶段口径。当前53份证据、35,750条主记录、38图；旧52份证据和37组图逐字节未改。计时后只有Stream测试命令改变，源码哈希另记；运行时／计时程序未变，历史推理成绩未复测，不跨阶段累加。当前仅本地工作，未推送；以下旧阶段保留其历史范围。


## E51 统一敏感文件与云端外发策略（2026-11-01）

所有文件工具接入同一敏感路径／实际配置／活动trace／inode规则，拒绝符号链接组件、普通文件硬链接及非规范路径；旧write/diff/append实际内容访问迁移到目录fd。list／grep跳过保护项，默认保护.env、密钥与本地配置，普通源码仍可读—改—测。宿主统一检查工具参数和结果，已知秘密在进入下一轮上下文前拦截；HTTP和模型解码边界再检查请求／响应。范围与限制只在[D-DATA](design/data-policy-v1.md)维护。

HTTP主提供者默认UNKNOWN，云端外发默认deny；启动者显式声明local或cloud，cloud可选task／workspace，task不派发工具，http.post另有独立scope。Native明确本地，llm.local也受凭据规则约束；回环不等于不会转云。当前HTTP仅回环，直接HTTPS未实现。策略来自CLI／启动环境，JSON和模型不能放宽。

445项策略检查、12组安全HTTP套件在ASan/UBSan下通过，原8组基础套件与完整默认回归通过；真实Native14,357-token、16项Stream和72项输出回归通过。初次新增Native测试引用旧检查点的断言失败保留，修正测试基准后通过；6条既有harness编译警告及预编译上游未插桩边界保留。无真实云端调用、模型能力或性能新结论。[E51功能证据](evidence/agent-data-policy-20261101.json)。

当前52份证据、35,520历史主记录、37图；旧51份证据逐字节保留。新增源码不继承旧性能成绩，策略扫描开销尚未测量。test.run仍无OS沙箱，不能阻止固定命令主动读取本机文件／联网，也不是通用DLP。当前工作仅本地整理，未提交／推送。下面E50等阶段记录保持其历史范围。

## E50 基础能力更新（2026-11-01）

新增 `fs.replace_line`（仅写权限挂载、唯一整行匹配、目录fd与原子替换、LF/CRLF保留），CLI `test.run` 默认最小环境（显式 `--test-env inherit` 放宽），以及 `--local-config` 启用的回环 `llm.local` 文本辅助。辅助仅接主模型显式选择的文本，独立认证、无工具权限、每任务两次配额；结果回到主提供者，不能称不会再到云端。契约见[工具基础](basic-agent.md)和[D-HYBRID](design/hybrid-v1.md)。

8组双端点桩在ASan/UBSan CLI上通过，基础编辑／环境sanitizer和默认完整回归通过；Native 14,357-token、16项Stream、72项工具输出回归通过。Native harness保留6条已有编译警告，固定上游库未插桩。[E50功能证据](evidence/agent-foundation-20261101.json)单列，不计作模型能力或性能；没有真实云请求／端云质量结果。

当前归档51份证据，35,520主记录和37图保持原历史测量口径。旧50份证据逐字节未改；本轮工具／CLI／协议源码已变化，旧性能不升级为当前版本数据。OS沙箱、统一敏感文件策略、全部旧文件路径硬化、原子文件CAS、HTTPS／云SSE／自动路由和辅助共享Native KV仍未完成。

## 首次 GitHub 发布（2026-11-01）

仓库所有者已明确授权将源码、文档、历史数据和图表上传至 `aptshark-g/cxf_agent`。本次发布按 438 文件源码白名单提交，保留远端只含 README 的初始化历史；密钥、本地配置、私有日志、模型／运行时、KV 快照、构建产物及独立项目继续排除。范围及校验见[发布指南](RELEASING.md)。

推理实现与策略不变；50 份 evidence、35,520 主记录和 37 图保留原测量版本及负结果。上传前重新检查清单与暂存树哈希，不重写旧数据、不把发布当成新模型／性能验收。以下“GitHub 发布准备”保留为本轮上传授权之前的阶段记录。

## GitHub 发布准备（2026-11-01）

中英文首页改为目的、使用、实测与限制的导航；MIT 已按所有者选择加入。公开源码中的本机模型默认目录改为仓库内 `local-model/`，三份脚本显式 SDK／runtime／model 参数不变。旧设计里的实际端点和凭据来源已脱敏；示例只保留空 key、通用回环地址／占位符。

新增源码白名单、内容／链接／哈希校验和确定性导出；忽略本地配置、会话历史、模型／运行时、日志、快照与独立项目。详细范围和验收见[发布指南](RELEASING.md)。本轮未改推理调度或实验策略，不新增 E 编号；50 份证据与原测量版本保持不变，35,520 主记录／37 图继续按历史实测解释，不标为当前源码重新测量。暂不提交或推送。

## E47–E49 当前更新（2026-10-31）

E47固定640容量、相同最终cap512／尾批合并，对关闭／仅线程／仅分词／真实growth做同负载控制（47/54词、0/1200万循环）。工具等待与GPU工作分开：仅线程没有工具期间GPU预填充，仍出现整任务多1868.877ms，其中工具多1817.950ms、producer墙钟多1761.856ms、自身CPU反少22.205ms、ready代理多1784.891ms；growth最差多392.829ms，工具多522.869ms、最终prefill反少334.860ms。这证明并发推理不是这些停顿的必要条件，不将其全归kernel效率或某个竞争PID。旧217ms bare仍不能追溯精确设备／通知归因。

私有逐批callback已接专用Metal harness，关联decode position／tokens与buffer，生产默认空hook，稳定C API和预算／同步语义不变。M5实测只支持stage timestamp、不支持dispatch sampling，也无公开带宽／占用counter；未采GPU频率或外部全设备调度。GPU区间仍含访存、内部停顿与抢占，不能称纯kernel忙时。

E48固定prefix768／2048、increment128／224／512／582，同shape比较空闲／算术／64MiB内存线程。582-token空闲合并GPU中位快7.869／13.458ms，负对保留；内存对算术压力使224／582-token合并的GPU跨度中位多约6.5–10.2ms，提交残差一般仅多0.02–0.19ms。说明内存路径竞争可体现在设备执行阶段，不是纯提交排队；不足以证明旧217ms全由内存或解释全部106ms设备尾部。root物理保留2048 cells，即使逻辑prefix768，不能从这组数据外推单会话上下文曲线；逻辑读写量不等于DRAM带宽。

E49先冻结更大输出发布量子1536字节（旧512），再测新49/55词、0/1000万循环。该优化尝试失败：新coarse相对旧growth，突发-3.567ms（2/7；最差-34.783ms）／-5.492ms（3/7；最差-47.290ms），持续-54.667ms（0/7；最差-75.754ms）／-3.009ms（1/7；最差-59.735ms）。带marker持续样本旧／新均42批，token从9240增至11970，GPU累计2518.927→3274.296ms，没有减少批次。较晚启动使每三工具任务join逐对中位多82.889／73.485ms，最终prefill虽少25.716／63.708ms，仍未抵消。默认保留旧门槛，coarse仅实验选项，不按这批评价继续调参。

旧growth对关闭在持续新任务仍快104.276ms（7/7；最差92.344ms）／67.290ms（7/7；最差56.796ms）；coarse对关闭仍快45.908ms（7/7；最差26.977ms）／65.197ms（7/7；最差6.830ms），这不能包装为对旧growth的改进。容量640的373.44MiB计算预留、896MiB物理KV保持，E49不额外施压、不做快照／逐出；默认容量仍512、预填充off。

E47为63个原生SAN场景／48真实CLI任务，E48为96个SAN样本／8个fresh shape，E49为72场景／48 CLI任务及最终640容量14,357-token Native、strict Stream、默认C/HTTP/CLI回归。三轮主记录1539／387／1539，现50份证据／35520主记录／37图，Metal附加记录共23465条（command／wait／batch分列）；主记录含warm／状态。旧47份证据保持原样，失败试验、负对与不能归因部分全部保留。[D-PREP §33](design/prewarm-v1.md)、[完整数据](benchmarks/README.zh-CN.md)。未推送GitHub。


## E45/E46 历史更新（2026-10-31）

旧E44的86.283ms bare负对无法追溯GPU／通知起止；多93.325ms prefill分散在四次prepare（22.717／11.694／32.119／26.795ms），每次owner CPU反而下降。E45同负载split／coalesce×bare／无marker／marker重测，没有复现86ms。持续bare最差慢1.908ms（工具＋3.871ms、prefill－2.058ms）；无marker trace最差慢11.248ms（工具＋14.212ms、prefill－3.054ms）；marker最差慢1.157ms，通知交付／marker后返回反少0.180／0.043ms。新负对可定位到对应阶段，但旧尾延迟仍未精确归因，不宣称已消除系统回退。45词持续的GPU跨度收益只有约2.5ms，合批不是各节奏都稳定获益。

计算缓冲区膨胀来自既有n_batch／n_ubatch耦合，llama.cpp按微批容量预留最大图。E46复用现有`--llama-batch 640`，较大初始输入继续分块、约600-token工具增量可合并，不改推理核心／固定runtime、不新增context／动态扩容。Metal计算预留373.44MiB，对比1024容量597.50MiB减少224.06MiB（37.5%）；只比默认512多74.69MiB，消除了原额外预留的75%。CPU计算24.03→15.02MiB，物理KV仍896MiB。这是初始化预留，不能称峰值RSS或KV下降。

E46新47／54词、0／1200万循环，7个block旋转三容量与模式、奇数block倒转词数顺序；21次模型加载、252评价＋252warm实际CLI任务。640关闭流式对默认512的任务收益：突发15.664ms（6/7；最差-9.195ms）／2.217ms（4/7；最差-176.227ms）；持续35.721ms（6/7；最差-12.543ms）／-2.180ms（3/7；最差-62.071ms）。640增长流式对默认512增长策略：突发27.527ms（5/7；最差-2.404ms）／-1.849ms（3/7；最差-168.364ms）；持续-24.934ms（1/7；最差-48.957ms）／-12.957ms（2/7；最差-217.550ms）。640关闭流式对旧1024：突发-6.090ms（3/7；最差-121.219ms）／-45.389ms（2/7；最差-156.310ms）；持续-8.158ms（3/7；最差-56.075ms）／-17.900ms（2/7；最差-111.176ms）。正值为640更快，负值为回退，全部保留；不能把重开模型的加载成本当在线换容量免费，容量不是只影响分块的单变量。

默认容量仍512、预填充仍off，640为显式可选配置。E45有47原生sanitizer场景／48实际CLI sanitizer任务，E46三容量72实际CLI sanitizer任务、640容量14,357-token Native、strict Stream和默认C/HTTP/CLI回归通过；性能无SAN且顺序独占。该阶段47份证据／32055主记录／34图，Metal附加记录共16477条；主记录含warm／状态，不能当独立延迟样本。旧45份证据保持原样，负结果和未复现均保留。[D-PREP §32](design/prewarm-v1.md)、[完整数据](benchmarks/README.zh-CN.md)。未推送GitHub。


E46新负结果进一步区分：640增长持续组对默认512中位反慢24.934／12.957ms，最差217.550ms中工具多190.983ms、producer墙钟多150.844ms、自身CPU多12.507ms、ready代理多153.688ms，最终prefill多30.848ms。marker最差640对512慢136.408ms：最终prefill多107.952ms，GPU并集多106.174ms，通知交付／marker后返回反少0.320／0.073ms，未解释提交延迟仅多0.148ms。可将这次新prefill尾部定位到设备执行跨度；GPU核效率、内部停顿、设备调度或频率状态尚未进一步识别，不能倒推旧86ms。640只作为显式内存／速度取舍，不升为推荐默认。

## E42–E44 历史更新（2026-10-31）

专用测试harness补Metal逐command／wait时间线，固定b10938运行时不改。同步等待主要与GPU命令执行重叠，commit→GPUStart的大部分区间与自己前序GPU执行重叠，不能全叫设备排队；CPU driver调度、GPU执行跨度、通知交付和host返回分列。E42 strict关闭组单wait GPU结束→返回最高14.006ms，全部带trace模式最高57.657ms；E43另加可关闭completion marker，53词突发累计GPU重叠中位655.283ms、通知交付最高8.584ms、marker退出→wait返回最高1.329ms。E43另一个43词增长突发单wait的16.376ms尾部可分为通知交付10.728ms和marker后返回5.648ms。后两段仍含驱动／handler／系统调度，不称纯CPU唤醒；未定位其他GPU队列或竞争PID，不能追溯精确分解旧44.039ms负对。

按实际设备工作优化有限尾批：E42看到588 token拆512＋76，76-token尾批仍占约29–37ms。E44在新45／51词、0／1400万循环任务比较同一1024容量下split512与可选合并<=1024尾批，不超预算、strict同步、单canonical／resident KV。关闭流式的整任务合并收益（突发45／51、持续45／51）为36.407ms（7/7，最差31.282ms）／28.445ms（7/7，最差11.714ms）／-3.341ms（3/7，最差-86.283ms）／22.623ms（6/7，最差-14.723ms）；已开启增长流式时为42.533ms（7/7，最差21.956ms）／29.019ms（7/7，最差0.537ms）／9.051ms（4/7，最差-20.868ms）／35.338ms（6/7，最差-56.910ms），负值即回退。合并后增长流式相对关闭，持续为152.803ms（7/7，最差125.405ms）／181.509ms（7/7，最差118.886ms），突发为1.718ms（4/7，最差-20.275ms）／-2.125ms（2/7，最差-14.450ms），全部配对保留。

`--llama-batch`默认512；尾批合并仍显式诊断策略。容量1024的Metal计算缓冲区597.50MiB，对比旧512容量298.75MiB，CPU计算缓冲区24.03对12.01MiB；物理KV均896MiB。当前对照两组都用1024，隔离分块效果，不证明扩容本身划算，也不是KV内存减少。observer／marker只在测试harness，bare性能组关闭采集；observer开销及E42数秒级producer／host异常全部保留。

三轮各1539主记录，该阶段45份证据／26421主记录／32图；另有9575条Metal附加记录（7179 command、2393 wait和3条metadata），以及此前500退出探针与2条忙／睡计量探针。E42/E43各30原生sanitizer场景、E44为47，每轮48实际CLI sanitizer任务；最终E44源码通过14,357-token合并Native、strict Stream与默认C/HTTP/CLI回归。当前实现／历史源码哈希分别归档，旧42份证据逐字节未变。[D-PREP §31](design/prewarm-v1.md)、[统一数据和负结果](benchmarks/README.zh-CN.md)。未推送GitHub。



E44剩余负对补充：45词持续关闭组最差反慢86.283ms，最终prefill多93.325ms（sync多84.062ms），工具墙钟反少20.539ms。该bare对没有设备trace，不能精确分配GPU／host原因。带probe的51词持续合并组GPU跨度少23.360ms、最终prefill少18.263ms，整任务却中位反慢11.240ms，工具墙钟多32.426ms；证明设备优化与工具／系统波动可以并存，不能宣布剩余回退已解决。

## E38–E41 历史更新（2026-10-31）

已把llama_decode调用和显式同步的wall／owner CPU拆开，加入producer compute/setup/output、Mach runnable／CPU计数、非自愿切换和同期独立producer对照。E38突发关闭组的最终同步中位519／669ms，ownerCPU不足0.4ms；本轮prefill最大减最小约22ms，主要随同步等待增加，旧71–110ms范围未复现。计时定位到等待已提交工作的阶段，尚未测Metal核函数起止，不能把全部等待称为GPU计算。

部分工具延迟已证实与系统背景CPU供给有关：模型空闲时，42词持续独立producer仍有56ms ready代理、非自愿切换中位790次。ready代理与非自愿切换相关，但runnable含运行态、计数有滞后，负代理值保留。55词独立背景较低，仍有较小模式相关变化；没有定位竞争PID，也未排除全部间接争用。M5原始proc计数须乘125/3，100ms忙／睡校验已单列。

新增显式off-drain／progress-drain，仅合并同次prefill内部屏障，公开返回前仍排空。E39新43/53词中位整任务收益：关闭2.3–3.9ms、增长1.3–11.8ms，每项4–5/7获益，最差仍慢44ms。53词突发sync虽少497ms，decode却多491ms，主要是等待转移。默认仍逐块同步；不将几百毫秒的计时搬移宣传为提速。

E40/E41补四状态竞争源控制：模型加载前也有27.2ms ready等待代理；70.6ms等待期间模型CPU仅0.42ms，系统侧CPU供给得到交叉支持，仍未定位竞争PID或排除所有间接历史效应。

E38/E39各1123主记录，包含每实验96条producer-alone；旧38份证据不改，该阶段42证据／21804主记录／29图（含E40/E41各211条诊断记录），另保留此前500退出探针与2条忙／睡计量探针。22／30个sanitizer场景、32／32个真实CLI sanitizer任务及24／24个独立producer控制通过；E39源码的14,357-token合并同步Native、完整strict Stream和默认C/HTTP/CLI回归通过。[D-PREP §30](design/prewarm-v1.md)、[完整数据与负结果](benchmarks/README.zh-CN.md)。未推送GitHub。

## E36/E37 历史更新（2026-10-31）

完成同负载off／休眠线程／owner空操作／仅分词／真实预填充隔离，新增逐次prepare进程CPU、owner CPU与decode次数。相同token／8次decode、无消费者时，累计prefill仍有71–110ms范围波动；后续prepare发生在消费者join后，不能把这些负对全归为直接抢owner。工具本身的CPU／非CPU时间也会突增；间接争用与设备／系统调度仍未完全细分。

新增显式`lazy-race`，首次合格输出增长才创建消费者。新42/55词任务，42次评价突发调用的消费者由42个降到0个，但每三工具任务原构造仅约0.049–0.079ms；按需版相对原增长策略，突发反慢2.556／1.157ms、持续反慢8.057／1.786ms，全部保留为负结果。按需持续输出相对关闭仍快141.552／186.168ms（6/7、7/7），没有证明比原增长策略更快。默认不切换，lazy仅可选资源实验，不将其包装为已解决大回退。

E36/E37各1283／771主记录；13／22个原生sanitizer场景和40／24个真实CLI sanitizer任务、最终默认回归通过。旧36份证据保持原样，当前38证据／19136主记录／26图，另有此前500条诊断探针。细节见[D-PREP §29](design/prewarm-v1.md)及[完整报告](benchmarks/README.zh-CN.md)；本轮仍未验收Windows/CUDA、GPU硬件计数器、云SSE或自主模型任务能力，未推送GitHub。

## E33–E35 历史更新（2026-10-31）

已补producer CPU/墙钟、EOF回收和worker排队归因，新增可选macOS事件等待及ESRCH退出竞态的1ms有界重试。默认不切换。E35每组84次调用的最大EOF等待由原poll/event约12ms降至修正版约1.26ms；完整任务仍有负对，不能当作稳定端到端加速。

新46/54词持续任务中，执行器修正单独相对原关闭组快40.677／48.196ms；在同样修正的执行器上，增长预填充另快152.085／138.815ms（7/7、6/7）。突发没有稳定收益。旧36词慢32ms未稳定复现，新的最差负对仍有启动、producer和推理波动。当前36证据／17082主记录／24图，另存500条诊断探针，历史证据不覆盖。

当前策略与验收见[D-PREP §28](design/prewarm-v1.md)，逐对数字／负结果见[成果报告](benchmarks/README.zh-CN.md)。显式profile中选择`progress-race`＋prefill on；`off-race`＋prefill off为执行器控制。Windows/CUDA未验收；最终sanitizer、真实CLI及默认回归通过，未推送GitHub。

更新：2026-10-31。当前实现清单；后续新增、改动与撤回都在这里更新，阶段历史见 [变更记录](benchmarks/CHANGELOG.md)。性能数字以 [成果报告](benchmarks/README.zh-CN.md) 和 [证据目录](benchmarks/data/catalog.json) 为准。

我们已经建立可选的 llama.cpp 原生上下文后端、Qwen3 聊天／工具桥、KV 文件恢复宿主模块和流式输入流水线。原生 CLI 可显式启用；E21在原生CLI显式选项下接入test.run工具窗口；E22补阶段观测，E23提供父进度ETA和macOS26+ spawn后端；E24完成整任务归因并提供可选提前释放；E25隔离unlink／最后close，E26定位新程序首次执行pre-main等待并验证显式增量构建命令；E29显式Native输出通道把test.run真实stdout接到同一canonical会话预填充；独立Stream仍由宿主调用，默认HTTP路径保持原样。

## 接入方式与验证范围

| 层次 | 当前方式 | 已验证范围 |
| --- | --- | --- |
| 推理引擎 | 直接链接 `libllama` / ggml，聊天桥另用上游 common/Jinja | 固定 llama.cpp b10938，commit `f1e44dcc11d8802d107bd7331a3d3fd3e6f57b93` |
| 模型 | GGUF 加载、tokenize、prefill、greedy 生成 | Qwen3-0.6B-Q8_0；聊天关闭 thinking；已验收模板才启用准备快路径 |
| 系统与设备 | 本机 macOS / Metal | Apple M5、16 GiB 统一内存；原生 Windows/CUDA 待验收 |
| 默认 Agent | 既有 C 工具执行器与默认 HTTP 路径 | 工具链执行回归通过；默认构建不增加 libllama/C++ 依赖 |
| 原生入口 | 可选 `agentctl-native`、上下文C API、KV/Stream及工具准备宿主API | E21显式test.run窗口集成；通用策略整合仍未完成 |

适配代码在 cxf 一侧，调用 llama.cpp 已有接口；没有修改上游推理内核。底层会话和 token 块在 `src/llama.c`，聊天、KV worker、Stream 等可选宿主实现目前位于 `tests/`，由专用构建脚本链接。它们还不是统一安装的跨引擎 SDK。common/Jinja 部分依赖固定上游源码／ABI，不能假定升级版本仍兼容。

## 已实现适配清单

“已验证”指所列固定模型、版本和本机环境下的结果；宿主 API 可调用不代表默认 Agent 已自动使用。

| ID | 适配 | 具体行为与入口 | 当前状态 / 证据 |
| --- | --- | --- | --- |
| A01 | 原生推理后端 | `cxf_llama_open`、session、tokenize、piece、prefill、greedy；公开 C 头不暴露 llama 头文件 | 可选模块已验证；E03–04 |
| A02 | 单份规范 token 历史 | 每个引擎一个 cxf token 块池；session 持句柄和块视图；追加仅复制新后缀 | 核心已验证；E03、E07–09 |
| A03 | 显式变更与回退 | `append` / `replace(keep, suffix)` 加 revision；拒绝过期／跨引擎／复用句柄，失败不发布半次修改 | 核心已验证；E03 |
| A04 | 同引擎前缀分支 | `fork` 共享 token 块，调用 `llama_memory_seq_cp` 共享已计算前缀 KV；分支后缀独立追加 | 核心已验证；E01、E03、E07–09 |
| A05 | 预算预填充与采样状态 | token budget、最小批量、结束 flush；区分 committed/evaluated；必要时只重放最后 token 以恢复该 session 的 logits | 核心已验证；E03、E10–11 |
| A06 | Qwen3 聊天／工具协议 | 上游 Jinja 模板与工具输出解析器；映射到既有 action、权限与工具执行器；有界生成 | 原生提供者／CLI 已验证；E04–06 |
| A07 | 聊天稳定检查点 | 保存 generation-header 前的字节／token 边界及前缀哈希；后续只分词并替换后缀；前缀改变则重建 | 已验证；E04–05；仍完整渲染及校验前缀 |
| A08 | 模板准备缓存 | 缓存已验收模板的解析／工具语法元数据；工具 schema 改变使缓存失效；不缓存第二套对话历史 | 已验证；E06；未知模板快路径不自动启用 |
| A09 | 前端内存与复制缩减 | 提前释放临时对象；借用 transcript visitor 跳过完整请求 JSON；暖态直接 Jinja 值与结果收集；分词缓冲不足才重试 | 已验证；E07–09；部分尺寸无改善或回退 |
| A10 | KV 私有文件保存／恢复 | `cxf_llama_kv_save_file` / `evict` / `restore_file`；快照不附带 token 列表；只恢复同一存活引擎／会话／修订 | 私有边界及宿主接口已验证；E10；无外部导入／崩溃恢复 |
| A11 | 提前恢复的 owner worker | `AgentKvWorker` 单 owner 线程；异步 prepare ticket、poll、cancel、finish；可与宿主装配重叠 | 宿主模块已验证；E10；逐出和提前量由调用者决定 |
| A12 | 流式输入与持续预填充 | `AgentStream` 接收有序 NDJSON，重组任意线缆字节分块；完整语义单元到达后合批预填充 | 宿主API／回放及E28真实进程常驻计时通过；持续产出有收益、突发无稳定收益，尚未接厂商SSE |
| A13 | 完整文件处理与版本收据 | `file_end` 后运行配置的文件处理回调；输入结束前可取收据、发送新版本；旧收据标 stale，最终提示只收当前版本 | 宿主 API 已验证；E11；最终 CLI 回调仅有界预览 |
| A14 | 验收与成果归档 | 独立全文分词／greedy oracle、真实工具链、取消／资源边界、ASan/UBSan、分组性能和负结果 | 当前33份证据；统一报告、CSV、SVG/PNG与发布清单可离线复核 |
| A15 | 本机预填充耗时估算 | 固定 64 条数值窗口、工作量特征拟合、profile 隔离与在线纠正；`include/cxf/cost.h` / `src/cost.c` | 独立模块与 E13 实测通过；不改变推理或自动调度 |
| A16 | Roofline 结构估算与残差层 | GEMM FLOPs／量化字节／GQA，逐组 Roofline；同后端微测与可选三系数校准；`include/cxf/roofline.h` | E14–17速率／算量／形状；E19新增raw残差与等量coverage对照，cap64冻结仍回退；E20新增 `cost_kalman.h` 可选40B残差层，C回放／EWMA对照；shape-online本轮MAE改善约3%，区间失准；复杂成本API未接调度，工具窗另用E23阶段EWMA |
| A17 | 真实工具窗口准备 | `tests/agent_tool_prepare.h/.cpp`：单worker／借用Native，test.run期间late／立即／EWMA定时／父进度ETA；resident／重算／恢复，epoch取消排空；macOS26+用spawn | E21–E28验收；E27正常resident包装无稳定额外收益，已有前缀均复用。E28新输入流水线属独占Stream；E29在本worker新增真实CLI工具输出流，不含构建优化；不改默认，通用冷启动／多消费者未接 |
| A18 | 真实工具输出增量预填充 | 显式Native CLI通道、常驻同session、4KiB有界后缀、BPE复核接纳、取消排空 | E29已验收；持续输出有收益、突发回退，默认off；不含通用Stream/tools整合 |

## E29：真实agentctl工具输出流（2026-10-31）

新增显式`--tool-output-prefill off|on`（需`--llama-model`与`--test-command`、resident KV）；REPL空闲时可用`/tool-output-prefill off|on`切换同一通道。`test_run_exec_output`观察实际保留stdout/stderr前缀；`agent_tool_prepare_output`以latest-only4KiB队列、512新字节合批，单owner异步调用Native工具后缀预填充。规范token和已评估历史KV留在原session；最终正常模板／前缀guard与BPE LCP核对后接纳，只回退本次不匹配尾部。没有快照、主动逐出、第二个推理context或完整历史副本。仍保留原文和完整渲染临时量，不承诺全链路仅一个表示或物理KV缩容。

显式off/on共同采用正文优先、最终状态在尾部的收据，并在入站补缺失LF，使首轮用户消息边界稳定；不指定选项时保持原收据与默认行为。当前只支持固定test.run空参数和已验收Qwen3模板，非通用工具／云SSE；失败准备仅诊断，不重跑真实工具。默认不开启，recompute/restore组合拒绝。

E29经真实CLI REPL／registry／AgentStore／test.run路径：每任务三次实际工具、四次Native准备＋首greedy，动作选择固定。32／56词持续产出的完整任务逐对节省95.254／183.960ms，均7/7更快；突发输出反慢43.004／20.590ms（0/7／2/7更快）。额外预填充执行46.853–169.079ms／三轮任务，持续产出工具墙钟还增加21.597／97.364ms；不能只展示等待下降。515记录包含8预热＋56评价任务、192工具调用、256全文token oracle；13项独立正确性／fresh-greedy、16个CLI ASan任务、Native14K／原Stream／默认回归通过。数据和负结果见[成果报告](benchmarks/README.zh-CN.md)，规范见[D-PREP§25](design/prewarm-v1.md)。

## “上下文一份”目前做到哪里

| 表示 | 当前所有权／处理方式 | 仍保留的开销 |
| --- | --- | --- |
| 原始任务／工具事件 | 应用 transcript 或 Stream 的有界源事件存储 | 仍需要可重渲染、可审计的原文 |
| 规范 token ID | 每个引擎一个 cxf 块池；共享分支借用同一前缀 | 部分裁剪块尾部可暂留少量 ID；不是任意全文副本去重 |
| KV | llama.cpp 管理的同一 context；同池序列前缀可共享 | 预分配物理 KV buffer 大小未变；原文／ID／KV 仍是不同表示 |
| 聊天转换临时对象 | 借用 visitor、短生命周期对象、已验收模板快路径 | 完整模板输出、前缀哈希、部分上游对象复制仍存在 |
| KV 快照 | 私有临时文件，token_count=0 | 文件间可能重复共享前缀；保存与搬运本身有成本 |

底层 token API 已由调用者明确给出 `keep`，不走 llama-server 的完整输入前缀匹配。聊天桥为了验证渲染语义仍保留 O(n) 的完整渲染和前缀哈希，因此目前省掉了重复全文分词及引擎侧推测边界，并未消除所有线性前端工作。

## 工具闭环的接入程度

原生提供者沿用已有文件工具和 `test.run`：读文件 → 获得真实失败测试 → 写修正 → 获得通过测试 → 回读。脚本指定动作的执行链已经验证；真实 0.6B 单文件读取通过，代码修复探测未通过。工具链正确性与模型自主决策能力分别记录。

KV worker 与 Stream 给宿主提供提前行动的接口。宿主可以在装配上下文时提交恢复提示，也可以在接收完整语义单元时预填充／处理完整文件。E21新增显式原生CLI的test.run包装：工具执行期间按late／立即／EWMA定时准备，支持resident／重算／私有文件恢复。E23另有父进度ETA（--tool-prepare progress），默认timed不变；macOS26+进程启动改用posix_spawn。E24显式--tool-snapshot-release early可将已恢复快照的删除提前，默认return；短／长任务相对原progress逐对中位仍慢313.434／4.653ms，不升默认。尚未实现多工具／子Agent的通用计划、会话切换、容量或冷启动策略。

## 尚未完成的适配

| 项目 | 当前缺口 | 后续验收需要回答的问题 |
| --- | --- | --- |
| 通用 Agent 自动调度 | E21已接可选Native串行test.run；KV/Stream、HTTP及多消费者尚未统一编排 | 同一任务能否自动发提示、处理修订／取消、回收资源；保留默认行为与正确性 |
| 云端真实流接入 | 只有中立 NDJSON 回放；未接真实 SSE／tool-argument 增量 | 何时形成不可变单元；未闭合 JSON、Unicode、改写和断流如何处理 |
| 上下文压缩／摘要交接 | 可显式 replace／fork；尚无自动压缩和 KV 迁移策略 | 摘要改变前缀后怎样正确重建；何时值得预取／保存 |
| 事件级增量模板 | 每轮仍完整渲染与哈希 | token／工具语义保持一致时，能否进一步降低长历史的线性成本 |
| 原生 Windows/CUDA | 本机 macOS/Metal 已验收，第二台机器待运行 | 文件、进程、动态库、工具链 ABI、CUDA 同步和搬运收益是否成立 |
| 其他模型／模板／引擎 | 已验收 Qwen3 固定模板与 llama.cpp 固定版本 | 每种模板的稳定边界与工具协议；不同模型的等价性和资源开销 |
| 物理 KV 缩容／分页 | 当前释放逻辑单元供复用，物理预分配缓冲不缩小 | 是否需要引擎侧新能力；冷存储、PCIe 与共享前缀持久化成本 |
| 提前意图与预热时机 | E21–E26工具窗口用EWMA／可选父进度修正及提前释放；未接通用system/tools意图、多消费者deadline或异步冷启动 | 稳定前缀正确性；晚到、过早占位、作废计算与争用成本能否同时降低 |
| 稳定性和能力规模 | 暖态、单机、少量样本；0.6B 仅作载体 | 冷 SSD、长任务、多模型争用、尾延迟与较大模型任务结果 |

接下来的工程顺序建议：E29已接原生test.run真实输出，下一步扩展来源／策略前继续核对时序、最终模板和取消边界；Windows/CUDA 可在第二台机器上独立验收。新的调度必须同时报告保存成本、实际重叠、总耗时和无收益场景。

<a id="后续设计提前意图与按需预热未实现"></a>

## 后续设计：提前意图与按需预热（部分实现）

按用户调整，已先独立实现 [D-COST 耗时估算](design/cost-estimator-v1.md)：63 条标定、84 条评价；平均绝对误差从固定吞吐 68.59 ms 降至冻结 15.99 ms、在线 10.44 ms。E14原始负结果保留；E15已排查到孤立图墙钟速率／算子错配，补连续图差分、实际FA与最后FFN算量。E15旧式raw／修正raw／校准的MAE为240.283／14.220／9.133ms，同轮原frozen／online为10.117／6.434ms。E16／E17补了batch上限与context拆分、公平online、19/20边界和时间插值；E17形状online MAE为cap512的2.793ms、cap128的1.926ms，同轮原online为3.753／3.335ms，冻结形状校准仍有回退。E19再分开表覆盖／样本覆盖／目标与basis，新增3参数残差API；新cap256 residual/diverse MAE3.452ms，cap64为2.645ms仍差于raw2.183ms；不默认替换。E13–E20仅比较预测；E21新增工具窗口简单EWMA，复杂成本模型仍独立，完整P0–P4仍未整体实现。

详细方案统一维护于 [D-PREP v1](design/prewarm-v1.md)：版本化环境通知、不可变源借用、
稳定前缀候选与原子交接、按预计提交派发、容量／过期、异步冷启动和 P0–P4 验收。
正文默认等提交，输入开始只作软信号；前缀就绪和完整请求就绪分开计量。

当前Stream仍同步open、tools为空；Native已有独立／借用两模式；E21借用同一worker engine，worker仍为FIFO。
这些差异在 [缺口审计](design/audit-20261030.md) 中逐项标明，E21仅补齐借用Native与串行工具窗口子集；其余仍不能计入已有实验成绩。
所有当前／待实现契约的维护位置见 [设计入口](design/README.md)。

## 文件与文档入口

| 内容 | 当前位置 |
| --- | --- |
| 核心 C API / 实现 | `include/cxf/llama.h`、`src/llama.c`、`src/llama_internal.h` |
| 原生聊天／消息桥 | `tests/agent_native.h/.cpp`、`tests/agent_native_messages.h/.cpp` |
| KV owner worker | `tests/agent_kv_worker.h/.cpp` |
| 真实工具窗口 | `tests/agent_tool_prepare.h/.cpp`、`tools/bench_tool_prepare.py`、`tools/test_tool_prepare_cli.py` |
| Stream / 回放入口 | `tests/agent_stream.h/.cpp`、`tools/test_stream_replay.py`、`docs/examples/local-stream.ndjson` |
| 可选构建 / 验收 | `tools/build_agent_native.py`、`tools/test_llama_context.py`、`Makefile` 中的可选测试目标 |
| 设计契约 | `docs/llama-context-v1.md`、`docs/llama-chat-v1.md`、`docs/context-memory-audit.md`、`docs/basic-agent.md` |
| 数据与后续更新 | [成果报告](benchmarks/README.zh-CN.md)、[方法](benchmarks/METHODOLOGY.md)、[维护规则](benchmarks/MAINTENANCE.md)、[变更记录](benchmarks/CHANGELOG.md) |

最近更新已新增A15–A17和E13–E28；先前文档／设计整理没有冒充运行时交付。E01–E12/L01继续对应原实测版本，未覆盖旧证据。

E24完整任务归因：旧长任务中位退化15.058ms对应同一rep的工具+8.354、工具前+8.207（保存为主）、工具后−3.517ms，其余为真实修正及记账。新512/2304词与乘法修复／rotate-xor任务，659新记录、492调用的12fresh参考；直接回调最大6.458μs。提前drop使工具后暴露清理均值2.308→.648／4.004→3.346ms，却未稳定改善完整任务，负结果与间接争用未完全归因边界保留。详见[D-PREP §19](design/prewarm-v1.md#19-e24-当前结果与保留边界2026-10-31)及[E24报告](benchmarks/README.zh-CN.md#e24--完整任务归因与提前清理)。

E25/E26快照用途与归因：默认Native工具窗resident不save／evict／restore，文件只用于显式容量策略；当前snapshot仅同一live engine/session/revision，非冷重启，逻辑逐出不缩小预分配物理KV。E25用retain fd区分unlink和最后close，313ms系统性退化未复现，提前删除仍有负对。E26新程序首次运行的spawn返回到main逐条中位277.827ms，立即rerun的pre-main仅1.788ms；增量命令避免无变化第三轮重编译，任务逐对少327.722ms、7/7更快，测试仍实际执行，不自动改用户命令。详见[D-PREP §22](design/prewarm-v1.md#22-e25e26-最终结论与使用范围2026-10-31)和[E25/E26报告](benchmarks/README.zh-CN.md#e25--快照删除与资源隔离)。

E27/E28常驻结果：本轮不调用save/evict/restore，任务内保留KV，无.kv文件，生产API／默认未改。六轮真实配置修复都只算新增397token，resident包装相对直接工具的短／长任务逐对节省−14.293／+5.014ms，仅2/7／4/7更快，无稳定额外收益。真实进程持续输出完整语义单元时，合批／急切完整任务逐对节省136–234ms，各7/7更快；额外小批次工作、producer变慢及正常close已计费。突发输入略慢，负例保留。不是厂商SSE或Agent默认自动流式接线，与E26构建命令优化分开。见[D-PREP §24](design/prewarm-v1.md#24-e27e28-常驻结果与当前选择2026-10-31)及[E27/E28报告](benchmarks/README.zh-CN.md#e27--正常常驻多轮工具流程)。

## E30当前策略变更

显式输出on通道增加固定10ms首次观察窗，并在工具结束后停止消费积压队列；最终正常prepare负责剩余正文。保持同一常驻session、原收据和512字节阈值。新策略结果以E30为准，E29仅描述先前策略。分阶段观测显示E29突发额外prefill明显大于前端和非join包装；尚未用仅分词／仅线程消融建立精确因果比例。验收与新负载结果见统一报告，完整契约维护于D-PREP§26。

E30新任务实测（逐对off减on，ms）：

|词/段|循环/段|任务节省中位[min,max]|更快/7|额外prefill|
|---|---|---|---|---|
|40|0|-9.710 [-54.902,15.563]|2|15.157|
|40|30000000|188.140 [36.308,222.913]|7|89.427|
|48|0|11.535 [-22.266,28.884]|5|0.387|
|48|30000000|282.308 [243.066,293.666]|7|32.678|

13项ASan、16任务真实CLI验收通过；515正式记录、负对和源码ZIP保留。旧30证据与上一发布ZIP逐字节一致。突发组工具墙钟波动会贡献任务差异，不能将正中位全归给策略；同负载旧on及仅分词／仅线程消融尚未测。新数据支持继续保留显式策略，不保证所有短任务更快。

## E31/E32 historical results (2026-10-31)

已补关闭、E29旧策略、E30计时策略、仅线程和仅分词的同负载对照，并用新任务验证增长策略。增长策略避免了42次突发调用的无用推测，但对计时策略仍有负结果，保持显式可选；下表和统一报告保留完整范围。当前不能宣称已稳定降低所有任务的尾延迟。

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
