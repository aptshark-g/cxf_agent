# agentctl 会话式 TUI：第一刀

## 目标与边界

第一刀是**行式会话 REPL**：一个持续运行的会话窗口，普通文本提交一次任务，斜杠命令在两次任务之间查看或调整配置。它不是 ncurses 全屏界面，不引入第二套 agent 引擎，也不改变现有 `agentctl` 的一次性执行模式。

复用边界保持在现有 `agentctl` 内：装配仍由 `ctl_setup` 完成，任务仍走 `ctl_submit → cxf_world_tick → agent_final`。交互层只负责输入、命令分发、配置写入和输出，不复制 INJECT / MATCH / EXECUTE / RECYCLE 控制逻辑。

本刀不做：流式 token 渲染、运行中取消或审批、rewind、GUI、能力根目录和信任级别热切换。后两者需要重建 world / 重渲染工具清单，不能伪装成普通配置项；`/context` 只做任务间复盘，不借机提供运行中 attach。

## 生命周期与任务语义

一键构建并进入会话：

```sh
./cxf.sh
```

脚本默认连接 `http://127.0.0.1:8000/v1/chat/completions`，模型名为 `local-model`，工作根目录为脚本所在目录。

**三项都能改，但优先级不同，这一点文档上一版说错了**（写着"三项都可覆盖，无需修改脚本"，实测不成立）：

| 变量 | 生效条件 |
| --- | --- |
| `CXF_ROOT` | 只在命令行参数里出现，环境变量直接生效 |
| `CXF_LLM_URL` / `CXF_LLM_MODEL` | **只在 JSON 没声明该字段时**生效。若从示例复制的本地 `agentctl.json` 声明了这两个字段，改环境变量**不会**换端点；仓库仅发布 `agentctl.example.json` |

实测（2026-10-28）：`CXF_LLM_URL=http://… ./cxf.sh` 之后 `/status` 仍显示 `http://127.0.0.1:8000/…`。
原因在 `ctl_config_init`：先 `agent_llm_cfg_from_env`，再用 JSON 的 `present` 位图逐字段覆盖。

要临时换端点，两条路都对，选一条：

```sh
# ① 指向另一份 json（推荐：不碰仓库里的文件，key 也不会落进项目）
CXF_CONFIG=/tmp/my-endpoint.json ./cxf.sh
# ② 或把 url / model 字段从 agentctl.json 里删掉，交给环境变量
```

`CXF_LLM_KEY` 按需从环境继承；JSON 里的 `key` 为空串表示清除。额外参数原样交给 `agentctl`，例如 `./cxf.sh --write /path/to/workspace`；`./cxf.sh --help` 查看启动说明。

等价的手动方式：

```sh
./build/agentctl --repl --root DIR [--config agentctl.json] [其他部署参数]
```

## JSON 配置与热加载

一键脚本默认读取项目根目录的 `agentctl.json`；`agentctl.example.json` 是可分享的模板。本地文件已被 `.gitignore` 排除，写入真实密钥后建议执行 `chmod 600 agentctl.json`。

```json
{
  "api": {
    "url": "http://127.0.0.1:8000/v1/chat/completions",
    "model": "local-model",
    "key": "",
    "timeout_ms": 30000
  },
  "agent": {
    "budget": 0,
    "rounds": 8,
    "conc": 0,
    "rpm": 0
  }
}
```

- `api`：地址、模型、密钥和超时；密钥为空字符串表示清除。
- `agent`：上下文字节预算、轮数、并发声明和 RPM；0 的含义与对应斜杠命令一致。
- JSON 是持续配置源，字段优先于环境变量，也会覆盖会话中对应的临时修改。
- 每次普通任务提交前自动重读，无需重启；`/reload` 用于立即重读并查看结果。
- 解析或校验失败时保留上一份有效配置，并拒绝提交新任务。未知字段、重复字段、非法转义和数值溢出都会报错。
- 配置在任务边界生效；不会中断或改写正在执行的请求。目录根、写能力和信任级别不在 JSON 热加载范围内。

启动时装配一次 world，随后循环：

1. 打印 `cxf> ` 并读取一行；EOF 与 `/quit` 正常退出。
2. `/...` 进入命令分发，不提交给模型。
3. 非空普通文本调用同一个 run-one 辅助函数，任务结束后回到提示符。
4. 配置修改只发生在任务边界，对下一次提交生效。

当前引擎的 `final_h` 是累计终值日志，统计计数也是累计值。因此 run-one 在提交前记录终值偏移，结束后只显示本次新增部分；`/stats` 明确显示进程累计值。

**这还不是带持久对话记忆的聊天器。** 每条普通输入会创建一份新的 `AGENT_PLAN_LOOP` 上下文；上一任务的问题、工具记录和答案不会自动注入下一任务。会话首先提供的是持续进程与可调控制面，持久对话历史需要另立上下文数据结构和预算策略。

