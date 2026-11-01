#!/usr/bin/env python3
"""probe_latency —— 回路到底在等什么，以及并发能不能救（docs/agent.md §4.17）。

它回答一个**结构问题**：EXECUTE 里那次同步 `exec` 把 tick 卡了多久，
把 K 个请求同时打出去以后墙钟怎么变。判据是两条曲线：

    K=1  →  p50 就是单次延迟  ⇒ 回路吞吐 = 1 / 延迟
    K=K* →  墙钟不再下降      ⇒ 服务端到容量上限了，再多并发只是排队（还烧钱）

产出 `build/latency.csv`，与 `make budget` 同族：**产出曲线，不产出过/不过。**

用法：
    make probe                     # 默认 K=1,2,4,8，每档 2 轮
    make probe K=1,4 R=3 Q="..."   # 自定义

★ 为什么用线程而不是 select/poll：这里量的是**服务端**的容量，不是我们客户端的
多路能力。用一个必定能并发的客户端去压，才能把"我们的同步实现"这个变量摘掉 ——
否则量到的永远是自己的形状。
"""
import csv
import json
import os
import sys
import threading
import time
import urllib.error
import urllib.request

URL = os.environ.get("CXF_LLM_URL", "")
KEY = os.environ.get("CXF_LLM_KEY", "")
MODEL = os.environ.get("CXF_LLM_MODEL", "")
TIMEOUT = float(os.environ.get("CXF_LLM_TIMEOUT_MS", "30000")) / 1000.0 or 30.0

DEFAULT_Q = "只回答一个阿拉伯数字，不要解释：37 乘 41 等于多少？"


def one(i, q, out, deadline):
    body = json.dumps({
        "model": MODEL,
        "messages": [{"role": "user", "content": q}],
        "temperature": 0,
    }).encode()
    req = urllib.request.Request(URL, data=body, method="POST")
    req.add_header("Content-Type", "application/json")
    if KEY:
        req.add_header("Authorization", "Bearer " + KEY)
    # 与 agentctl 同一个头：服务端日志能按 (run, call) 和这边对齐
    req.add_header("X-CXF-Ctx", f"run=probe{os.getpid()} call={i}")

    t0 = time.monotonic()
    try:
        with urllib.request.urlopen(req, timeout=deadline) as r:
            n = len(r.read())
            out[i] = (time.monotonic() - t0, "ok", n)
    except urllib.error.HTTPError as e:
        out[i] = (time.monotonic() - t0, f"http{e.code}", 0)
    except Exception as e:                       # noqa: BLE001
        out[i] = (time.monotonic() - t0, type(e).__name__, 0)


def worker(k, r, q, samples, idx, deadline):
    """一个 worker 顺序跑 r 次。闭环比"一次并发 K 个"更接近真实回路。"""
    for j in range(r):
        tag = idx * r + j
        one(tag, q, samples, deadline)


def pct(xs, p):
    if not xs:
        return 0.0
    xs = sorted(xs)
    i = min(len(xs) - 1, int(round(p * (len(xs) - 1))))
    return xs[i]


def measure(k, r, q, deadline):
    samples = {}
    th = [threading.Thread(target=worker, args=(k, r, q, samples, i, deadline))
          for i in range(k)]
    t0 = time.monotonic()
    for t in th:
        t.start()
    for t in th:
        t.join()
    wall = time.monotonic() - t0

    lat = [v[0] for v in samples.values()]
    ok = sum(1 for v in samples.values() if v[1] == "ok")
    bad = sorted({v[1] for v in samples.values() if v[1] != "ok"})
    rps = (ok / wall) if wall > 0 else 0.0
    return {
        "k": k, "r": r, "n": k * r, "ok": ok,
        "fail": ",".join(bad) if bad else "",
        "wall_s": round(wall, 2),
        "p50_s": round(pct(lat, 0.50), 2),
        "p95_s": round(pct(lat, 0.95), 2),
        "rps": round(rps, 3),
    }


def main(argv):
    ks, r, q = [1, 2, 4, 8], 2, DEFAULT_Q
    for a in argv[1:]:
        if a.startswith("K="):
            ks = [int(x) for x in a[2:].split(",")]
        elif a.startswith("R="):
            r = int(a[2:])
        elif a.startswith("Q="):
            q = a[2:]

    if not URL or not MODEL:
        print("agentctl: 缺 CXF_LLM_URL / CXF_LLM_MODEL", file=sys.stderr)
        return 2

    rows = []
    print(f"-- {MODEL} @ {URL} | 每档 {r} 轮/worker，deadline {TIMEOUT:.0f}s --")
    print(f"{'K':>2} {'ok/n':>6} {'wall':>7} {'p50':>6} {'p95':>6} {'rps':>6}  fail")
    for k in ks:
        m = measure(k, r, q, TIMEOUT)
        rows.append(m)
        print(f"{m['k']:>2} {m['ok']:>3}/{m['n']:<2} {m['wall_s']:>6.2f}s"
              f" {m['p50_s']:>5.2f}s {m['p95_s']:>5.2f}s {m['rps']:>6.3f}  {m['fail']}",
              flush=True)

    os.makedirs("build", exist_ok=True)
    path = "build/latency.csv"
    # 合并而不是覆盖：一档一档分开跑时（K=8,16,32 单独一次），覆盖会让曲线只剩最后一段。
    # 同 K 的新行替换旧行 —— 重复跑同一档是"重测"，不是"再画一个点"。
    merged = {int(r["k"]): r for r in rows}
    if os.path.exists(path):
        with open(path, newline="") as f:
            for old in csv.DictReader(f):
                k = int(old["k"])
                if k not in merged:
                    merged[k] = old
    fields = ["k", "r", "n", "ok", "fail", "wall_s", "p50_s", "p95_s", "rps"]
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for k in sorted(merged):
            w.writerow(merged[k])
    print(f"→ {path}（累计 {len(merged)} 档：{sorted(merged)}）")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
