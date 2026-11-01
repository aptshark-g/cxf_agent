#!/bin/sh
# Finder 双击入口：进入项目目录后交给同一个 cxf.sh，避免两套启动逻辑。
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$ROOT" || exit 1
exec "$ROOT/cxf.sh"
