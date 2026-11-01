#!/usr/bin/env python3
"""agentctl --repl 的本地端到端回归；不访问云端，也不需要 API key。"""

import json
import os
from pathlib import Path
import queue
import subprocess
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / "build" / "agentctl"


class Fixture:
    def __init__(self, answers):
        self.answers = list(answers)
        self.bodies = []
        self.headers = []
        fixture = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                n = int(self.headers.get("Content-Length", "0"))
                body = self.rfile.read(n)
                fixture.bodies.append(json.loads(body))
                fixture.headers.append(dict(self.headers))
                answer = fixture.answers[len(fixture.bodies) - 1]
                message = ({"role": "assistant", **answer} if isinstance(answer, dict)
                           else {"role": "assistant", "content": answer})
                payload = json.dumps({"choices": [{"message": message}]}).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(payload)

            def log_message(self, _fmt, *_args):
                pass

        self.server = HTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    @property
    def url(self):
        return f"http://127.0.0.1:{self.server.server_port}/v1/chat/completions"

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)


def env_for(url, model="model-a"):
    env = os.environ.copy()
    env["CXF_LLM_URL"] = url
    env["CXF_LLM_MODEL"] = model
    env.pop("CXF_LLM_KEY", None)
    env.pop("CXF_LLM_SYSTEM", None)
    return env


def run(args, stdin, env):
    # 普通用例测环境变量路径；空 JSON 防止开发机的 agentctl.json 污染测试。
    with tempfile.TemporaryDirectory(prefix="cxf-empty-config-") as td:
        config = Path(td) / "empty.json"
        config.write_text("{}\n")
        proc = subprocess.run(
            [str(BIN), "--config", str(config), *args],
            input=stdin, text=True, capture_output=True,
            cwd=ROOT, env=env, timeout=20, check=False,
        )
    if proc.returncode != 0:
        raise AssertionError(
            f"agentctl rc={proc.returncode}\nSTDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}"
        )
    return proc


def require(cond, message, proc=None):
    if not cond:
        detail = "" if proc is None else f"\nSTDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}"
        raise AssertionError(message + detail)


def test_repl():
    fixture = Fixture(["answer-one", "answer-two"])
    try:
        with tempfile.TemporaryDirectory(prefix="cxf-repl-") as td:
            trace = Path(td) / "session.trace"
            script = f"""/help
/status
/model
/budget
/rounds
/conc
/rpm
/tools
/budget nope
/rounds -1
/quit extra
/conc 2
/rpm 0
/rpm 60
/trace {trace}
first task
/model model-b
/budget 4096
/rounds 5
/conc 3
/rpm 120
second task
/trace off
/trace
/status
/stats
/does-not-exist
/quit
"""
            proc = run(["--repl", "--root", str(ROOT)], script, env_for(fixture.url))

            out = proc.stdout
            err = proc.stderr
            require(out.count("answer-one") == 1, "第一项答案缺失或被累计结果重放", proc)
            require(out.count("answer-two") == 1, "第二项答案缺失或被重复打印", proc)
            require("model = model-b" in out and "budget = 4096 B" in out,
                    "模型或预算没有按命令更新", proc)
            require("rounds = 5" in out and "quota = conc 3, rpm 120" in out,
                    "轮数或配额没有按命令更新", proc)
            require("tasks    2" in out and "trace    off" in out,
                    "状态中的任务数或 trace 状态不对", proc)
            require("未知命令 /does-not-exist" in err and "/budget 需要一个" in err,
                    "非法命令没有响亮报错", proc)
            require("/quit 不接受参数" in err and "只设 conc" in err,
                    "多余参数或无效 conc 声明缺少诊断", proc)
            require("[stats] 以下为当前进程的累计值" in err and "llm calls 2" in err,
                    "累计统计没有覆盖两次调用", proc)
            require(len(fixture.bodies) == 2, "本地端点没有恰好收到两个请求", proc)
            require(fixture.bodies[0]["model"] == "model-a" and
                    fixture.bodies[1]["model"] == "model-b",
                    "/model 没有作用于下一任务的实际请求字节", proc)
            second_raw = json.dumps(fixture.bodies[1], ensure_ascii=False)
            require("answer-one" not in second_raw and "first task" not in second_raw,
                    "独立任务语义被破坏：第二请求携带了上一任务内容", proc)
            require(trace.exists(), "trace 文件没有生成", proc)
            trace_text = trace.read_text()
            require(trace_text.count("=== summary ===") == 2 and
                    "answer-one" in trace_text and "answer-two" in trace_text,
                    "trace 没有覆盖两项任务")
    finally:
        fixture.close()


def test_eof_without_task():
    # EOF 是正常退出，不应尝试连接端点。
    proc = run(["--repl", "--root", str(ROOT)], "/status\n/context\n",
               env_for("http://127.0.0.1:1/v1/chat/completions"))
    require("tasks    0" in proc.stdout, "EOF 前状态查询失败", proc)
    require("[context] 尚无任务\n[context] end" in proc.stdout,
            "无任务时的 /context 边界或提示不对", proc)


