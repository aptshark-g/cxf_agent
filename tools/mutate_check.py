#!/usr/bin/env python3
"""变异验证：逐个改坏实现，重建，跑测试，数红，还原。

用法: python3 tools/mutate_check.py <file> <old> <new> <label>
退出码 0 = 变异后被测试抓到（有 FAIL）；1 = 假绿；2 = 锚点没找到 / 构建失败；
       3 = 写后校验不过 / 二进制没重建 / 还原后自检不过。

★ 本机（macOS + WorkBuddy 沙箱）三条坑，全部踩过，症状全是"假绿"：
  1. 文件写入被代理中转，写完不代表已落盘 ⇒ 每次写后回读校验，不过就重试。
  2. os.remove / os.utime 同样被延迟 ⇒ 删 .o、touch 都不可靠，make 会认为
     "无需重建"，跑的是旧二进制。所以用 `make -B` 强制全建。
  3. 即便 make -B，也要用**二进制哈希**确认重建真的发生了：变异前后
     build/test_llm 与 build/test_agent 的 sha256 必须变。哈希没变 ⇒
     这次构建是旧的，重试。没有这一条，前面两条都可能静默失效。
"""
import hashlib
import os
import subprocess
import sys
import time

path, old, new, label = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
BINS = ["./build/test_llm", "./build/test_agent"]


def sha(p):
    with open(p, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def write_verify(content):
    """写文件并回读校验；代理延迟时重试。"""
    for _ in range(60):
        with open(path, "w", encoding="utf-8") as f:
            f.write(content)
        with open(path, "r", encoding="utf-8") as f:
            if f.read() == content:
                return True
        time.sleep(0.05)
    return False


def build_force():
    """make -B 强制全建。返回 True = 成功。"""
    b = subprocess.run(["make", "-B", "-j8"], capture_output=True, text=True)
    if b.returncode != 0:
        print(f"BUILDFAIL {label}")
        print(b.stderr[-1500:])
        sys.exit(2)
    return True


def run_tests():
    out = ""
    for t in BINS:
        r = subprocess.run([t], capture_output=True, text=True)
        out += r.stdout
    return [l for l in out.splitlines() if "FAIL" in l]


with open(path, "r", encoding="utf-8") as f:
    orig = f.read()

if orig.count(old) != 1:
    print(f"SKIP  {label}: 锚点出现 {orig.count(old)} 次（要恰好 1 次）")
    sys.exit(2)

# 基线：干净二进制的哈希
build_force()
base = {p: sha(p) for p in BINS}
if not write_verify(orig):
    print(f"WRITEFAIL {label}")
    sys.exit(3)

result = 1
try:
    mutated = orig.replace(old, new, 1)
    if not write_verify(mutated):
        print(f"WRITEFAIL {label}: 变异写后校验不过")
        sys.exit(3)

    # 建到哈希变化为止 —— 哈希没变说明这次构建是旧的（沙箱延迟）
    ok = False
    for attempt in range(6):
        build_force()
        if any(sha(p) != base[p] for p in BINS):
            ok = True
            break
        time.sleep(0.2)
    if not ok:
        print(f"STALE {label}: 二进制哈希没变，重建没生效")
        sys.exit(3)

    lines = run_tests()
    result = 0 if lines else 1
    print(f"{'RED  ' if lines else 'GREEN'} {label}: {len(lines)} FAIL")
    for l in lines[:6]:
        print(f"      {l.strip()}")
finally:
    if not write_verify(orig):
        print("      !! 还原写后校验不过 —— 树被污染了")
        result = 3
    else:
        bad = None
        for _ in range(6):
            build_force()
            bad = run_tests()
            if not bad:
                break
            time.sleep(0.2)
        if bad:
            print(f"      !! 还原后自检不过（{len(bad)} FAIL）—— 树被污染了")
            result = 3

sys.exit(result)