## 第一刀命令

空参数表示查询；带参数表示设置并回显新值。非法命令、缺参数形式以外的多余参数、非数字和数值溢出都响亮报错，不猜测。

| 命令 | 行为 |
| --- | --- |
| `/help` | 命令帮助 |
| `/status` | 端点、模型、预算、轮数、配额、trace、信任级别与已提交任务数 |
| `/stats` | 当前 world 的累计执行统计 |
| `/tools` | 打印实际发给模型的工具清单 JSON |
| `/context` | 复盘最近一次任务的实际 LLM 输入、规范动作与结果码 |
| `/reload` | 立即重读 JSON；失败时保留旧配置 |
| `/model [NAME]` | 查询或修改 `g_llm.model`；若 JSON 含 model，下一任务前会被文件覆盖 |
| `/budget [N]` | 查询或修改 `agent_args_budget_set`；0 恢复引擎默认上限 |
| `/rounds [N]` | 查询或修改 `agent_set_max_rounds`；0 恢复引擎默认轮数 |
| `/conc [N]` | 查询或修改并发声明；实际与 RPM 一起通过 `agent_quota_set` 写入 |
| `/rpm [N]` | 查询或修改 RPM；`conc=0 && rpm=0` 为不限 |
| `/trace [FILE\|off]` | 查询、切换或关闭 trace；任务边界切换，新文件覆盖写 |
| `/quit` | 关闭 trace、停止并释放 world，退出 |

只给 `/conc N` 且 `/rpm` 为 0 时必须警告：当前实现里它不会单独形成有效闸门。`/model` 只改模型名，不重渲染工具清单；`/trust` 本刀不提供。

## `/context`：最近任务的内存复盘

`/context` 无参数，在两次任务之间展示最近一次已结束任务。当前 REPL 阻塞执行任务，所以它不是运行中 attach；它也不是跨任务持久对话历史。尚无任务时明确提示，输出统一以 `[context] end` 结束，便于交互前端和测试确定边界。

每次 LLM 调用展示：

- 调用序号与 `agent_llm_chat_action` 的结果码；失败调用也保留，不只展示成功轮。
- 实际进入 LLM 能力的 prompt 原始字节，不从最终答案反推、不转述。
- 返回的规范动作：统一投影为 `DONE <args>` / `CALL <cap> <args>`；坏块报告偏移。
- 任务开始时的模型、端点、预算、轮数、工具清单长度快照，以及最终输出长度。热加载后的当前配置不能用来描述旧任务。

### 防过时契约

这里记录的是**真实 LLM 边界**，不是当前工具列表的格式化副本。唯一记录点在 `ctl_step_exec` 的 `agent_llm_chat_action(...)` 返回之后：`args/args_len` 是实际输入，`act/alen` 是实际规范动作。

1. 新增 `#call`、`#result` 以外的上下文记录类型时，prompt 仍按原始字节展示，**不得要求同步修改 `/context`**。
2. 新增工具时，展示只经共享 `agent_act_decode` / `agent_act_bytes` 遍历，不增加任何按工具名分支。
3. 底层动作编码变化时只更新共享解码器；禁止在 `/context` 养第二份动作解析逻辑。
4. 内存容量有界。事件表或字节池不足必须累计丢失事件数、prompt/action 丢失字节数并显示在摘要里，不能静默截断后伪装完整。
5. `/context` 独立于 `--trace` 和 `/trace`：trace 是落盘证据，内存事件日志是在线复盘数据源。
6. 不保存或展示 API key；任务快照不长期持有指向 store/blob 的指针。

容量是当前部署的复盘窗口，不是上下文协议的一部分。需要更长时间线时调整窗口或另做 trace 投影，不能为了展示改变引擎请求构造。

## 输出与错误

- REPL 默认在答案后打印一条本次任务摘要，不把累计计数冒充本次计数。
- `/stats` 才打印完整累计统计；段化直方图与耗时诊断仍保留在一次性模式。
- LLM 失败诊断沿用现有 `(last_status, last_rc)` 判据，成功不得打印成失败。
- 无法打开新 trace 时保留原 trace；命令错误不会终止会话。
- 单次任务没有新增终值时，REPL 回到提示符并可用 `/stats` 检查；进程退出码不因中途某个任务失败而钉死为 1。

## 后续扩展点

- 运行中取消、审批：需要 world tick 与终端输入的事件循环，不属于当前阻塞式 run-one。
- 持久对话：显式 session/transcript 数据结构、序列化与上下文预算，不能靠累计 `final_h` 或 `/context` 的最近任务窗口冒充。
- 第三个前端出现时再抽公共 session 库；现在不提前制造框架层。
