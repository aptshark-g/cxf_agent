# 只读工具自预测 v1（D-SPEC）

状态：2026-11-02 实施前冻结，本轮已实现实验宿主、正确性测试和异机测量入口；验收见 E58。默认 agentctl 不启用。不把 0.6B 兼容性验收作为预测质量或加速证据。

## 目标与基线

参考 [SPORK 论文](https://arxiv.org/abs/2607.03333) 的同模型前缀分叉探针：主模型开始生成后，从实际已计算前缀 fork，强制结束思考并开始一个工具调用；探针预测成功后提前读取，主模型继续原来的生成。只有正式调用确认后才交付工具结果。

首版只有 `fs.read`；不注册执行命令、写入、网络、目录扫描或云端能力。相同模型、模板、任务、greedy、生成上限、线程及 KV 容量下比较 off/on。off 不 fork、不创建工具线程；两组均完成主生成、正式工具读取／采纳、结果回流后的有界最终生成。模型加载单列；完整任务时间从准备输入到最终生成及清理结束。达到生成上限、解析失败或结果不一致均保留，不按成功样本筛选成绩。

## 所有权与调度

一个 libllama 引擎、一个调用 owner、最多一个探针和一个本地读取线程。所有分词、fork、prefill、greedy、close 串行由 owner 调用；工具线程不碰 engine。探针暂时占用 owner，主解码暂停，这项额外成本完整计入；读取可与恢复后的主解码重叠。不是两条 GPU 推理流并发。

复用 D-CTX 的 token/KV 共享：fork 不复制已有 canonical 历史，分支新增后缀独立；探针结束立即关闭，再恢复主解码。context-wide logits 切换仍经 cxf_llama_greedy 的末 token replay。不修改 llama.cpp 内核、不 save/evict、不另开模型或 engine。记录共享 token、分支逻辑 token 和 canonical 容量，不能写成物理 KV／RSS 节省。

Qwen3 thinking 为实验入口的显式准备选项，由同版本上游模板生成；默认 Native 仍关闭 thinking。首次分叉点为已生成 N 个主 token（默认 1），探针附加 `\n</think>\n<tool_call>\n`，最多生成给定 token 数，完整单工具封套才解析；正式主输出必须正常 EOG，不能从部分主输出执行工具。模板、工具 schema、分叉点、probe/main/final 上限写入运行元数据；其他模型模板未验收时失败关闭。

## 状态、采纳与失败

上游 Qwen3 开启 thinking 的 generation header 只有 assistant 起始标记，不由模板预写 `<think>`；关闭时才预写空 thinking。模式进入解析缓存标识，切换时重新准备。初版误假设 true header 带 `<think>` 的 DEP 失败保留于 E58，完整上游分词 oracle 覆盖 false/true/false/true 切换。

`idle → predicted → reading → joined → adopted/discarded → closed`；每轮最多一个候选。调用键是工具名和已解析 `args` 的完整字节，禁止模糊相似、路径归一化或只比工具名。主输出含多个调用、未知工具、DONE 或畸形输出不能采用候选。

配置／权限对象在读取线程排空前保持存活且不可变。候选绑定任务 epoch；调用不匹配、epoch 不同、取消、读取失败、权限拒绝、文件变化均丢弃。只有正式单个 fs.read 有效时才走正常读取回退；不执行被预测的其他工具。主生成失败先排空／清理候选后返回失败，不把候选当正式意图。线程启动或探针失败只影响投机路径，主任务仍继续；主引擎错误不得伪装成功。

读取复用现有敏感文件、根目录、链接和内容策略。只暂存最多 BASIC_OUT_HINT 的回执，不把候选正文写入主上下文或诊断。读取用同一 fd 取得前后版本（dev/ino/size/mode/uid/gid/nlink/mtime/ctime，含 ns）；采纳时经原 policy 重新打开路径并比较版本。变化或拒绝则丢弃后按正式调用重新读取。不是文件系统事务／强快照；验证后仍可能发生外部写入，和正常读取一样有并发边界。仅用于本地普通文件；pread 不可抢占，取消／关闭必须 join，网络文件系统阻塞不在时限保证内。

取消不终止已发生的只读 I/O，排空后清除暂存正文；重复 finish 不重复执行。进程权限、敏感文件或云外发规则不得因命中而绕过。无置信门、自动择时、partial-token accept、多候选、跨模型预测或 Windows 原生支持承诺。

## 验收及异机交接

无模型测试覆盖精确命中、参数失配、未知工具、文件修改／替换／权限变化、敏感文件／内容／链接、epoch、取消、重复消费、off、失败回退和线程关闭，含 ASan/UBSan。

真实 KV 验收使用 0.6B 验证分叉共享、主 greedy 与无探针一致、子关闭后的父继续、模板 thinking 切换及最终 session 清理；确定性分支注入只验收流程，不进入模型命中率。

异机模型测量保留所有 off/on 记录与轮换顺序。分别报告候选解析率、工具名命中、完整参数命中、实际采纳率；分母分别为全部 on 任务、正式单调用、有效预测，不能只报告成功子集。阶段为准备、主生成、探针、实际工具、正式调用后等待、采纳校验、结果回流／最终生成、join/close、整任务；模型任务完成率及 off/on 主输出一致性单列。不跨模型／平台累加收益。短 fs.read 可能小于探针推理成本，这是必须保留的负结果。

Windows 16GB 显存先走 WSL2 Linux + CUDA + 固定 b10938；原生 Windows 的文件／构建平台层另做。交接步骤、任务格式、原始数据导出和待测清单维护在 `docs/self-speculation-handoff.md`，不包含本机路径、模型、日志或凭据。

## 本轮实现与验收

`tests/agent_speculate.h/.cpp` 是模型无关候选控制器；`basic_read_versioned`/`basic_read_validate` 给现有 fs.read 增加可选 fd 版本证据，普通 read 不增加后置 stat。`agent_native_prepare_thinking` 为实验宿主提供显式模式，默认 Native 行为不变。`tests/self_speculate_native.cpp` 执行真实同模型探针；`tools/run_self_speculate.py` 负责新目录、off/on 轮换、全部失败／负对、显式分母和输出一致性检查。

24项状态／权限／新鲜度测试通过；8项 Native 验收包含真实模型的 KV／模板核对及两项明确注入 token 的完整采纳／回退流程，不把后两项计作模型命中。原14K Native回归通过；没有新正式性能样本。目标机器的模型任务质量与端到端收益仍待实测，见[E58](../evidence/self-speculation-20261102.json)。
