#!/usr/bin/env python3
"""Real-model CLI acceptance. Writes receipts only inside --out and a temp workspace."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", type=Path, required=True)
    ap.add_argument("--model", type=Path, required=True)
    ap.add_argument("--runtime", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--probe-tools", action="store_true", help="record model task capability separately from infrastructure acceptance")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith("CXF_")}
    env["CXF_LLM_SYSTEM"] = "Do not call tools. Respond with the single word OK to every user message."
    results = []
    with tempfile.TemporaryDirectory(prefix="native-cli-") as folder:
        root = Path(folder)
        (root / "agentctl.json").write_text("INVALID JSON: native mode must not read HTTP configuration")
        base = [str(args.binary.resolve()), "--root", folder]
        native = ["--llama-model", str(args.model.resolve()), "--llama-backend", str(args.runtime.resolve())]

        def run(name, options, input_text, expected):
            p = subprocess.run(base + options, cwd=root, input=input_text, env=env,
                               capture_output=True, text=True, timeout=180)
            (args.out / (name + ".stdout.txt")).write_text(p.stdout)
            (args.out / (name + ".stderr.txt")).write_text(p.stderr)
            assert p.returncode == expected, (name, p.returncode, p.stderr[-2000:])
            assert "AddressSanitizer" not in p.stderr and "runtime error:" not in p.stderr
            results.append({"case": name, "exit": p.returncode, "passed": True})
            return p

        p = run("repl", native + ["--repl", "--llama-max-tokens", "64"],
                "/status\nSay OK.\n/context\n/model replacement\n/reload\nSay OK again.\n/quit\n", 0)
        assert "backend  libllama" in p.stdout and "endpoint http://" not in p.stdout
        assert "[task] #2" in p.stderr and "generations 2" in p.stderr
        assert "[llm] 最后一次调用没成" not in p.stderr
        assert "请重启以更换" in p.stderr
        p = run("generation-limit", native + ["--llama-max-tokens", "1"], "Say OK.\n", 1)
        assert "generations 1" in p.stderr and "原生推理失败" in p.stderr
        assert not p.stdout.strip(), p.stdout
        run("missing-model", ["--llama-backend", str(args.runtime)], "", 2)
        run("explicit-http-config", native + ["--config", str(root / "agentctl.json")], "", 2)
        run("bad-threads", native + ["--llama-threads", "0"], "Say OK.\n", 2)
        if args.probe_tools:
            (root / "note.txt").write_text("ANSWER=COBALT_7319\n")
            env["CXF_LLM_SYSTEM"] = "Use tools to read requested files. Do not guess file contents. After a tool result, answer concisely."
            trace = (args.out / "real-tool-probe.trace.txt").resolve()
            p = subprocess.run(base + native + ["--llama-max-tokens", "256", "--rounds", "4", "--trace", str(trace)],
                               cwd=root, input="Read note.txt using fs.read and report the exact value after ANSWER=.\n",
                               env=env, capture_output=True, text=True, timeout=180)
            (args.out / "real-tool-probe.stdout.txt").write_text(p.stdout)
            (args.out / "real-tool-probe.stderr.txt").write_text(p.stderr)
            transcript = trace.read_text() if trace.exists() else ""
            passed = p.returncode == 0 and "COBALT_7319" in p.stdout and "CALL fs.read " in transcript and "#file" in transcript
            results.append({"case": "real-tool-probe", "exit": p.returncode, "passed": passed,
                            "scope": "single file read and receipt-based answer; not autonomous code repair acceptance"})
            print("real-model single-file tool probe: " + ("passed" if passed else "not passed"))
            (root / "a.c").write_text("int main(void) { return 1; }\n")
            env["CXF_LLM_SYSTEM"] = ("You are a coding assistant. Read files before editing. Run tests before and after fixing. "
                                     "Use fs.write_apply with args path newline complete content. Do not claim success before test.run exits zero.")
            trace = (args.out / "real-repair-probe.trace.txt").resolve()
            p = subprocess.run(base + native + ["--llama-max-tokens", "256", "--rounds", "8", "--write", folder,
                               "--test-command", "cc a.c -o check && ./check", "--trace", str(trace)], cwd=root,
                               input="Repair a.c so the configured test exits successfully. First read a.c, run test.run, edit it to return 0, run test.run again, read the updated file, then finish.\n",
                               env=env, capture_output=True, text=True, timeout=180)
            (args.out / "real-repair-probe.stdout.txt").write_text(p.stdout)
            (args.out / "real-repair-probe.stderr.txt").write_text(p.stderr)
            (args.out / "real-repair-probe.a.c").write_text((root / "a.c").read_text())
            transcript = trace.read_text() if trace.exists() else ""
            passed = (p.returncode == 0 and "CALL fs.write_apply " in transcript and
                      transcript.count("CALL fs.read ") >= 2 and transcript.count("CALL test.run ") >= 2 and
                      "#test exit=1" in transcript and "#test exit=0" in transcript and
                      "return 0" in (root / "a.c").read_text())
            results.append({"case": "real-repair-probe", "exit": p.returncode, "passed": passed,
                            "scope": "one instructed trivial C repair; not a general coding benchmark"})
            print("real-model instructed C repair probe: " + ("passed" if passed else "not passed"))
    (args.out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    print("PASS native CLI: real-model REPL, isolated task sessions, ignored HTTP config, fixed model controls, generation limit, invalid options")


if __name__ == "__main__":
    main()
