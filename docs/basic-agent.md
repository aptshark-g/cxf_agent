# 基本 Agent 工具闭环

> 执行链验收与真实模型失败探测分列于 [成果汇总](benchmarks/README.zh-CN.md#正确性能力与未验证项)（E04、E12）；失败记录保留，不合并为实现通过率。

2026-10-30：先做可执行的基本 Agent，再适配本地推理引擎的上下文与 KV 生命周期。
本阶段不引入记忆系统、规划框架或新的调度层。

## 工具与部署

| 工具 | 挂载条件 | 契约 |
|---|---|---|
| `fs.list` | `--root DIR` | 列直接子项；参数 `.` 或相对目录，名称排序；有界输出明确报截断 |
| `fs.read` | `--root DIR` | 读文本；参数为相对路径，可加换行和十进制字节偏移；每页最多 4096 B，收据 `next=-1` 表示读完，否则按 next 继续 |
| `fs.grep` | `--root DIR` | 既有子串检索 |
| `fs.diff` | `--diff DIR` 或 `--write DIR` | 既有只读差异预览 |
| `fs.write_new` | `--create DIR` | 既有新建，不覆盖 |
| `fs.write_apply` | `--write DIR` | 既有整文件替换／创建 |
| `fs.append` | `--append DIR` | 既有追加 |
| `test.run` | `--test-command COMMAND` | 模型参数必须为空；命令由启动者固定，工作目录为 `--root` |
| `http.post` | `--net HOST:PORT` | 既有向固定服务发送 |

`fs.read` / `fs.list` 用目录描述符逐段打开路径，拒绝 `..`、绝对路径和符号链接组件。
`fs.list` 最多展示 128 项，路径名含控制字节的项会省略并计数；大目录明确报告未展示的项。
`fs.read` 面向文本，不读二进制；UTF-8 页边界不切断字符。工具不创建额外持久会话或全文副本。

`test.run` 是显式启用的进程执行能力，按 WRITE 信任级挂载，**不是 OS 沙箱**。
启动者应把编译／测试所需的完整命令作为部署事实提供。它可能执行工作区里的代码，
继承当前进程的环境和系统权限，写根约束不限制这个子进程。
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
./cxf.sh --write /path/to/project --root /path/to/project \
  --test-command 'make test' --test-timeout-ms 30000 --rounds 16
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