def test_one_shot_unchanged():
    fixture = Fixture(["single-answer"])
    try:
        proc = run(["--root", str(ROOT)], "single task\n", env_for(fixture.url))
        require(proc.stdout.count("single-answer") == 1, "一次性模式答案不对", proc)
        require("[stats]" in proc.stderr and "[time]" in proc.stderr,
                "一次性模式原有统计或耗时输出丢失", proc)
        require(len(fixture.bodies) == 1, "一次性模式请求数不对", proc)
    finally:
        fixture.close()


def read_until(lines, needle, timeout=10):
    text = ""
    while True:
        try:
            line = lines.get(timeout=timeout)
        except queue.Empty as exc:
            raise AssertionError(f"等待输出 {needle!r} 超时，已读到：\n{text}") from exc
        text += line
        if needle in text:
            return text


def line_queue(stream):
    lines = queue.Queue()

    def pump():
        for line in stream:
            lines.put(line)

    threading.Thread(target=pump, daemon=True).start()
    return lines


def test_json_hot_reload():
    fixture = Fixture(["answer-one", "answer-two", "answer-three"])
    proc = None
    try:
        with tempfile.TemporaryDirectory(prefix="cxf-hot-config-") as td:
            config = Path(td) / "agentctl.json"

            def write(model, key):
                config.write_text(json.dumps({
                    "api": {"url": fixture.url, "model": model, "key": key,
                            "timeout_ms": 45000},
                    "agent": {"budget": 4096, "rounds": 5, "conc": 2, "rpm": 120},
                }))

            write("model-a", "secret-a")
            proc = subprocess.Popen(
                [str(BIN), "--repl", "--root", str(ROOT), "--config", str(config)],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, cwd=ROOT, env=env_for(fixture.url), bufsize=1,
            )
            out_lines = line_queue(proc.stdout)
            err_lines = line_queue(proc.stderr)

            def send(line):
                proc.stdin.write(line + "\n")
                proc.stdin.flush()

            send("first task")
            read_until(out_lines, "answer-one")
            write("model-b", "secret-b")
            send("second task")  # 不发 /reload：下一次任务前必须自动重读。
            read_until(out_lines, "answer-two")
            require(len(fixture.bodies) == 2, "热加载阶段请求数不对")
            require([b["model"] for b in fixture.bodies] == ["model-a", "model-b"],
                    "修改 JSON 后模型没有进入下一请求")
            require(fixture.headers[0].get("Authorization") == "secret-a" and
                    fixture.headers[1].get("Authorization") == "secret-b",
                    "修改 JSON 后密钥没有进入下一请求")

            config.write_text('{"api":')
            send("must-not-send")
            read_until(err_lines, "本行未提交")
            require(len(fixture.bodies) == 2, "坏 JSON 仍然提交了任务")

            write("model-c", "secret-c")
            send("/reload")
            reload_out = read_until(out_lines, "[config] 已加载")
            require("secret-c" not in reload_out, "/reload 泄露了密钥")
            send("third task")
            read_until(out_lines, "answer-three")
            send("/model model-d")
            read_until(out_lines, "model = model-d")
            send("/context")
            context = read_until(out_lines, "[context] end")
            require("[context] task #3 | model model-c" in context and
                    "third task" in context and "DONE answer-three" in context,
                    "/context 没有展示最近任务的真实输入、动作和配置快照")
            require("model-d" not in context and "first task" not in context and
                    "answer-one" not in context,
                    "/context 混入当前配置或更早任务")
            require(not any(secret in context for secret in
                            ("secret-a", "secret-b", "secret-c")),
                    "/context 泄露了密钥")
            send("/status")
            status = read_until(out_lines, "tasks    3")
            require("model    model-d" in status and "budget   4096 B" in status and
                    "timeout  45000 ms" in status and "api key  已设置" in status,
                    "状态没有显示当前配置（应与旧任务快照分开）")
            require("secret-c" not in status, "/status 泄露了密钥")
            send("/quit")
            out, err = proc.communicate(timeout=10)
            require(proc.returncode == 0, "热加载会话退出失败")
            require("secret-a" not in out + err and "secret-b" not in out + err and
                    "secret-c" not in out + err, "终端输出泄露了密钥")
            require([b["model"] for b in fixture.bodies] ==
                    ["model-a", "model-b", "model-c"], "最终模型序列不对")
    finally:
        if proc is not None and proc.poll() is None:
            proc.kill()
            proc.communicate()
        fixture.close()


