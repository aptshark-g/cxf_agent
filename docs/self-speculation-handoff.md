# 只读自预测：异机交接与测量

本轮先做基础设施；预测质量和端到端收益留到目标机器实测。唯一行为契约是 [D-SPEC](design/self-speculation-v1.md)，此处只维护操作步骤。参考 [SPORK](https://arxiv.org/abs/2607.03333)，尚未实现其 confidence gate 或 partial-token accept，不能把本实现称作完整 SPORK 复现。

## 当前入口与 0.6B 的用途

`tests/agent_speculate.cpp` 管理单个只读候选；`tests/self_speculate_native.cpp` 是复用 Native 模板和 cxf KV fork 的实验宿主。默认 agentctl 没有自预测开关。宿主只支持一个 fs.read → 结果回流 → 最终回答；不支持多轮、多工具 fanout 或任意命令。

0.6B 能验证模板、共享前缀、主分支 greedy 不受探针影响、采纳／回退和资源清理；不能预先断言它一定能或不能预测。它可能输出畸形参数、没有工具调用或用尽 thinking 预算，必须把这些记作失败。模型更大也不保证获益：短本地读取通常很快，而探针需要额外解码。先测完整调用命中率和 probe 成本，再决定是否扩大工具范围。

## Windows 机器的准备方式

先用 WSL2 的 Linux 用户态和 CUDA。以下是待目标机器验收的操作配方，macOS 通过不等于 Windows/CUDA 已通过。无需运行 llama-server 服务；只借它的 `--version` 核对动态库版本。原生 Windows 的 POSIX 文件权限与构建适配另做；当前整个 agent 的 test.run 沙箱也不能据此宣布支持 Linux。

所需：C/C++17 编译器、Python3、Git、CMake、CUDA 工具链；与 SDK 同一 C++ ABI 编译 libllama-common。模型自行准备受支持的 Qwen3 dense GGUF，先从能留出 KV／计算缓冲余量的配置开始。16GB 显存只是容量条件，不能仅凭显存指定可靠模型上限。

```sh
# 在项目目录运行；路径是示例，自行替换。固定同一上游提交。
git clone https://github.com/ggml-org/llama.cpp.git build/llama.cpp-b10938
git -C build/llama.cpp-b10938 checkout f1e44dcc11d8802d107bd7331a3d3fd3e6f57b93
cmake -S build/llama.cpp-b10938 -B build/llama-cuda \
  -DCMAKE_BUILD_TYPE=Release -DGGML_CUDA=ON -DBUILD_SHARED_LIBS=ON \
  -DLLAMA_BUILD_COMMON=ON -DLLAMA_BUILD_TOOLS=ON
cmake --build build/llama-cuda --config Release -j 4

# 无模型状态与安全验收；先做，不需要 CUDA。
PYTHONDONTWRITEBYTECODE=1 python3 tools/test_self_speculate.py \
  --sanitize --out build/spec-controller-san-target

# SDK 头文件和 runtime 必须都对应上述提交；构建，不启动正式测量。
PYTHONDONTWRITEBYTECODE=1 python3 tools/build_agent_native.py \
  --build-self-speculate --sdk build/llama.cpp-b10938 \
  --runtime build/llama-cuda/bin --model /path/to/Qwen3.gguf \
  --out build/spec-native-target

# 短真实 KV／模板验收，8 项（含2项注入流程）；不报告模型命中率或提速。
PYTHONDONTWRITEBYTECODE=1 python3 tools/run_self_speculate.py \
  --binary build/spec-native-target/self-speculate-native \
  --model /path/to/Qwen3.gguf --runtime build/llama-cuda/bin \
  --check --out build/spec-kv-check-target

# 正式 off/on 对照：默认创建三份无秘密文本和三个任务，不加 sleep。
PYTHONDONTWRITEBYTECODE=1 python3 tools/run_self_speculate.py \
  --binary build/spec-native-target/self-speculate-native \
  --model /path/to/Qwen3.gguf --runtime build/llama-cuda/bin \
  --repeats 7 --warmup 1 --threads 4 --gpu-layers 99 \
  --context-tokens 8192 --main-tokens 1024 --probe-tokens 128 \
  --final-tokens 256 --fork-after 1 --out build/spec-model-target-01
```

输出目录必须不存在。模型、动态库、任务和硬件换了就用新目录，不覆盖旧试验。正式计时使用无 sanitizer 构建；正确性另用 `--sanitize --build-self-speculate` 构建和 `--check` 验收。源码或二进制变化不能沿用旧测量身份。

## 自定义新任务

用 `--root /path/to/workspace --tasks /path/to/tasks.json` 同时替换默认夹具。文件应事先存在并在整个对照期保持不变；不放私有数据。任务文件格式如下（`expected_args` 只用于事后标记，不参与 probe／main 提示或采纳）：

```json
[
  {"id":"settings_read","prompt":"Read settings.txt with fs.read, then report its first line.","expected_args":"settings.txt"},
  {"id":"notes_read","prompt":"Read notes.txt with fs.read, then summarize its contents.","expected_args":"notes.txt"}
]
```

先固定一组开发任务／预算；随后换没参与调参的文件和任务做验证。扩大 main token 预算或更换模型也算新试验，保留原来的预算耗尽失败。不要人为加工具 sleep 来制造收益。将来若增加较慢的只读检索工具，先补权限、输入版本和结果新鲜度契约。

## 如何读结果

`rows.jsonl` 保留元数据、所有预热和评价任务；`pairs.csv` 按同 case/block 配对，奇偶 block 交换 off/on 顺序；`summary.json` 保留全部负对和失败／不一致对。`metadata.json` 保留模型、二进制、编译时／当前源码哈希与平台，`run-config.json` 和 `engine.log` 只在本地。加载耗时单列；任务 total 包含准备、probe、主生成、工具、回流、最终生成及显式清理，不含模型加载。每对的主生成、工具回执、最终生成哈希应一致。

率值都以 `[计数, 分母]` 展示：全部 on 任务的有效预测率、正式单调用的工具名／完整参数命中率、有效预测的完整命中率、全部 on 任务的实际采纳率，以及 off/on 完成率。正式工具不合法、main 没结束、probe 截断、读失败和 final 没结束不得删除。`task_ok` 只代表闭环结束，不评估回答语义正确性；`expected_call` 单列。

阶段包括 prepare、probe、main span／扣除 probe 的 main decode、预测读取、正式调用后等待、join、版本校验、正常读取、回流准备、最终生成和 cleanup。`issued_lead_ms` 是预测派发到正式调用的提前量；`read_finished_before_call` 表示读取是否完成。各中位数不可相加重建总耗时。`all_task_paired_saving_ms` 包含失败任务，只作完整账本；性能结论应同时展示完整且输出一致的配对数量、完成率及负对，不从失败缩短的时间宣称加速。样本少时不称 p95／p99。

共享 KV/token 指标是逻辑计数与 canonical payload 容量，未测物理 KV／RSS。fork 后新增后缀、logits replay、候选暂存和线程都有成本。所有 engine 调用仍串行；没有两个 GPU 推理任务并发。

## 带回和登记

带回源码版本／构建 manifest、GPU 型号／显存／驱动、CUDA／编译器版本、原始 rows、summary、pairs 和失败说明。当前 runner 不自动采集完整 GPU／驱动信息，需要目标机器另保存 `nvidia-smi` 输出；检查并移除其中的私人进程或路径。不要带模型、运行时、真实请求日志、配置／key、KV 快照或未经检查的自定义任务正文上 GitHub。

目标机器结果先保留本地，再脱敏为新的实验编号和 evidence，登记报告生成器并更新图表／发布清单。当前功能验收不承诺性能；现有 E01–E57 的历史速度数据不能升级成自预测结果。

待完成：WSL2/CUDA 真机验收；至少一组更大模型的新任务命中率与 off/on 完整任务测量；实际 agentctl 接线；置信门／动态择时；更慢只读工具的安全与新鲜度策略。没有这些数据前保持实验默认关闭。
