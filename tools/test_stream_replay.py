#!/usr/bin/env python3
"""Check the local NDJSON replay CLI; external replay never runs source code."""
import argparse
import json
from pathlib import Path
import subprocess


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", type=Path, required=True)
    ap.add_argument("--model", type=Path, required=True)
    ap.add_argument("--runtime", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    example = Path(__file__).resolve().parents[1] / "docs/examples/local-stream.ndjson"
    wire = example.read_bytes()
    results = []

    def run(name, source, data, expected, result):
        scratch = args.out / name
        scratch.mkdir()
        command = [str(args.binary.resolve()), str(args.model.resolve()), str(args.runtime.resolve()), str(scratch.resolve()), "replay", source]
        proc = subprocess.run(command, input=data, capture_output=True, timeout=180)
        (scratch / "stdout.txt").write_bytes(proc.stdout)
        (scratch / "stderr.txt").write_bytes(proc.stderr)
        assert proc.returncode == expected, (name, proc.returncode, proc.stderr[-2000:])
        assert all(x not in proc.stderr for x in (b"AddressSanitizer", b"runtime error:", b"LeakSanitizer"))
        rows = [json.loads(line) for line in proc.stdout.decode().splitlines()]
        assert rows[0]["result"] == result, (name, rows)
        if not result:
            assert rows[0]["tokens"] > 0 and isinstance(rows[0]["preview"], str)
            assert "read-only" in rows[0]["scope"]
            assert rows[-1]["passed"]
        assert not list(scratch.glob("cxf-kv-*"))
        assert not list(scratch.rglob("unit.c")) and not list(scratch.rglob("unit"))
        results.append({"case": name, "exit": proc.returncode, "result": result, "passed": True})

    run("file", str(example), None, 0, 0)
    run("stdin", "-", wire, 0, 0)
    run("unfinished", "-", b'{"seq":1,"kind":"text","text":"partial', 1, 7)
    run("invalid-utf8", "-", b'{"seq":1,"kind":"text","text":"\xc0\xaf"}\n', 1, 7)
    run("missing-end", "-", b'{"seq":1,"kind":"text","text":"no end"}\n', 1, 7)
    run("missing-file", str((args.out / "does-not-exist.ndjson").resolve()), None, 1, 8)
    (args.out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    print("PASS stream replay CLI: file/stdin, real local preview, read-only file callback, malformed/unfinished input, cleanup")


if __name__ == "__main__":
    main()
