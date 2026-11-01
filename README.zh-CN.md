# cxf agent

[English](README.md) · [测试成果](docs/benchmarks/README.zh-CN.md) · [设计契约](docs/design/README.md)

一个用 C 实现的轻量 Agent 运行时，探索与 **llama.cpp** 更紧密的协调：共享规范 token 前缀、显式传递上下文变化，在工具输出过程中提前预填充。

**当前是研究原型。** 工具执行闭环与可选原生后端已跑通；持续输出场景有局部收益，但尚未证明通用任务稳定提速、整体内存下降或可靠自主编程。调度实验保持显式可选。

## 已经实现什么

| 组件 | 当前范围 |
| --- | --- |
| 工具闭环 | 文件列表、读取、搜索、差异及可选写入；部署者固定测试命令；有界收据与多轮执行 |
| 原生上下文 | 一个 libllama context 管理 KV；共享规范 token 块、revision、追加／替换、前缀分支、回退与预算预填充 |
| 聊天桥接 | 已验收的 Qwen3 模板／工具解析、后缀增量分词与模板准备缓存 |
| 工具输出预填充 | `test.run` 输出时可选有界推测；生成前完整 token 校验；KV 常驻 |
| 实验适配 | 显式 KV 保存／恢复、语义块流式输入、耗时估算及分阶段诊断 |

调用上游 llama.cpp 接口，没有修改推理 kernel。共享 token 存储不等于原文、模板和全部中间表示只占一份，也不代表物理 KV 缩小。普通工具流程不需要快照／逐出；它们属于显式资源策略实验。详细接线程度见[当前适配清单](docs/adaptation-status.md)。

## 先运行基本工具流程

需要 POSIX 环境、C11 编译器、Make 与 Python 3.10+。已验收主机是 macOS / Apple M5；原生 Windows 及 Windows/CUDA 尚未验收。

```sh
make test
make build/agentctl
```

测试使用本地确定性端点，不需要模型、云账户或 API key。生成的 C 注册表已随源码提供，正常构建不需要运行生成器。

已有本地 OpenAI 兼容 HTTP 服务时：

```sh
cp agentctl.example.json agentctl.json
# 在 agentctl.json 中填写自己的端点与模型；示例 key 为空。
./cxf.sh --root /path/to/workspace
```

示例 URL 只是通用回环地址。内置 HTTP 传输目前只接受 `http://`；HTTPS 服务需要另行提供传输／代理。本地 `agentctl.json` 优先于环境变量、每次任务前重载，并排除在发布范围之外。

需要修改文件和运行测试时显式启用：

```sh
./cxf.sh --root /path/to/workspace --write /path/to/workspace \
  --test-command 'make test' --rounds 16
```

`test.run` 用当前进程权限和环境执行启动者选定的命令，**不是 OS 沙箱**；文件工具另有路径约束。REPL 每条输入目前创建独立任务，请求和工具缓冲区有上限，适合小文件、短任务。

## 使用原生 llama.cpp

已测组合是 **llama.cpp b10938**，commit `f1e44dcc11d8802d107bd7331a3d3fd3e6f57b93`，配 **Qwen3-0.6B-Q8_0**，运行在 Metal。小模型用于兼容性与调度验证；自主修复失败单独保留。其他模型／模板需要重新验收。

自行准备匹配 SDK 头文件、构建好的运行时库（包括 `llama-common`）及 GGUF；脚本不会自动下载：

```sh
mkdir -p build
python3 tools/build_agent_native.py \
  --sdk /path/to/llama.cpp-b10938 \
  --runtime /path/to/llama-runtime \
  --model /path/to/Qwen3-0.6B-Q8_0.gguf \
  --out build/native-local --test

./build/native-local/agentctl-native --repl --root /path/to/workspace \
  --llama-model /path/to/Qwen3-0.6B-Q8_0.gguf \
  --llama-backend /path/to/llama-runtime
```

每次验收使用新的 `--out`，避免覆盖旧结果。依赖默认位置改为仓库内通用的 `build/llama.cpp-b10938` 与 `local-model/`，显式参数可覆盖。默认 Native batch 容量仍为 **512**，工具输出预填充默认关闭，`--tool-output-prefill on` 才启用。报告中的诊断策略／容量不是推荐默认值。

## 实测到了什么

以下是对应源码版本的历史实测，不表示本轮重新跑过全部性能。单机配置：Apple M5 / 16 GiB、Metal、Qwen3 0.6B、固定 llama.cpp；任务对照每种负载 7 对评价样本。

| 问题 | 观测结果 | 边界 |
| --- | --- | --- |
| 持续工具输出能隐藏预填充吗？ | E49 中原 growth 策略对关闭，完整任务配对中位节省 **104 / 67 ms**，两组均 7/7 更快 | 三次真实工具调用，模型选择由脚本固定；依赖输出节奏 |
| 增大发布量子更好吗？ | E49 的 1536 字节试验对原 512 字节策略，持续任务反慢 **55 / 3 ms** | 失败优化保留，没有升为默认 |
| 尾部回退解决了吗？ | E47 仅线程控制最差任务反慢 **1.87 s**，工具期间没有 GPU 工作 | 工具／系统波动仍在，没有尾延迟保证 |
| 内存下降了吗？ | E09 临时前端堆 **512 → 141 KiB**；共享前缀场景 48,000 逻辑 token 只存 **6,000 个 ID** | 属于临时堆和 token payload，不是 RSS 或物理 KV 收益 |
| batch 640 更划算吗？ | E46 计算预留对 1024 容量从 **597.50 → 373.44 MiB** | 默认 512 为 298.75 MiB；物理 KV 均 **896 MiB**，耗时可能回退 |

![同负载控制与负结果](docs/benchmarks/figures/contention640-controls.svg)

目前归档 **50 份证据、35,520 条主记录、37 张图**。主记录包括预热、元数据、正确性与预测样本，不能当成同等数量的独立性能样本。功能验收、模型失败、性能、历史读数与未验证项分开记录，不跨阶段累加收益。

[中文完整报告](docs/benchmarks/README.zh-CN.md) · [English results](docs/benchmarks/README.md) · [测试方法](docs/benchmarks/METHODOLOGY.md) · [验收矩阵](docs/benchmarks/data/validation.csv)

## 代码位置与下一步

`include/cxf/`、`src/` 是 C 运行时与可选 libllama 适配。`tests/` 当前也承载 CLI、工具实现与 Native 聊天桥，尚未抽成独立应用层。`tools/` 包含构建、验收和报告脚本；`docs/evidence/` 保存不可变的实测证据。

下一步重点是长上下文的真实内存核算、减少保留表示、稳定的完整任务收益，以及目标 Windows 机器上的较大模型验证。通用多会话调度、云端 SSE 接线、更多模板和 Windows/CUDA 仍未完成；设计范围和已实现切片见[设计入口](docs/design/README.md)。

## 校验与发布准备

```sh
python3 tools/build_benchmark_report.py --check
python3 tools/prepare_github_release.py --check
```

源码干净导出见[发布指南](docs/RELEASING.md)，离线成果包见[成果发布说明](docs/benchmarks/PUBLISHING.md)。模型、运行时二进制、凭据、本地请求日志、KV 快照、会话历史与同目录独立项目不发布。准备脚本不执行暂存、提交或推送。

采用 MIT 许可证，见 [LICENSE](LICENSE)。
