#!/bin/sh
# 一键构建并进入 cxf 会话。所有默认值都可用同名环境变量覆盖。
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
    cat <<'EOF'
用法：./cxf.sh [agentctl 部署参数]

默认进入 REPL，并自动执行 make build/agentctl。

环境变量：
  CXF_LLM_URL    默认 http://127.0.0.1:8000/v1/chat/completions
  CXF_LLM_MODEL  默认 local-model
  CXF_LLM_KEY    可选，由本地端点决定是否需要
  test.run 默认 macOS 文件隔离与禁网；无后端时拒绝执行。
  --test-read DIR / --test-write DIR   显式扩展命令目录权限
  --test-net deny|allow                独立网络授权
  --test-toolchain BIN_DIR --test-sdk SDK_DIR  显式绑定工具链与 SDK，不增加目录权限
  --test-isolation off                 显式取消 OS 隔离
  CXF_ROOT       默认 cxf.sh 所在目录；agent 可读取的根目录
  CXF_MODEL_SCOPE local 或 cloud；HTTP 默认未知，云外发默认拒绝
  CXF_CLOUD_EGRESS deny / task / workspace；启动者显式授权
  CXF_NET_SCOPE  http.post 独立声明 local 或 cloud
  CXF_CONFIG     默认 cxf.sh 所在目录的 agentctl.json

JSON 中的字段优先于环境变量，并在每次任务前自动重载。

示例：
  ./cxf.sh
  ./cxf.sh --model-scope local --config /path/to/agentctl.json
  CXF_LLM_MODEL=qwen3 ./cxf.sh --model-scope local
  ./cxf.sh --write /path/to/workspace --trace /tmp/cxf.trace
  ./cxf.sh --model-scope local --root /path/to/workspace --write /path/to/workspace --test-write /path/to/workspace --test-command 'DEVELOPER_DIR=/Library/Developer/CommandLineTools make test' --rounds 16
EOF
    exit 0
fi

: "${CXF_LLM_URL:=http://127.0.0.1:8000/v1/chat/completions}"
: "${CXF_LLM_MODEL:=local-model}"
: "${CXF_ROOT:=$ROOT}"
: "${CXF_CONFIG:=$ROOT/agentctl.json}"
export CXF_LLM_URL CXF_LLM_MODEL

printf 'cxf: config   %s\n' "$CXF_CONFIG"
printf 'cxf: root     %s\n' "$CXF_ROOT"
make -C "$ROOT" build/agentctl
exec "$ROOT/build/agentctl" --repl --root "$CXF_ROOT" --config "$CXF_CONFIG" "$@"
