#!/usr/bin/env python3
"""Build optional native agentctl and real-model chat acceptance, pinned b10938."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import statistics
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
LOCAL = ROOT / "local-model"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--sdk", type=Path, default=ROOT / "build/llama.cpp-b10938")
    ap.add_argument("--runtime", type=Path, default=LOCAL / "runtime/llama-b10938")
    ap.add_argument("--model", type=Path, default=LOCAL / "models/Qwen3-0.6B-Q8_0.gguf")
    ap.add_argument("--out", type=Path)
    ap.add_argument("--bench-tool-balance",action="store_true")
    ap.add_argument("--test-tool-balance",action="store_true")
    ap.add_argument("--bench-tool-taskcost",action="store_true")
    ap.add_argument("--test-tool-taskcost",action="store_true")
    ap.add_argument("--bench-tool-isolate",action="store_true")
    ap.add_argument("--test-tool-isolate",action="store_true")
    ap.add_argument("--bench-tool-launch",action="store_true")
    ap.add_argument("--test-tool-launch",action="store_true")
    ap.add_argument("--test-resident-flow",action="store_true")
    ap.add_argument("--bench-resident-flow",action="store_true")
    ap.add_argument("--test-resident-stream",action="store_true")
    ap.add_argument("--bench-resident-stream",action="store_true")
    ap.add_argument("--bench-tool-timing",action="store_true")
    ap.add_argument("--test-tool-prepare-cli", action="store_true", help="compile deterministic actions against the actual native CLI routing")
    ap.add_argument("--metal-probe",action="store_true",help="macOS process-local Metal command timing observer for test harness")
    ap.add_argument("--test-tool-output", action="store_true", help="resident real output correctness")
    ap.add_argument("--build-tool-output-cli", action="store_true", help="build actual CLI deterministic-choice output benchmark")
    ap.add_argument("--test-tool-prepare", action="store_true")
    ap.add_argument("--bench-tool-prepare", action="store_true")
    ap.add_argument("--test", action="store_true")
    ap.add_argument("--build-self-speculate",action="store_true",help="experimental read-only same-model fork probe host; no model run")
    ap.add_argument("--test-long-context", action="store_true", help="extend native acceptance to a real 14K-token sequence")
    ap.add_argument("--sanitize", action="store_true")
    ap.add_argument("--bench-chat", action="store_true")
    ap.add_argument("--test-kv-worker", action="store_true", help="real KV snapshot/worker correctness and cancellation acceptance")
    ap.add_argument("--bench-kv-worker", action="store_true", help="compare early KV restore with post-assembly restore")
    ap.add_argument("--test-stream", action="store_true", help="stream framing, versioned local file processing and real prefill acceptance")
    ap.add_argument("--bench-stream", action="store_true", help="replayed input pipeline versus wait-all baseline")
    ap.add_argument("--replay-stream", help="read-only local replay of an NDJSON file, or - for stdin")
    ap.add_argument("--audit-context", action="store_true", help="macOS context lifetime/branch audit; separate from sanitizers and chat benchmarks")
    ap.add_argument("--audit-kv-tokens", type=int, default=8192)
    ap.add_argument("--repeats", type=int, default=5)
    args = ap.parse_args()
    if args.test_long_context:
        args.test = True
    if not 1 <= args.repeats <= 30:
        ap.error("repeats must be 1..30")
    if args.sanitize and (args.bench_chat or args.bench_kv_worker or args.bench_stream or args.bench_tool_prepare or args.bench_tool_timing or args.bench_tool_balance or args.bench_tool_taskcost or args.bench_tool_isolate or args.bench_tool_launch or args.bench_resident_flow or args.bench_resident_stream):
        ap.error("do not mix sanitizer overhead with performance measurements")
    if args.bench_kv_worker and (args.bench_chat or args.audit_context):
        ap.error("run KV worker measurements separately from other performance probes")
    if args.bench_stream and (args.bench_chat or args.bench_kv_worker or args.audit_context or args.replay_stream):
        ap.error("run stream measurements separately from other performance/replay probes")
    if args.audit_context and (args.sanitize or args.bench_chat or platform.system() != "Darwin"):
        ap.error("context memory audit requires macOS and a separate unsanitized run")
    if not 8192 <= args.audit_kv_tokens <= 16384:
        ap.error("audit-kv-tokens must be 8192..16384 on this memory-limited test host")
    if platform.system() not in ("Darwin", "Linux"):
        ap.error("native Windows build acceptance remains pending")
    commit = subprocess.check_output(["git", "-C", str(args.sdk), "rev-parse", "HEAD"], text=True).strip()
    version = subprocess.run([str(args.runtime / "llama-server"), "--version"], text=True, capture_output=True, check=True)
    if commit != "f1e44dcc11d8802d107bd7331a3d3fd3e6f57b93" or commit[:9] not in version.stdout + version.stderr:
        ap.error("matching b10938 source and runtime required")
    out = (args.out or Path(tempfile.mkdtemp(prefix="agent-native-", dir=ROOT / "build"))).resolve()
    out.mkdir(parents=True, exist_ok=True)
    if (out / "manifest.json").exists():
        ap.error("choose a fresh --out to preserve evidence")
    commands, hashes = [], {}

    def run(cmd):
        commands.append(cmd)
        result = subprocess.run(cmd, text=True, capture_output=True)
        with (out / "compile.txt").open("a") as log:
            log.write(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    inc = [ROOT / "include", ROOT / "tests", args.sdk / "include", args.sdk / "ggml/include",
           args.sdk / "common", args.sdk / "vendor", args.sdk / "vendor/hash", args.sdk / "vendor/hash/sha256"]
    flags = ["-g", "-O1" if args.sanitize else "-O2", "-Wall", "-Wextra", "-Wpedantic"]
    flags += ["-I" + str(p) for p in inc[:2]]
    if args.metal_probe:
        flags += ["-DCXF_METAL_PROBE"]
    if args.audit_context:
        flags += ["-DCXF_CONTEXT_AUDIT"]
    if args.test_long_context:
        flags += ["-DCXF_LONG_CONTEXT_TEST"]
    for path in inc[2:]:
        flags += ["-isystem", str(path)]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    objects = []

    def compile_file(src, label, cpp=False, native=False):
        obj = out / (label + ".o")
        command = ["c++" if cpp else "cc", "-std=c++17" if cpp else "-std=c11"] + flags
        if native:
            command += ["-DCXF_NATIVE_LLAMA"]
        run(command + ["-c", str(src), "-o", str(obj)])
        hashes[str(src.relative_to(ROOT)) if src.is_relative_to(ROOT) else str(src)] = hashlib.sha256(src.read_bytes()).hexdigest()
        return str(obj)

    cfiles = sorted((ROOT / "src").glob("*.c"))
    cfiles += [ROOT / "tests" / n for n in ["agent_types.gen.c", "agent_caps.c", "agent_caps.gen.c",
               "agent_fs.c", "agent_fs.gen.c", "agent_net.c", "agent.c", "agent_http.c", "agent_llm.c", "agent_policy.c", "agent_sandbox.c", "agent_basic.c"]]
    for i, src in enumerate(cfiles):
        objects.append(compile_file(src, "base" + str(i)))
    objects.append(compile_file(args.sdk / "vendor/hash/sha256/sha256.c", "sha256"))
    objects.append(compile_file(ROOT / "tests/agent_native.cpp", "native", cpp=True))
    objects.append(compile_file(ROOT / "tests/agent_native_messages.cpp", "native_messages", cpp=True))
    worker = compile_file(ROOT / "tests/agent_kv_worker.cpp", "kv_worker", cpp=True)
    tool_prepare = compile_file(ROOT / "tests/agent_tool_prepare.cpp", "tool_prepare", cpp=True)
    link = flags + ["-L" + str(args.runtime), "-Wl,-rpath," + str(args.runtime),
                    "-lllama-common", "-lllama", "-lggml", "-lggml-base", "-lm"]
    ctl = compile_file(ROOT / "tests/agentctl.c", "ctl", native=True)
    config = compile_file(ROOT / "tests/agentctl_config.c", "config")
    run(["c++"] + objects + [worker, tool_prepare, ctl, config] + link + ["-pthread"] + ["-o", str(out / "agentctl-native")])
    if args.build_self_speculate:
        speculate=compile_file(ROOT/"tests/agent_speculate.cpp","speculate",cpp=True)
        host=compile_file(ROOT/"tests/self_speculate_native.cpp","speculate_host",cpp=True)
        run(["c++"]+objects+[speculate,host]+link+["-pthread","-o",str(out/"self-speculate-native")])
    if args.test:
        test = compile_file(ROOT / "tests/test_native_chat.c", "test")
        ref = compile_file(ROOT / "tests/native_chat_reference.cpp", "reference", cpp=True)
        run(["c++"] + objects + [test, ref] + link + ["-o", str(out / "test_native_chat")])
    if args.bench_chat:
        bench = compile_file(ROOT / "tests/bench_native_chat.cpp", "bench", cpp=True)
        run(["c++"] + objects + [bench] + link + ["-o", str(out / "bench_native_chat")])
    if args.audit_context:
        audit = compile_file(ROOT / "tests/bench_context_memory.cpp", "memory_audit", cpp=True)
        run(["c++"] + objects + [audit] + link + ["-o", str(out / "bench_context_memory")])
    stream = args.test_stream or args.bench_stream or args.replay_stream
    if args.test_kv_worker or args.bench_kv_worker:
        worker_test = compile_file(ROOT / "tests/test_kv_worker.cpp", "kv_worker_test", cpp=True)
        run(["c++"] + objects + [worker, worker_test] + link + ["-pthread", "-o", str(out / "test_kv_worker")])
    if stream or args.test_resident_stream or args.bench_resident_stream:
        stream_obj = compile_file(ROOT / "tests/agent_stream.cpp", "stream", cpp=True)
    if stream:
        stream_test = compile_file(ROOT / "tests/test_stream.cpp", "stream_test", cpp=True)
        run(["c++"] + objects + [worker, stream_obj, stream_test] + link + ["-pthread", "-o", str(out / "agent-stream-replay")])
    if args.test_tool_prepare or args.bench_tool_prepare or args.bench_tool_timing or args.bench_tool_balance or args.test_tool_balance or args.bench_tool_taskcost or args.test_tool_taskcost or args.bench_tool_isolate or args.test_tool_isolate or args.bench_tool_launch or args.test_tool_launch:
        tool_test = compile_file(ROOT / "tests/test_tool_prepare.cpp", "tool_prepare_test", cpp=True)
        tool_reference = compile_file(ROOT / "tests/native_chat_reference.cpp", "tool_reference", cpp=True)
        run(["c++"] + objects + [worker, tool_prepare, tool_test, tool_reference] + link + ["-pthread", "-o", str(out / "test_tool_prepare")])
    if args.test_tool_prepare_cli:
        cli_test = compile_file(ROOT / "tests/test_tool_prepare_ctl.c", "tool_prepare_ctl")
        run(["c++"] + objects + [worker, tool_prepare, cli_test, config] + link + ["-pthread", "-o", str(out / "test-tool-prepare-ctl")])
    if args.build_tool_output_cli or args.test_tool_output:
        output_ref=compile_file(ROOT / "tests/native_chat_reference.cpp", "output_reference", cpp=True)
        probe_objects=[]
        if args.metal_probe:
            assert platform.system()=="Darwin"
            probe_objects=[compile_file(ROOT/"tests/agent_metal_probe.mm","metal_probe",cpp=True)]
        output_ctl=compile_file(ROOT / "tests/test_tool_output_ctl.c", "output_ctl")
        run(["c++"]+objects+[worker,tool_prepare,output_ctl,config,output_ref]+probe_objects+link+(["-framework","Metal","-framework","Foundation"] if args.metal_probe else [])+["-pthread","-o",str(out/"test-tool-output-ctl")])
        producer=compile_file(ROOT / "tests/tool_output_producer.c", "output_producer")
        run(["cc",producer]+(["-fsanitize=address,undefined"] if args.sanitize else [])+["-o",str(out/"tool-output-producer")])
    if args.test_tool_output:
        output_test=compile_file(ROOT / "tests/test_tool_output.cpp", "output_test", cpp=True)
        run(["c++"]+objects+[worker,tool_prepare,output_test,output_ref]+link+["-pthread","-o",str(out/"test_tool_output")])
    if args.bench_tool_isolate or args.test_tool_isolate or args.bench_tool_launch or args.test_tool_launch:
        phase = compile_file(ROOT / "tests/tool_phase_driver.c", "tool_phase_driver")
        run(["cc", phase] + (["-fsanitize=address,undefined"] if args.sanitize else []) + ["-o", str(out / "tool-phase-driver")])
    if args.test_resident_flow or args.bench_resident_flow or args.test_resident_stream or args.bench_resident_stream:
        resident_tool = compile_file(ROOT / "tests/resident_tool.c", "resident_tool")
        run(["cc", resident_tool] + (["-fsanitize=address,undefined"] if args.sanitize else []) + ["-o", str(out / "resident-tool")])
    if args.test_resident_flow or args.bench_resident_flow:
        resident_test = compile_file(ROOT / "tests/test_resident_flow.cpp", "resident_flow", cpp=True)
        resident_ref = compile_file(ROOT / "tests/native_chat_reference.cpp", "resident_ref", cpp=True)
        run(["c++"] + objects + [worker,tool_prepare,resident_test,resident_ref] + link + ["-pthread","-o",str(out / "test_resident_flow")])
    if args.test_resident_stream or args.bench_resident_stream:
        resident_stream = compile_file(ROOT / "tests/test_resident_stream.cpp", "resident_stream", cpp=True)
        run(["c++"] + objects + [worker,stream_obj,resident_stream] + link + ["-pthread","-o",str(out / "test_resident_stream")])
    (out / "manifest.json").write_text(json.dumps({"llama_commit": commit, "sanitizers": args.sanitize,
        "commands": commands, "sources_sha256": hashes}, indent=2) + "\n")
    print(f"native build: {out}", flush=True)
    if args.test_tool_output:
        with (out/"tool-output-test.jsonl").open("w") as stdout,(out/"tool-output-test.log").open("w") as stderr:
            result=subprocess.run([str(out/"test_tool_output"),str(args.model),str(args.runtime),str(out)],stdout=stdout,stderr=stderr)
        if result.returncode:raise SystemExit("tool output correctness failed; artifacts retained")
        print("tool output correctness passed",flush=True)
    for label,enabled,benchmark in [("resident-flow",args.test_resident_flow or args.bench_resident_flow,args.bench_resident_flow),("resident-stream",args.test_resident_stream or args.bench_resident_stream,args.bench_resident_stream)]:
        if not enabled:continue
        binary="test_"+label.replace("-","_")
        with (out/(label+".jsonl")).open("w") as stdout,(out/(label+"-engine.log")).open("w") as stderr:
            result=subprocess.run([str(out/binary),str(args.model),str(args.runtime),str(out),"bench" if benchmark else "test",str(args.repeats)],stdout=stdout,stderr=stderr)
        if result.returncode:raise SystemExit(label+" failed; private artifacts retained")
        print(label+" completed",flush=True)

    if args.test_tool_prepare or args.bench_tool_prepare or args.bench_tool_timing or args.bench_tool_balance or args.test_tool_balance or args.bench_tool_taskcost or args.test_tool_taskcost or args.bench_tool_isolate or args.test_tool_isolate or args.bench_tool_launch or args.test_tool_launch:
        with (out / "tool-prepare.jsonl").open("w") as stdout, (out / "tool-prepare-engine.log").open("w") as stderr:
            result = subprocess.run([str(out / "test_tool_prepare"), str(args.model), str(args.runtime), str(out),
                                     "launch" if args.bench_tool_launch else "launch-test" if args.test_tool_launch else "isolate" if args.bench_tool_isolate else "isolate-test" if args.test_tool_isolate else "taskcost" if args.bench_tool_taskcost else "taskcost-test" if args.test_tool_taskcost else "balanced" if args.bench_tool_balance else "balanced-test" if args.test_tool_balance else "timing" if args.bench_tool_timing else "bench" if args.bench_tool_prepare else "test", str(args.repeats)], stdout=stdout, stderr=stderr)
        if result.returncode:
            raise SystemExit("Tool preparation failed; private raw/log artifacts retained")
        print("tool preparation completed", flush=True)
    if stream:
        mode = "replay" if args.replay_stream else "bench" if args.bench_stream else "test"
        argument = args.replay_stream if args.replay_stream else str(args.repeats)
        with (out / "stream.jsonl").open("w") as stdout, (out / "stream-engine.log").open("w") as stderr:
            result = subprocess.run([str(out / "agent-stream-replay"), str(args.model), str(args.runtime), str(out), mode, argument], stdout=stdout, stderr=stderr)
        if result.returncode:
            print((out / "stream-engine.log").read_text()[-5000:])
            raise SystemExit(result.returncode)
        rows = [json.loads(line) for line in (out / "stream.jsonl").read_text().splitlines()]
        for row in rows:
            if row.get('kind') in ('correctness','closed') or mode=='replay': print(json.dumps(row,ensure_ascii=False),flush=True)
        summary = []
        for scenario, gap, variant in sorted({(r['scenario'],r['gap_ms'],r['mode']) for r in rows if r.get('kind')=='timing'}):
            group = [r for r in rows if r.get('kind')=='timing' and (r['scenario'],r['gap_ms'],r['mode'])==(scenario,gap,variant)]
            entry = {'scenario':scenario,'gap_ms':gap,'mode':variant,'samples':len(group),'correct':all(r['correct'] for r in group)}
            for key in ['input_ms','post_input_ready_ms','total_ready_ms','total_first_token_ms','total_output_ms','render_ms','tokenize_ms','prefill_ms','tool_ms','input_evaluated_tokens','input_decode_calls','tokenized_tokens','tokenizer_calls','tools_finished_before_end','pending_peak','backpressure']:
                vals = [r[key] for r in group];entry[key]={'median':statistics.median(vals),'min':min(vals),'max':max(vals)}
            summary.append(entry)
            print(f"{scenario} gap={gap}ms {variant}: after_input={entry['post_input_ready_ms']['median']:.3f}ms total_ready={entry['total_ready_ms']['median']:.3f}ms decode={entry['input_decode_calls']['median']}",flush=True)
        (out / "stream-summary.json").write_text(json.dumps(summary,indent=2)+'\n')
    if args.test_kv_worker or args.bench_kv_worker:
        with (out / "kv-worker.jsonl").open("w") as stdout, (out / "kv-worker-engine.log").open("w") as stderr:
            result = subprocess.run([str(out / "test_kv_worker"), str(args.model), str(args.runtime), str(out),
                                     "bench" if args.bench_kv_worker else "test", str(args.repeats)], stdout=stdout, stderr=stderr)
        if result.returncode:
            print((out / "kv-worker-engine.log").read_text()[-5000:])
            raise SystemExit(result.returncode)
        rows = [json.loads(line) for line in (out / "kv-worker.jsonl").read_text().splitlines()]
        for row in rows:
            if row['kind'] in ('correctness','closed'):
                print(json.dumps(row), flush=True)
        summary = []
        for words, records, history, window, mode in sorted({(r['words'],r['records'],r['history_records'],r['window_ms'],r['mode']) for r in rows if r['kind']=='timing'}):
            group = [r for r in rows if r['kind']=='timing' and (r['words'],r['records'],r['history_records'],r['window_ms'],r['mode'])==(words,records,history,window,mode)]
            entry = {'words':words,'records':records,'history_records':history,'window_ms':window,'mode':mode,'samples':len(group),'correct':all(r['correct'] for r in group)}
            for key in ['assembly_ms','restore_ms','overlap_ms','post_submit_prepare_ms','post_submit_first_token_ms','total_first_token_ms','restored_bytes','suffix_decode_tokens']:
                vals = [r[key] for r in group]
                entry[key] = {'median':statistics.median(vals),'min':min(vals),'max':max(vals)}
            summary.append(entry)
            print(f"words={words} records={records} history={history} window={window}ms {mode}: total={entry['total_first_token_ms']['median']:.3f}ms after_submit={entry['post_submit_first_token_ms']['median']:.3f}ms restore={entry['restore_ms']['median']:.3f}ms", flush=True)
        (out / "kv-worker-summary.json").write_text(json.dumps(summary,indent=2)+'\n')
    if args.test:
        with (out / "stdout.txt").open("w") as stdout, (out / "engine.log").open("w") as stderr:
            result = subprocess.run([str(out / "test_native_chat"), str(args.model), str(args.runtime)], stdout=stdout, stderr=stderr)
        print((out / "stdout.txt").read_text(), end="")
        if result.returncode:
            print((out / "engine.log").read_text()[-5000:])
            raise SystemExit(result.returncode)
    if args.audit_context:
        with (out / "memory.jsonl").open("w") as stdout, (out / "memory-engine.log").open("w") as stderr:
            result = subprocess.run([str(out / "bench_context_memory"), str(args.model), str(args.runtime),str(args.audit_kv_tokens)], stdout=stdout, stderr=stderr)
        if result.returncode:
            raise SystemExit(f"memory audit failed: inspect {out / 'memory-engine.log'}")
        rows = [json.loads(line) for line in (out / "memory.jsonl").read_text().splitlines()]
        summary = []
        for shape, size, mode in sorted({(r['shape'], r['size'], r['mode']) for r in rows if r['kind'] == 'memory'}):
            group = [r for r in rows if r['kind'] == 'memory' and (r['shape'], r['size'], r['mode']) == (shape,size,mode)]
            peak = [max(0,r['sampled_heap_peak']-r['heap_before']) for r in group]
            entry = {'shape':shape,'size':size,'mode':mode,'samples':len(group),'rc':group[0]['rc'],
                     'input_bytes':group[0]['input_bytes'],'stored':group[-1]['stored'],
                     'heap_extra_median':statistics.median(peak),'heap_extra_min':min(peak),'heap_extra_max':max(peak),
                     'steady_cxf_bytes':group[-1]['cxf_bytes']}
            summary.append(entry)
            print(f"{shape} size={size} {mode}: rc={entry['rc']} peak_extra={entry['heap_extra_median']:.0f}B steady_cxf={entry['steady_cxf_bytes']}B")
        (out / "memory-summary.json").write_text(json.dumps(summary,indent=2)+'\n')
        for r in rows:
            if r['kind'] in ('branch_shared','branch_suffix','closed'):
                print(json.dumps(r))
    if args.bench_chat:
        with (out / "bench.jsonl").open("w") as stdout, (out / "bench-engine.log").open("w") as stderr:
            result = subprocess.run([str(out / "bench_native_chat"), str(args.model), str(args.runtime), str(args.repeats)], stdout=stdout, stderr=stderr)
        if result.returncode:
            raise SystemExit(f"benchmark failed: inspect {out / 'bench-engine.log'}")
        rows = [json.loads(line) for line in (out / "bench.jsonl").read_text().splitlines()]
        summary = []
        for lines in sorted({r['prefix_lines'] for r in rows}):
            for mode in sorted({r['mode'] for r in rows}):
                group = [r for r in rows if r['prefix_lines'] == lines and r['mode'] == mode]
                entry = {'prefix_lines': lines, 'mode': mode, 'samples': len(group), 'correct': all(r['correct'] for r in group)}
                for key in ['prefix_tokens', 'wall_ms', 'front_ms', 'render_ms', 'tokenize_ms', 'guard_ms', 'commit_ms', 'prefill_ms', 'tokenized', 'evaluated', 'cache_hits', 'cache_misses']:
                    values = [r[key] for r in group]
                    entry[key] = {'median': statistics.median(values), 'min': min(values), 'max': max(values)}
                summary.append(entry)
                print(f"lines={lines} {mode}: wall={entry['wall_ms']['median']:.3f}ms front={entry['front_ms']['median']:.3f}ms tokenized={entry['tokenized']['median']} evaluated={entry['evaluated']['median']}")
        (out / "bench-summary.json").write_text(json.dumps(summary, indent=2) + '\n')


if __name__ == "__main__":
    main()
