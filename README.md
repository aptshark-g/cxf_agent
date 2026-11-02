# cxf agent

[简体中文](README.zh-CN.md) · [Results](docs/benchmarks/README.md) · [Design contracts](docs/design/README.md)

A lightweight C agent runtime exploring closer coordination with **llama.cpp**: share canonical token prefixes, submit explicit context changes, and overlap local prefill with tool output.

**Research prototype.** The tool execution loop and optional native backend work. Streaming prefill helps some sustained-output workloads; general end-to-end speedups, lower whole-process memory and autonomous coding quality have not been demonstrated. Experimental scheduling remains opt-in.

For read-only self-prediction tests on another Windows machine, follow the [target-machine handoff](docs/self-speculation-handoff.md): use WSL2/CUDA, run functional acceptance first, then off/on comparisons. Native Windows and the target GPU remain unvalidated.

## What is implemented

| Component | Current scope |
| --- | --- |
| Agent tool loop | File listing, reading, search, diff, versioned multiline edits, guarded single-line edits and optional writes; a deployment-fixed test command; bounded receipts and multi-round execution |
| Native context | One libllama context owns KV; shared canonical token blocks, explicit revisions, append/replace, prefix forks, rollback and budgeted prefill |
| Chat bridge | Tested Qwen3 template/tool parsing, incremental suffix tokenization and cached template preparation |
| Tool-output prefill | Optional bounded speculative prefill during `test.run`; final full token validation before generation; resident KV |
| Read-only self-prediction experiment | Same-model KV-fork probe, early fs.read, exact-call/version-checked adoption or fallback; separate host, not integrated into default CLI |
| Local text helper | Optional `llm.local`: deployment-bound loopback endpoint, separate credentials, two requests per task, data-only replies |
| Research adapters | Explicit KV save/restore, semantic-unit streaming, cost estimation and timing diagnostics |

The adapters call upstream llama.cpp APIs; inference kernels are unchanged. Shared token storage does **not** mean every text representation disappears or physical KV allocation shrinks. Snapshot/eviction is an explicit experiment, not required for normal tool use. See the [adaptation inventory](docs/adaptation-status.md) for integration levels and pending work.

## Cloud request observations (E62/E63)

