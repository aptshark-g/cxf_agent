#!/usr/bin/env python3
"""Build/run the pinned direct-libllama context experiment and retain raw evidence.

This is an isolated benchmark, not a replacement for agentctl's model provider.
Uses only Python's standard library and a C compiler; macOS/Linux runner.
"""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
LOCAL = ROOT / "local-model"


def command_output(args):
    p = subprocess.run(args, text=True, capture_output=True, check=True)
    return (p.stdout + p.stderr).strip()


def file_sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def aggregate(rows):
    groups = {}
    for row in rows:
        if "rep" not in row:
            continue
        key = (row["kind"], row.get("mode", ""), row.get("gap_ms", ""))
        groups.setdefault(key, []).append(row)
    result = []
    for (kind, mode, gap), samples in groups.items():
        summary = {"kind": kind, "mode": mode, "gap_ms": gap, "n": len(samples)}
        for field in samples[0]:
            values = [r[field] for r in samples]
            if field != "rep" and isinstance(values[0], (int, float)) and not isinstance(values[0], bool):
                summary[field + "_median"] = statistics.median(values)
                summary[field + "_min"] = min(values)
                summary[field + "_max"] = max(values)
        summary["all_top1_match"] = all(r.get("same_top1", False) for r in samples)
        result.append(summary)
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--sdk", type=Path, default=ROOT / "build/llama.cpp-b10938")
    ap.add_argument("--runtime", type=Path, default=LOCAL / "runtime/llama-b10938")
    ap.add_argument("--model", type=Path, default=LOCAL / "models/Qwen3-0.6B-Q8_0.gguf")
    ap.add_argument("--repeats", type=int, default=5)
    ap.add_argument("--prefix-lines", type=int, default=128)
    ap.add_argument("--stream-gap-ms", type=int, default=20)
    ap.add_argument("--stream-only", action="store_true")
    ap.add_argument("--out", type=Path)
    opt = ap.parse_args()
    if platform.system() not in ("Darwin", "Linux"):
        ap.error("this benchmark runner currently supports macOS/Linux; native Windows is not validated")
    for p in [opt.sdk / "include/llama.h", opt.runtime / "llama-server", opt.model]:
        if not p.exists():
            ap.error(f"missing dependency: {p}; provide matching --sdk, --runtime and --model")
    if not 1 <= opt.repeats <= 30 or not 8 <= opt.prefix_lines <= 512:
        ap.error("repeats must be 1..30, prefix-lines must be 8..512")
    if not 0 <= opt.stream_gap_ms <= 1000:
        ap.error("stream-gap-ms must be 0..1000")
    version = command_output([str(opt.runtime / "llama-server"), "--version"])
    commit = command_output(["git", "-C", str(opt.sdk), "rev-parse", "HEAD"])
    if commit[:9] not in version:
        ap.error(f"header/library commit mismatch: SDK {commit[:9]}, runtime {version}")
    out = opt.out or Path(tempfile.mkdtemp(prefix="llama-context-", dir=ROOT / "build"))
    out = out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    if (out / "manifest.json").exists():
        ap.error(f"output already contains a trial: {out}; use a fresh --out")
    source = ROOT / "tests/bench_llama_context.c"
    source_bytes = source.read_bytes()
    (out / "bench_source.c").write_bytes(source_bytes)
    binary = out / "bench_llama_context"
    compile_args = [os.environ.get("CC", "cc"), "-std=c11", "-O2", "-Wall", "-Wextra", "-Wpedantic",
                    "-I" + str(opt.sdk / "include"), "-I" + str(opt.sdk / "ggml/include"),
                    str(ROOT / "tests/bench_llama_context.c"), "-L" + str(opt.runtime),
                    "-Wl,-rpath," + str(opt.runtime), "-lllama", "-lggml", "-lggml-base", "-lm",
                    "-o", str(binary)]
    command_output(compile_args)
    run_args = [str(binary), str(opt.model), str(opt.runtime), str(opt.repeats), str(opt.prefix_lines),
                str(opt.stream_gap_ms), "1" if opt.stream_only else "0"]
    manifest = {"runtime_version": version, "source_commit": commit,
                "platform": platform.platform(), "machine": platform.machine(), "cpu_count": os.cpu_count(),
                "model": str(opt.model), "model_file_bytes": opt.model.stat().st_size,
                "compile": compile_args, "run": run_args,
                "benchmark_source_sha256": hashlib.sha256(source_bytes).hexdigest(),
                "model_sha256": file_sha256(opt.model),
                "measurement_scope": "direct C API; no HTTP, chat-template rendering or generation; shared warm prefix excluded",
                "stream_source": "simulated fixed token arrivals; tokenization done before arrival schedule",
                "memory_scope": "resident size on macOS, not GPU VRAM; sequence removal does not imply buffer deallocation"}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"results: {out}", flush=True)
    with (out / "raw.jsonl").open("w") as stdout, (out / "engine.log").open("w") as stderr:
        p = subprocess.run(run_args, stdout=stdout, stderr=stderr)
    if p.returncode:
        raise SystemExit(f"benchmark failed ({p.returncode}); inspect {out / 'engine.log'}; no success report generated")
    rows = [json.loads(line) for line in (out / "raw.jsonl").read_text().splitlines() if line]
    stats = aggregate(rows)
    (out / "summary.json").write_text(json.dumps(stats, indent=2) + "\n")
    fields = sorted({key for row in rows for key in row})
    with (out / "raw.csv").open("w", newline="") as f:
        writer = csv.DictWriter(f, fields)
        writer.writeheader()
        writer.writerows(rows)
    for row in stats:
        print(f"{row['kind']:8} {row['mode']:15} gap={str(row['gap_ms']):>2} "
              f"wall={row.get('wall_ms_median', 0):8.3f}ms "
              f"front={row.get('frontend_ms_median', 0):7.3f}ms "
              f"tail={row.get('post_input_ms_median', 0):7.3f}ms "
              f"correct={row['all_top1_match']}")
    print("Correctness and raw samples retained; inspect summary.json for ranges and state/fork timing.")


if __name__ == "__main__":
    main()
