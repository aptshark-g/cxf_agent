#!/usr/bin/env python3
"""Local-model baseline: trust disk, compiler and trace, never the final claim.

Requires an already running OpenAI-compatible local endpoint; no dependencies.
Keeps each trial under build/ for inspection. Does not edit agentctl.json.
"""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", default="http://127.0.0.1:18080/v1/chat/completions")
    parser.add_argument("--model", default="qwen3-0.6b")
    parser.add_argument("--timeout", type=int, default=90)
    opt = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    case = Path(tempfile.mkdtemp(prefix="basic-agent-live-", dir=root / "build"))
    config = case / "config.json"
    config.write_text(json.dumps({"api": {"url": opt.url, "model": opt.model,
                                         "key": "", "timeout_ms": 15000}}))
    source = case / "answer.c"
    source.write_text('#include <stdio.h>\nint main(void) { printf("%d\\n", 40); return 0; }\n')
    trace = case / "trace.txt"
    command = ('cc answer.c -o answer && actual=$(./answer) && '
               'printf "actual=%s\\n" "$actual" && test "$actual" = 42')
    args = [str(root / "build/agentctl"), "--config", str(config), "--root", str(case),
            "--write", str(case), "--test-command", command, "--rounds", "12", "--trace", str(trace)]
    question = ("请修复 answer.c，让程序输出 42。先读取文件，再写入完整的新源码，"
                "最后调用 test.run 验证。一次只调用一个工具，依据真实结果决定下一步。\n")
    print(f"trial: {case}", flush=True)
    started = time.monotonic()
    try:
        proc = subprocess.run(args, input=question, text=True, capture_output=True, timeout=opt.timeout)
        (case / "stdout.txt").write_text(proc.stdout)
        (case / "stderr.txt").write_text(proc.stderr)
        status = proc.returncode
    except subprocess.TimeoutExpired as exc:
        for name, value in [("stdout.txt", exc.stdout), ("stderr.txt", exc.stderr)]:
            (case / name).write_text(value.decode(errors="replace") if isinstance(value, bytes) else value or "")
        status = "timeout"
    elapsed = time.monotonic() - started
    # Independent verification uses the deployment's test command, outside the
    # agent loop. It does not count as the model having invoked test.run.
    actual = subprocess.run(["/bin/sh", "-c", command], cwd=case, text=True,
                            capture_output=True, timeout=15)
    (case / "independent-test.txt").write_text(actual.stdout + actual.stderr)
    text = trace.read_text() if trace.exists() else ""
    calls = [line for line in text.splitlines() if line.startswith("CALL ")]
    tested = any("CALL test.run" in line for line in calls)
    passed = (status == 0 and actual.returncode == 0 and tested and
              any("CALL fs.read " in line for line in calls) and
              any("CALL fs.write_apply " in line for line in calls) and
              "#test exit=0 signal=0 timeout=0 io_error=0" in text)
    report = {"passed": passed, "model": opt.model, "agent_status": status,
              "elapsed_seconds": round(elapsed, 3), "calls": calls,
              "independent_test_exit": actual.returncode, "model_ran_test": tested}
    (case / "report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n")
    print(json.dumps(report, indent=2, ensure_ascii=False), flush=True)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
