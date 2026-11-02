# 基本 Agent 工具闭环

E62：默认system按实际权限加入整文件参数提示，只读／append-only不带写整文件提醒；保持每轮单工具与512B预算。显式--trace增加仅数值／注册名的provider和工具计时，工具exec与外层策略分开；默认不开新增日志。云端relay编码、usage／缓存和计时口径由[D-HYBRID](design/hybrid-v1.md#e62-cloud-reproduction)统一维护。

E59–E61当前：fs.read返回实际fd元数据版本，--write新增fs.edit唯一多行块编辑、版本前置条件与冲突回执。34项状态／安全、4组CLI、完整回归和Native14K／分叉兼容通过，真实云DeepSeek Flash的正常、冲突恢复、只读三例独立核对通过。64KiB成功编辑约多5.6–5.8ms，主要与完整正文策略扫描量级一致；未撤除检查。接口／边界只由[D-EDIT](design/versioned-edit-v1.md)维护，数据见[成果报告](benchmarks/README.zh-CN.md)。

E58新增独立只读自预测实验宿主，未接默认agentctl：只允许fs.read，候选结果经正式调用精确匹配、epoch、当前权限和文件版本校验后采纳，否则回退。普通fs.read复用原逻辑；版本证据仅投机读取显式请求。24项状态／安全及8项Native验收通过（含2项注入流程），尚无模型命中率／提速结论。见[D-SPEC](design/self-speculation-v1.md)与[异机交接](self-speculation-handoff.md)。

E56/E57当前：生产test.run接口与默认权限保持E55，只有可选tools/run_cpp_batch.sh任务私有PCH配方，新增6组正常／失效／泄露／网络测试。旧绑定目标版本漏检已在[D-EXEC](design/test-sandbox-v1.md)修正比较口径；配方不是新增agent能力或推理加速，分组候选不启用。

E55当前补充：启动者可同时指定--test-toolchain BIN_DIR和--test-sdk SDK_DIR，只绑定子环境PATH／SDKROOT，不增加目录、IPC或网络权限；无效参数不执行，不自动替换编译器。默认profile／日志与宿主保护保持E53。561项C／15组OS及原回归通过；新任务与负结果见[D-EXEC](design/test-sandbox-v1.md)和[成果](benchmarks/README.zh-CN.md)，推理收益另算。

> 执行链验收与真实模型失败探测分列于 [成果汇总](benchmarks/README.zh-CN.md#正确性能力与未验证项)（E04、E12）；失败记录保留，不合并为实现通过率。

2026-10-30：先做可执行的基本 Agent，再适配本地推理引擎的上下文与 KV 生命周期。
本阶段不引入记忆系统、规划框架或新的调度层。

## E53进程隔离（2026-11-01，当前）

`test.run`默认OS限制以[D-EXEC](design/test-sandbox-v1.md)为准：当前macOS工作区只读、私有TMP可写、后代禁网。启动者显式用`--test-read DIR`／`--test-write DIR`（各4个）及`--test-net allow`授权，敏感文件保护不因此解除。`--test-isolation off`才整体取消OS约束；无后端或准备失败不退回裸执行。最小环境和宿主结果保护继续生效。

542项C、12组系统权限、原12／8组套件及Native／Stream／输出回归通过。cxf自身环境先验证省略再启用命令，未覆盖其他同UID进程或argv。最终开销见[E53](evidence/test-sandbox-host-20261101.json)：完整CLI中位空工具多15.724ms、CLT多3.565ms、标准cc多2467.548ms，不能称固定或近零成本；E52初版旁路与负结果保留。用于确定性oracle的Stream夹具显式选择CLT，生产命令仍由部署决定。框架自身包含网络／沙箱测试的`make test`在宿主执行；不能假定嵌套沙箱或无网络的测试命令天然兼容。

## E51 统一数据准入（2026-11-01，历史边界）

当前文件／外发规则以[D-DATA](design/data-policy-v1.md)为唯一契约：8个文件工具均检查敏感名称、实际配置／trace路径与inode，拒绝链接与非规范路径。grep/list跳过保护项；写权限不能覆盖敏感路径。普通文本源码仍可读—改—测。

CLI为工具统一加参数／结果准入，在结果进入Agent transcript前拦截已知秘密；HTTP出口检查请求和响应，模型路径另检查解码后的动作。云／UNKNOWN默认为deny，任务文本和工作区上下文由启动者分别授权；task不公开工具、不执行其CALL。Native及显式本地辅助仍执行内容保护。没有自动删词、切云或重试；已执行命令的副作用不因结果拦截而回滚。

445项策略单测和12组安全集成套件、原8组基础套件、Native14K／Stream／工具输出均通过；[E51证据](evidence/agent-data-policy-20261101.json)是功能验收，不是性能或模型能力。test.run固定命令仍无OS沙箱，保护规则不能约束其全部系统调用或识别任意未知秘密。

## 2026-11-01 基础能力增补（E50历史边界）

本轮只增加可由确定性测试验收的执行机制，不以小模型任务能力作为通过条件，不新增性能结论。

- `fs.replace_line`：仅随 `--write DIR` 挂载，沿用 WRITE 权限。参数为 `相对路径\n完整旧行\n完整新行`，新行可空，旧行不可空，不带末尾额外换行。按字节匹配整行，忽略文件行尾的 LF／CRLF 差别并保留原行尾；仅恰好一个匹配时替换。零／多匹配返回 `#edit status=not_found|ambiguous matches=N`，明确不写；相同新旧行返回 same。结果是事实收据，不能把 handler OK 视为已经编辑。
- 编辑最多处理 64 KiB 普通文本，拒绝 NUL、硬链接、非普通文件、路径中的空段／`.`／`..`／控制字节及符号链接组件。沿目录 fd 打开父目录，在同一父 fd 下创建排他临时文件、写入并 fsync，再 renameat 替换；仅保留普通权限位，不复制 owner／ACL／扩展属性。旧文件在失败时保留，清理临时文件；不创建目录或目标文件。输出容量不足在读写之前拒绝，落盘后不返回 FULL、不触发扩容重跑。
- 编辑前后 stat 检查尽力识别外部并发修改；这不是文件系统原子 CAS，也不是 OS 沙箱。E50时旧write_apply／append尚未统一迁移；E51已将文件工具接入D-DATA的目录fd与敏感路径规则，完整进程隔离仍待做。
- `test.run` 的 `TestRunCfg` 追加环境策略：0 保留原调用者继承环境语义，1 使用固定最小环境。实际 CLI 默认选择 clean，仅提供固定 PATH、LANG、LC_ALL、TMPDIR；`--test-env inherit` 是启动者显式放宽，模型无法更改。两种进程后端使用同一 envp，不能在 fork 子进程中修改父进程环境。该措施减少 API 凭据误继承，不限制子进程文件／网络权限，不阻止它主动读取本机配置。
- 本地文本辅助能力另见 [D-HYBRID](design/hybrid-v1.md)，作为现有工具循环的普通数据工具，不创建第二套规划／执行器。

验收：唯一／零／多行匹配、LF／CRLF／无末尾换行、空新行、重复请求、容量不足、路径与链接拒绝、内容／权限／临时文件保留；clean/inherit 在 fork／可用 spawn 后端的合成凭据隔离；实际 CLI 注册权限与端到端读—改—测闭环；主模型—本地辅助—主模型的错误、预算和数据隔离。旧证据不可改写，新增功能证据单列，Native 兼容与真实模型能力分开记录。

## 工具与部署

| 工具 | 挂载条件 | 契约 |
|---|---|---|
| `fs.list` | `--root DIR` | 列直接子项；参数 `.` 或相对目录，名称排序；有界输出明确报截断 |
| `fs.read` | `--root DIR` | 读文本；每页最多4096B；回执version用于fs.edit，next为后续字节偏移，-1表示读完；读后核对版本 |
| `fs.grep` | `--root DIR` | 既有子串检索 |
| `fs.diff` | `--diff DIR` 或 `--write DIR` | 既有只读差异预览 |
| `fs.write_new` | `--create DIR` | 既有新建，不覆盖 |
| `fs.write_apply` | `--write DIR` | 既有整文件替换／创建 |
| `fs.append` | `--append DIR` | 既有追加 |
| `fs.replace_line` | `--write DIR` | 唯一整行匹配才替换，保留行尾，明确报告 applied／same／not_found／ambiguous |
| `fs.edit` | `--write DIR` | 复制read版本，单文件唯一精确多行块；applied／same／conflict／not_found／ambiguous，冲突先重读；参数仍最多1KiB，文件64KiB |
| `llm.local` | `--local-config FILE` | 回环文本辅助，每任务最多两次，无工具／执行权限；详见 D-HYBRID |
| `test.run` | `--test-command COMMAND` | 模型参数必须为空；命令由启动者固定，工作目录为 `--root` |
| `http.post` | `--net HOST:PORT` | 既有向固定服务发送 |

`fs.read` / `fs.list` 用目录描述符逐段打开路径，拒绝 `..`、绝对路径和符号链接组件。
`fs.list` 最多展示 128 项，路径名含控制字节的项会省略并计数；大目录明确报告未展示的项。
`fs.read` 面向文本，不读二进制；UTF-8 页边界不切断字符。工具不创建额外持久会话或全文副本。

`test.run` 是显式启用的进程执行能力，按WRITE信任级挂载，当前默认OS边界见D-EXEC。
启动者应把编译／测试所需的完整命令作为部署事实提供。它可能执行工作区里的代码，
当前macOS进程受到独立文件／网络权限，文件工具的--write不自动授予test.run写权限。CLI默认最小环境，--test-env inherit仅继承值（隔离时剥离loader注入变量）；显式off才取消OS边界。
命令合并捕获 stdout/stderr，保留有界前缀；超限有 `#truncated`，不会堵住管道。
默认超时 30000 ms，可通过 `--test-timeout-ms` 配置；超时终止进程组并收割子进程。
启动前输出缓冲不足返回 RANGE，不执行；执行后不返回 FULL，避免框架扩容导致重复执行。
`#test` 的 exit、signal、timeout、io_error 均是模型可见事实；不能只看 handler 返回 OK 判定测试通过。

实际 `agentctl` 不再注册报价／车票等演示能力；`test_agent` 的演示 bundle 保持原样。
内部入口 `agent.step` 保留在运行时目录，但不再出现在模型的工具清单，避免模型嵌套
调用它而丢失当前任务上下文。工具清单由实际注册成功的公开能力生成。
写入族必须有路径与正文之间的换行分隔符；只有路径报 FORMAT，显式 `path\n` 才是空内容。

## 使用

```sh
./cxf.sh --model-scope local --write /path/to/project --root /path/to/project \
  --test-write /path/to/project --test-command 'DEVELOPER_DIR=/Library/Developer/CommandLineTools make test' --test-timeout-ms 30000 --rounds 16
```

任务示例：读取有关文件，修正一个错误，运行已配置的测试；失败则依据真实输出修正，
通过后回读修改并汇报。没有配置 `test.run` 时应当明确说明未做执行验证。

## 验证与进入下一阶段的条件

- `make test` 包含基本工具单测与本地 HTTP 端到端回归，不需要云端或密钥。
- 新端到端用例真实读取和改写 C 文件、调用系统 C 编译器：先得到失败测试，再修正、
  重测通过并回读；断言每轮工具结果实际进入标准 tool messages，且没有扩容重复执行。
- 模型决策使用确定性 HTTP 桩；这证明执行链，不证明模型具备自主修复能力。
- 真实本地模型仍须用相同类型任务验收，并记录终值、轮数、失败与资源开销，作为引擎适配前基线。

已找到并试跑本机 `Qwen3-0.6B-Q8_0.gguf`，配套 llama.cpp build 10938。两次试验都未通过：
第一次缺正文写入把文件清空、编译失败却口头声称通过；第二次能继续尝试，但没有产生
可编译的最终源码，并调用了内部 `agent.step`。因此补了分隔符校验和公开工具清单隔离。
最后一项清单隔离后未再做模型试跑；Mac 测试模型服务已停止，较大模型验收转移到另一台机器。
证据分别保留在 `build/qwen-basic-p9yiig_z/` 和 `build/basic-agent-live-j7kvvv9q/`。
可复现验收脚本为 `python3 tools/check_basic_agent_live.py --url URL --model MODEL`，
独立编译并检查磁盘产物，不以模型最终回答作为通过依据。

现有边界仍在：REPL 每条输入创建独立任务；请求体 16 KiB、单工具参数 1 KiB、
默认最多 8 轮。当前适合小文件和短任务；超过容量明确失败，不代表通用编码 Agent 已成熟。
持久对话、流式、运行中用户取消、任意 Shell、引擎共享上下文均不在本轮实现范围。

## 2026-10-31：可选Native工具输出通道

`agentctl-native --llama-model MODEL --root DIR --test-command COMMAND --tool-output-prefill off|on`显式建立可流式收据通道，仅resident KV；off/on共同采用保留正文＋LF＋最终`#test`状态尾和入站补缺失LF。不加该选项仍是原header-first收据。命令仍由部署固定、模型参数必须空，容量不足先拒绝、不因流式准备失败重跑；截断／超时／exit事实完整保留。开关只影响有界输出提前预填充，不影响权限或提前发布工具结果。详细边界／取消在[D-PREP§25](design/prewarm-v1.md)，E29性能和模型能力失败分别展示；常驻KV不需要快照。

## E50 验收与使用（2026-11-01）

基础编辑／环境的 ASan/UBSan、8组双端点 HTTP 桩在 sanitizer CLI 上的功能验收，以及默认完整回归均通过。Native 14,357-token、16项 Stream 与72项工具输出检查通过；固定运行时库未插桩，Native harness 的6条已有编译警告保留。没有模型任务质量、真实云端或新性能结论；旧性能版本未重测。[E50 功能证据](evidence/agent-foundation-20261101.json)。

```sh
# 主端点仍使用自己的 agentctl.json；本地辅助配置只由启动者提供。
./cxf.sh --root /path/to/project --write /path/to/project \
  --test-command 'make test' --test-env clean \
  --local-config /path/to/agentctl.local.json
```

辅助配置仅含 api.url、api.model、可选空 key 和 timeout_ms，端点只收精确回环 IPv4。它与主配置和认证独立，使用者自行填写；不要把本地配置加入仓库。单行编辑仍需既有 `--write`，不会因启用本地辅助而获得写入或执行权限。

E51已补默认敏感路径和云端外发准入，实际内容打开统一走目录fd。E52已补macOS进程文件／网络隔离，Linux／Windows后端、细粒度目标网络授权、受约束多行patch、直接HTTPS及Native共享辅助会话仍待做。这些不能从本轮有限工具测试推导为已经实现。