Read-only prompts now omit write instructions, and the trusted test relay sends compact UTF-8 JSON.20 actual tasks /50 API rounds pass using requested model `deepseek-v4-flash-0731` (DeepSeek Flash). Each round uses18 fewer prompt tokens; bytes fall, but4/8 evaluated task pairs regress and no stable speedup/cache improvement is established. Per-round usage/cache/bytes/API waits, real tool time and forwarding costs are [reported separately](docs/benchmarks/README.md#e62e63--cloud-utf-8-requests-usage-and-separated-waiting). Serialization wall time slightly increases; all negatives remain. [Contract and reproduction](docs/design/hybrid-v1.md#e62-cloud-reproduction).

## Versioned multiline editing (E59–E61)

Read with `fs.read`, then pass its version to WRITE-only `fs.edit` for one unique exact multiline replacement. Conflicts require re-reading; pre-commit failures preserve the original. CLI args stay1KiB and files64KiB. See the [contract](docs/design/versioned-edit-v1.md).34 security/failure cases,4 CLI suites, full regression and Native compatibility pass.

Three actual user-selected cloud DeepSeek Flash tasks pass exact-source checks: normal editing, conflict recovery preserving an external update, and read-only refusal. At64KiB, successful edits add about5.6–5.8ms over legacy handlers, consistent with full text-policy scan work. All negative pairs remain; cloud latency and historical inference gains are separate. See the [results](docs/benchmarks/README.md).

## Start with the tool loop

Requirements: a POSIX environment, a C11 compiler, Make and Python 3.10+; model-free self-prediction tests also use C++17. The current OS-isolated test runner additionally requires the accepted macOS host-protection backend; other backends are pending. The validated host is macOS / Apple M5; native Windows and Windows/CUDA acceptance remain pending.

```sh
make test
make build/agentctl
```

The tests use local deterministic endpoints and need no model, cloud account or API key. Generated C registries are included; normal builds do not run the generator.

For an already running local OpenAI-compatible HTTP service:

```sh
cp agentctl.example.json agentctl.json
# Edit agentctl.json with your own endpoint and model; key is empty by default.
./cxf.sh --model-scope local --root /path/to/workspace
```

The example URL is generic loopback. The current built-in HTTP transport accepts `http://` only; HTTPS services require a separate transport/proxy. Local `agentctl.json` overrides environment settings and reloads before each task; it is excluded from publication.

Enable writes and a fixed test command explicitly:

```sh
./cxf.sh --model-scope local --root /path/to/workspace --write /path/to/workspace \
  --test-write /path/to/workspace --test-command 'DEVELOPER_DIR=/Library/Developer/CommandLineTools make test' --rounds 16
```

`test.run` executes the launcher's fixed command. The CLI defaults to a minimal child environment; `--test-env inherit` explicitly restores environment inheritance. On macOS, OS isolation defaults to workspace reads, private temporary writes and no network; other platforms fail closed until a backend is available. `--test-write DIR` is independent from file-tool `--write`. Fixed commands that need external data or networking require explicit grants. REPL inputs currently create independent tasks; finite request/tool buffers suit small files and short tasks.

## Use the optional local text helper

```sh
./cxf.sh --model-scope local --root /path/to/workspace --local-config /path/to/agentctl.local.json
```

The separate file accepts only `api.url`, `api.model`, optional `api.key` and `api.timeout_ms`; the endpoint must be exactly IPv4 loopback. The primary model can call `llm.local` with a bounded text subtask. Its reply returns through the existing tool loop, with no tool permissions, automatic routing or shared Native KV. A cloud primary requires explicit workspace egress permission before helper/tool text can return to it. See the [contract and configuration template](docs/design/hybrid-v1.md).

E50 validates this plumbing with eight scripted two-provider HTTP suites under sanitizers, alongside the real read/edit/test loop and Native compatibility regressions. Real cloud/model quality and performance remain unmeasured.

## File and cloud data policy

All file tools now reject `.env` variants, key files, common credential directories and local configuration names. The actual primary/helper config and active trace are protected even with arbitrary names. Symlink components and regular-file hardlinks are rejected; list/grep omit protected entries. Ordinary source files and `agentctl.example.json` remain readable.

HTTP scope is **unknown by default** and cloud egress defaults to **deny**. Even a loopback endpoint can relay to cloud: declare `--model-scope local` only for a trusted local service. For a trusted cloud relay, use `--model-scope cloud --cloud-egress task` for plain task text, or explicitly choose `workspace` to allow mounted tool context. `http.post` has an independent `--net-scope`. The equivalent startup environment settings are `CXF_MODEL_SCOPE`, `CXF_NET_SCOPE`, `CXF_CLOUD_EGRESS`; command-line options take precedence. The current HTTP transport only connects to loopback; direct HTTPS remains pending. Native inference is explicitly local.

Configured credentials, common token/private-key patterns and known base64/hex representations are blocked in requests, responses and tool results. Blocked results do not enter the next transcript, trace or context preview. E51 passes445 policy checks and12 HTTP security suites under ASan/UBSan, with the normal tool loop and Native/14K/Stream/output regressions. This request/result policy is not universal DLP. The separate E52 OS execution layer is described below. See the [policy contract](docs/design/data-policy-v1.md) and [functional evidence](docs/evidence/agent-data-policy-20261101.json). No new performance claim.

## Isolate test commands

`test.run` now defaults to macOS Seatbelt: workspace content read-only, a fresh private temporary directory writable, network denied for the command and descendants, and inherited file descriptors closed. Protected names and active configs/trace stay denied. Repeated `--test-read DIR` / `--test-write DIR` grants extend directories; `--test-net allow` independently permits networking. `--test-isolation off` explicitly removes OS restrictions. Unsupported backends or setup failures never fall back silently. Linux/Windows backends remain pending; this is not a VM or resource-exhaustion boundary.

E53 passes542 C checks and12 OS-security suites under sanitizers, plus the prior12 policy /8 foundation suites and Native14K/Stream/output acceptance. A localhost-filter candidate admitted a local non-loopback interface and was removed; only deny/allow ship. Run this repository's own nested sandbox/network tests directly on the host; deployment commands do not automatically receive those privileges.

E52 later exposed cxf startup environment through process inspection; E53 protects cxf itself and verifies omission before enabling commands. Other same-UID process environments and visible argv remain outside this guarantee. No system SIP setting is changed; unavailable protection refuses execution. The230 timing rows contain93 evaluation pairs. Whole CLI median paired overhead is **15.724ms** for an empty tool, **3.565ms** for explicit CLT compilation, and **2467.548ms** for standard Xcode cc (worst **2534.841ms**). Most of that large regression is inside the command; exact cache/IPC attribution is unfinished. CLT is an explicitly different workload, not an automatic production substitution. See [contract](docs/design/test-sandbox-v1.md), [all results](docs/benchmarks/README.md) and [E53 evidence](docs/evidence/test-sandbox-host-20261101.json). Historical inference gains are not added to these costs.

## Explicit binding to the same Xcode toolchain

A fresh private xcrun lookup cache makes system compiler shims invoke expensive Xcode discovery. Pin the intended compiler directory and SDK explicitly, while retaining the default sandbox:

```sh
./cxf.sh --model-scope local --root /path/to/workspace \
  --test-toolchain /Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin \
  --test-sdk /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk \
  --test-command 'cc -mmacosx-version-min=26.0 unit.c -o "$TMPDIR/unit" && "$TMPDIR/unit"'
```

Both paths are required. They set PATH/SDKROOT and grant no directories or network; there is no automatic CLT substitution or rewrite of absolute /usr/bin/cc or explicit xcrun calls. E55 new C/archive/C++ tasks save **2.37/3.45/2.38s** paired median versus unbound isolation, each7/7 faster. Bound isolation still adds **21/15/59ms** paired median; C++ has wide residual ranges and all negative pairs remain. Zero-config shims retain the old regression. This build-command optimization is separate from inference/KV work. [Full diagnosis/data](docs/benchmarks/README.md), [contract](docs/design/test-sandbox-v1.md).561 C checks,15 OS suites and prior policy/foundation/Native regressions pass.

E56 later found different implicit deployment targets in E55 bound-off/on (26.0/10.4). Matching compiler/SDK and output did not ensure equal compile semantics. The historical numbers and plots above remain unchanged; new controls use an explicit target.

## Optional per-task C++ precompiled header

For multiple files sharing a valid common prefix header, select the [private PCH recipe](tools/run_cpp_batch.sh) explicitly under the existing sandbox:

```sh
# Inside a fixed test.run command, with the intended toolchain/SDK and project target.
/bin/sh tools/run_cpp_batch.sh pch 26.0 common.hpp "$TMPDIR/program" main.cpp extra.cpp
# plain uses the same header and compile parameters for a matched baseline.
```

The recipe fixes Clang C++17/O2, builds a fresh PCH, retains compiler validation and cleans up on success/failure. No cross-task cache or new grant. E57 new3/5-TU tasks save **330/794ms** including cold setup (each7/7); single-file tasks regress **159/118ms**, so there is no automatic enablement. PCH uses14.56-15.51MB of transient logical file payload; RSS/KV improvements are unmeasured. The grouped-rule candidate fails performance acceptance; default rules/logging are unchanged. [Full stages/negative results](docs/benchmarks/README.md), [contract](docs/design/test-sandbox-v1.md).

## Use native llama.cpp

The tested combination is **llama.cpp b10938**, commit `f1e44dcc11d8802d107bd7331a3d3fd3e6f57b93`, and **Qwen3-0.6B-Q8_0**, on Metal. The small model tests compatibility and scheduling; it is not a coding-capability claim. Other templates/models need their own acceptance.

Supply matching SDK headers, built runtime libraries (including `llama-common`) and a local GGUF. Nothing is downloaded automatically:

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

Use a fresh `--out` for each run. Generic defaults live under `build/llama.cpp-b10938` and `local-model/`; explicit flags override them. Default native batch capacity remains **512**. Tool-output prefill is disabled unless selected with `--tool-output-prefill on`; diagnostic policies and capacities in the reports are not recommended defaults.

## What the measurements show

These are historical measured revisions, not a rerun of every current file. Single machine: Apple M5 / 16 GiB, Metal, Qwen3 0.6B, pinned llama.cpp. Matched task results below use seven evaluation pairs per workload.

| Question | Measured result | Limit |
| --- | --- | --- |
| Can sustained tool output hide prefill? | E49's existing growth policy saves **104 / 67 ms** in paired whole-task medians vs disabled; 7/7 faster in both fixtures | Three real tool calls with scripted model choices; workload-dependent overlap |
| Does a larger publication quantum help? | E49's 1536-byte trial loses **55 / 3 ms** vs the existing 512-byte growth policy on paced fixtures | Failed optimization retained; not promoted |
| Are tails resolved? | E47's thread-only control has a **1.87 s** worst task regression without tool-window GPU work | System/tool variation remains; no tail-latency guarantee |
| Is memory lower? | E09 temporary frontend heap **512 → 141 KiB**; shared-prefix fixture stores **6,000 IDs** for 48,000 logical tokens | Temporary heap/token payload; not RSS or physical KV savings |
| Is batch 640 preferable? | E46 compute reservation **597.50 → 373.44 MiB** vs capacity 1024 | Default 512 reserves 298.75 MiB; physical KV stays **896 MiB** and timing can regress |

![Matched tool controls, including negative results](docs/benchmarks/figures/contention640-controls.svg)

The archive contains **64 evidence files, 37,290 main raw records and 43 figures**. E62/E63 add50 cloud rounds and60 serialization blocks, with20 task/30 tool auxiliary records separate. E59/E61 retain tool diagnostics; E60 records3 earlier cloud tasks separately. Inference performance remains tied to historical revisions. The [target-machine handoff](docs/self-speculation-handoff.md) covers later platform validation. Raw counts include warmups, metadata, correctness and prediction records; they are not independent latency samples. Reports separate functional acceptance, model failures, performance, historical observations and unvalidated claims. Gains from different experiments must not be added together.

[English results](docs/benchmarks/README.md) · [中文成果汇总](docs/benchmarks/README.zh-CN.md) · [Methodology](docs/benchmarks/METHODOLOGY.md) · [Validation matrix](docs/benchmarks/data/validation.csv)

## Project map and next steps

`include/cxf/` and `src/` contain the C runtime and optional libllama adapter. `tests/` currently also hosts the CLI, tool implementations and native chat bridge; these have not been extracted into a separate application layer. `tools/` contains builders, acceptance runners and report generators. `docs/evidence/` holds immutable measured evidence.

Near-term priorities are long-context memory accounting, fewer retained representations, stable task-level gains and larger-model validation on the target Windows machine. General multi-session scheduling, automatic local/cloud routing, cloud SSE, broader model templates and Windows/CUDA support remain unfinished. Scope and implemented subsets are tracked in the [design registry](docs/design/README.md).

## Verify and prepare publication

```sh
python3 tools/build_benchmark_report.py --check
python3 tools/prepare_github_release.py --check
```

See the [source release guide](docs/RELEASING.md) for the clean source export and the [results package guide](docs/benchmarks/PUBLISHING.md) for the separate offline data bundle. Models, runtime binaries, credentials, local request traces, KV snapshots, session history and sibling projects are excluded. Preparation scripts do not stage, commit or push.

MIT License — see [LICENSE](LICENSE).
