CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -MMD -MP -Iinclude
LDFLAGS ?=
LDLIBS  ?= -lm

SRC := src/base.c src/alloc.c src/arena.c src/table.c src/ring.c \
       src/service.c src/world.c src/type.c src/soa.c src/str.c src/sb.c \
       src/bitmap.c src/buf.c src/blob.c src/namemap.c src/algo.c src/csr.c \
       src/pqueue.c src/vec.c src/cost.c src/roofline.c src/cost_residual.c src/cost_kalman.c
OBJ := $(SRC:.c=.o)

# 生成物进版本库，make 不自动跑生成器 —— 生成器需要 clang 与 python，
# 构建本身不应该依赖它们。改了结构体或服务要自己跑 make gen。
GENSRC := tests/soa_types.gen.c tests/services.gen.c \
          tests/agent_types.gen.c tests/agent_caps.gen.c tests/agent_fs.gen.c
GENOBJ := $(GENSRC:.c=.o)

# 两类产物必须分开链接。服务表里的条目是 extern 引用 CXF_SERVICE 定义的符号，
# 把它们链进不需要服务的测试，会因缺符号直接链接失败 —— 而不是延迟到运行期。
TYPE_GENOBJ := tests/soa_types.gen.o
SVC_OBJ     := tests/services.o tests/services.gen.o

# agent 第一刀：类型 + 能力服务 + 模块实现
AGENT_OBJ := tests/agent_types.gen.o tests/agent_caps.o tests/agent_caps.gen.o \
             tests/agent_fs.o tests/agent_fs.gen.o \
             tests/agent_net.o \
             tests/agent.o

# 协议层：传输 + 协议（test_llm 与 test_agent 共用；test_agent 里有一节把
# 它接进能力层跑完整 tick）
LLM_OBJ := tests/agent_http.o tests/agent_llm.o

all: build/test_ds build/test_world build/test_soa build/test_registry \
     build/test_service build/test_agent build/test_stress build/test_llm build/bench_ds

build/test_cost: src/cost.o tests/test_cost.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ src/cost.o tests/test_cost.c $(LDFLAGS) $(LDLIBS)

build/test_cost_residual: src/cost_residual.o tests/test_cost_residual.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ src/cost_residual.o tests/test_cost_residual.c $(LDFLAGS) $(LDLIBS)

build/test_cost_kalman: src/cost_kalman.o tests/test_cost_kalman.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ src/cost_kalman.o tests/test_cost_kalman.c $(LDFLAGS) $(LDLIBS)

build/test_roofline: src/cost.o src/roofline.o tests/test_roofline.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ src/cost.o src/roofline.o tests/test_roofline.c $(LDFLAGS) $(LDLIBS)

build/test_stress: $(OBJ) tests/test_stress.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(OBJ) tests/test_stress.c $(LDFLAGS) $(LDLIBS)

build/test_agent: $(OBJ) $(AGENT_OBJ) $(LLM_OBJ) tests/test_agent.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(AGENT_OBJ) $(LLM_OBJ) tests/test_agent.c $(LDFLAGS) $(LDLIBS)

# 协议层：传输 + 协议，与内核无关（只用到 base 的结果码）
build/test_llm: $(OBJ) $(LLM_OBJ) tests/test_llm.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LLM_OBJ) tests/test_llm.c $(LDFLAGS) $(LDLIBS)

build/test_service: $(OBJ) $(SVC_OBJ) tests/test_service.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(SVC_OBJ) tests/test_service.c $(LDFLAGS) $(LDLIBS)

build/test_registry: $(OBJ) $(TYPE_GENOBJ) tests/test_registry.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(TYPE_GENOBJ) tests/test_registry.c $(LDFLAGS) $(LDLIBS)

build/test_soa: $(OBJ) $(TYPE_GENOBJ) tests/test_soa.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(TYPE_GENOBJ) tests/test_soa.c $(LDFLAGS) $(LDLIBS)

build/test_ds: $(OBJ) tests/test_ds.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(OBJ) tests/test_ds.c $(LDFLAGS) $(LDLIBS)

build/test_world: $(OBJ) tests/test_world.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(OBJ) tests/test_world.c $(LDFLAGS) $(LDLIBS)

build/bench_ds: $(OBJ) tests/bench_ds.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(OBJ) tests/bench_ds.c $(LDFLAGS) $(LDLIBS)

# 上下文预算对比台（docs/agent.md §4.15）：产出的是一张曲线，不是"过/不过"，
# 所以与 bench_ds 同类，不进 make test。只依赖 libc，不链内核。
build/bench_budget: tests/bench_budget.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/bench_budget.c $(LDFLAGS) $(LDLIBS)