def test_context_tool_roundtrip():
    call = {
        "content": None,
        "tool_calls": [{
            "id": "call-context", "type": "function",
            "function": {"name": "fs.grep",
                         "arguments": json.dumps({"args": "CONTEXT_TOKEN"})},
        }],
    }
    fixture = Fixture([call, "tool-answer"])
    try:
        with tempfile.TemporaryDirectory(prefix="cxf-context-tool-") as td:
            Path(td, "alpha.txt").write_text("actual CONTEXT_TOKEN result\n")
            proc = run(["--repl", "--root", td],
                       "find the token\n/context\n/quit\n", env_for(fixture.url))
            out = proc.stdout
            require(out.count("tool-answer") == 2,
                    "最终答案和 /context 中的 DONE 应各出现一次", proc)
            require("[context] events 2" in out and
                    "CALL fs.grep CONTEXT_TOKEN" in out and
                    "#call fs.grep 13" in out and
                    "#result fs.grep" in out and "actual CONTEXT_TOKEN result" in out,
                    "/context 没有保留两轮 prompt、CALL 和真实工具结果", proc)
            require("dropped events 0" in out and "dropped prompt 0 B" in out and
                    "dropped act 0 B" in out, "短工具任务不应丢失复盘数据", proc)
            require(len(fixture.bodies) == 2, "工具任务应当恰好调用模型两次", proc)
            second = fixture.bodies[1]
            require(any(m.get("role") == "tool" and
                        "actual CONTEXT_TOKEN result" in m.get("content", "")
                        for m in second["messages"]),
                    "第二请求没有携带真实工具结果", proc)
    finally:
        fixture.close()


def tool_call(name, args=""):
    return {"content": None, "tool_calls": [{
        "id": "call-basic", "type": "function",
        "function": {"name": name, "arguments": json.dumps({"args": args})},
    }]}


def test_basic_coding_loop():
    """Real C compiler and disk, scripted model: fail, fix, retest, read back."""
    # Use a literal printf format; only the second argument changes.
    bad = '#include <stdio.h>\nint main(void) { printf("%d\\n", 41); return 0; }\n'
    good = bad.replace(', 41)', ', 42)')
    fixture = Fixture([
        tool_call("fs.list", "."), tool_call("fs.read", "answer.c"),
        tool_call("fs.write_apply", "answer.c\n" + bad), tool_call("test.run"),
        tool_call("fs.write_apply", "answer.c\n" + good), tool_call("test.run"),
        tool_call("fs.read", "answer.c"), "BASIC_LOOP_DONE",
    ])
    try:
        with tempfile.TemporaryDirectory(prefix="cxf-basic-loop-") as td:
            path = Path(td, "answer.c")
            path.write_text(bad.replace(', 41)', ', 40)'))
            command = 'cc answer.c -o answer && actual=$(./answer) && printf "actual=%s\\n" "$actual" && test "$actual" = 42'
            proc = run(["--root", td, "--write", td, "--test-command", command,
                        "--rounds", "16"], "fix answer.c to print 42 and test it\n", env_for(fixture.url))
            require("BASIC_LOOP_DONE" in proc.stdout, "闭环没有交付最终结果", proc)
            require(path.read_text() == good, "修改未真实落盘", proc)
            require(len(fixture.bodies) == 8, "调用重跑或闭环未完成", proc)
            names = {t["function"]["name"] for t in fixture.bodies[0]["tools"]}
            require({"fs.read", "fs.list", "test.run", "fs.write_apply"} <= names,
                    "基本工具未注册到模型清单", proc)
            require(not names & {"travel.train", "price.quote", "market.secondhand", "plan.merge"},
                    "演示能力泄漏到实际部署", proc)
            def results(index):
                return "\n".join(m.get("content", "") for m in fixture.bodies[index]["messages"]
                                 if m.get("role") == "tool")
            require("f answer.c" in results(1), "目录结果没有回到模型", proc)
            require(', 40)' in results(2), "文件原文没有回到模型", proc)
            require("exit=1" in results(4) and "actual=41" in results(4),
                    "测试失败或输出没有回到模型", proc)
            require("exit=0" in results(6) and "actual=42" in results(6),
                    "修正后的测试没有通过", proc)
            require(', 42)' in results(7), "最终文件没有回读验证", proc)
    finally:
        fixture.close()


def test_basic_tool_deployment():
    fixture = Fixture(["done"])
    try:
        with tempfile.TemporaryDirectory(prefix="cxf-basic-deploy-") as td:
            proc = run(["--root", td], "hello\n", env_for(fixture.url))
            names = {t["function"]["name"] for t in fixture.bodies[0]["tools"]}
            require(names == {"fs.read", "fs.list", "fs.grep"},
                    "只读部署出现写入或执行能力", proc)
            # All compatible switches must fit the fixed system prompt budget.
            proc = run(["--repl", "--root", td, "--write", td, "--append", td,
                        "--net", "127.0.0.1:1", "--test-command", "true"],
                       "/tools\n/quit\n", env_for(fixture.url))
            require("test.run" in proc.stdout, "完整部署未能启动", proc)
    finally:
        fixture.close()


def main():
    test_repl()
    test_eof_without_task()
    test_one_shot_unchanged()
    test_json_hot_reload()
    test_context_tool_roundtrip()
    test_basic_coding_loop()
    test_basic_tool_deployment()
    print("test_agentctl_repl: all tests passed")


if __name__ == "__main__":
    main()
