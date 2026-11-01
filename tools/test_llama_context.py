#!/usr/bin/env python3
"""Optional real-model acceptance for the cxf libllama backend (macOS/Linux).

Does not download anything, start a server, call cloud APIs or alter agentctl.json.
Default make test remains independent of libllama. See docs/llama-context-v1.md.
"""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
LOCAL = ROOT / "local-model"


def capture(args):
    p = subprocess.run(args, text=True, capture_output=True)
    if p.returncode:
        raise RuntimeError(p.stdout + p.stderr)
    return p.stdout + p.stderr


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--sdk", type=Path, default=ROOT / "build/llama.cpp-b10938")
    ap.add_argument("--runtime", type=Path, default=LOCAL / "runtime/llama-b10938")
    ap.add_argument("--model", type=Path, default=LOCAL / "models/Qwen3-0.6B-Q8_0.gguf")
    ap.add_argument("--sanitize", action="store_true")
    ap.add_argument("--out", type=Path)
    args = ap.parse_args()
    if platform.system() not in ("Darwin", "Linux"):
        ap.error("native Windows platform/build acceptance remains pending")
    version = capture([str(args.runtime / "llama-server"), "--version"])
    commit = capture(["git", "-C", str(args.sdk), "rev-parse", "HEAD"]).strip()
    expected = "f1e44dcc11d8802d107bd7331a3d3fd3e6f57b93"
    if commit != expected or commit[:9] not in version:
        ap.error("v1 requires matching b10938 headers and runtime (f1e44dcc1)")
    out = args.out or Path(tempfile.mkdtemp(prefix="llama-native-", dir=ROOT / "build"))
    out = out.resolve(); out.mkdir(parents=True, exist_ok=True)
    if (out / "manifest.json").exists():
        ap.error("choose a fresh --out to preserve previous evidence")
    src = sorted((ROOT / "src").glob("*.c"))
    test_src = [ROOT / "tests" / name for name in [
        "agent_types.gen.c", "agent_caps.c", "agent_caps.gen.c", "agent_fs.c", "agent_fs.gen.c",
        "agent_net.c", "agent.c", "agent_http.c", "agent_llm.c", "test_llama_context.c"]]
    command = ["cc", "-std=c11", "-O1" if args.sanitize else "-O2", "-g", "-Wall", "-Wextra", "-Wpedantic",
               "-I" + str(ROOT / "include"), "-I" + str(ROOT / "tests"),
               "-I" + str(args.sdk / "include"), "-I" + str(args.sdk / "ggml/include")]
    if args.sanitize:
        command.append("-fsanitize=address,undefined")
    command += [str(p) for p in src + test_src]
    command += ["-L" + str(args.runtime), "-Wl,-rpath," + str(args.runtime),
                "-lllama", "-lggml", "-lggml-base", "-lm", "-o", str(out / "test_llama_context")]
    (out / "compile.txt").write_text(capture(command))
    manifest = {"llama_commit": commit, "runtime_version": version, "platform": platform.platform(),
                "model": str(args.model), "sanitizers": args.sanitize, "compile": command,
                "sources": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                            for p in src + test_src + [ROOT / "include/cxf/llama.h", ROOT / "src/llama_internal.h"]}}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"acceptance: {out}", flush=True)
    with (out / "stdout.txt").open("w") as stdout, (out / "engine.log").open("w") as stderr:
        result = subprocess.run([str(out / "test_llama_context"), str(args.model), str(args.runtime)],
                                stdout=stdout, stderr=stderr)
    print((out / "stdout.txt").read_text(), end="")
    if result.returncode:
        print((out / "engine.log").read_text()[-4000:])
        raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