# 第一个**可执行**的 agent 回路（docs/agent.md §4.16.3）。
#
# 真模型运行不进 make test：非确定、要花钱、要网络。REPL 回归使用本地假端点，
# 覆盖真实 HTTP 与进程交互，不需要 key；它与 test_agent 的剧本档互补。
build/agentctl: $(OBJ) $(AGENT_OBJ) $(LLM_OBJ) tests/agent_basic.o tests/agentctl.c tests/agentctl_config.c tests/agentctl_config.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(AGENT_OBJ) $(LLM_OBJ) tests/agent_basic.o tests/agentctl.c tests/agentctl_config.c $(LDFLAGS) $(LDLIBS)

build/test_agent_basic: tests/agent_basic.o tests/test_agent_basic.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/agent_basic.o tests/test_agent_basic.c $(LDFLAGS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# 头文件改动必须触发重编，否则改了结构体布局而 .o 没跟上会静默错位。
-include $(OBJ:.o=.d) tests/agent_basic.d

test: build/test_ds build/test_world build/test_soa build/test_registry \
      build/test_service build/test_agent build/test_stress build/test_llm build/agentctl build/test_agent_basic build/test_cost build/test_roofline build/test_cost_residual build/test_cost_kalman
	./build/test_roofline
	./build/test_cost_residual
	./build/test_cost_kalman
	./build/test_cost
	./build/test_ds
	./build/test_world
	./build/test_soa
	./build/test_registry
	./build/test_service
	./build/test_agent
	./build/test_stress
	./build/test_llm
	./build/test_agent_basic
	python3 tests/test_agentctl_repl.py

test-repl: build/agentctl
	python3 tests/test_agentctl_repl.py

# Optional, real libllama/model acceptance. Default tests need neither dependency.
# Additional paths/options: python3 tools/test_llama_context.py --help
test-llama:
	python3 tools/test_llama_context.py

# Optional native chat/tool provider. Pass explicit SDK/runtime/model paths via
# the Python entry point when they differ from this machine's local install.
test-native-chat:
	python3 tools/build_agent_native.py --test

# Optional owner-thread KV restore acceptance; add --bench-kv-worker via Python
# for measurements separate from sanitizers and default regression.
test-kv-worker:
	python3 tools/build_agent_native.py --test-kv-worker

test-stream:
	python3 tools/build_agent_native.py --test-stream

bench: build/bench_ds
	./build/bench_ds

budget: build/bench_budget
	./build/bench_budget

# 真跑一次（需要 CXF_LLM_URL / CXF_LLM_MODEL / CXF_LLM_KEY）。用法：
#   make live ROOT=$PWD Q="在仓库里找 pqueue 的 decrease-key 在哪实现"
# 不给 Q 就从 stdin 读一行。
live: build/agentctl
	@if [ -z "$(ROOT)" ]; then echo "用法: make live ROOT=目录 Q=问题"; exit 2; fi
	@if [ -n "$(Q)" ]; then printf '%s\n' "$(Q)" | ./build/agentctl --root "$(ROOT)" $(ARGS); \
	else ./build/agentctl --root "$(ROOT)" $(ARGS); fi

gen:
	python3 tools/gen_registry.py -I include tests/soa_types.h
	python3 tools/gen_registry.py tests/services.c
	python3 tools/gen_registry.py -I include tests/agent_types.h
	python3 tools/gen_registry.py tests/agent_caps.c
	python3 tools/gen_registry.py tests/agent_fs.c

# 延迟 / 吞吐曲线（docs/agent.md §4.17）：K 档并发 × R 轮闭环保压。
# 与 budget 同族 —— 产出曲线，不产出过/不过，所以不进 make test。
#   make probe K=1,2,4,8 R=2
probe:
	@if [ -z "$$CXF_LLM_URL" ] || [ -z "$$CXF_LLM_MODEL" ]; then \
		echo "用法: CXF_LLM_URL=… CXF_LLM_MODEL=… [CXF_LLM_KEY=…] make probe K=1,2,4,8 R=2"; exit 2; fi
	python3 tools/probe_latency.py $(if $(K),K=$(K)) $(if $(R),R=$(R)) $(if $(Q),Q=$(Q))

clean:
	rm -rf build $(OBJ) $(GENOBJ) tests/services.o tests/agent.o tests/agent_caps.o \
	       tests/agent_fs.o tests/agent_net.o tests/agent_http.o tests/agent_llm.o tests/agent_basic.o

.PHONY: all test test-repl test-llama test-native-chat test-kv-worker test-stream bench budget live gen probe clean
