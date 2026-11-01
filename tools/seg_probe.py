#!/usr/bin/env python3
"""段化档位探针：跑一批任务，把 `[batch]` / `[seg]` 聚合成分布（§4.18.17）。

它回答的不是"跑通了吗"，而是三个**有序**的问题：

    ① p̂  = ok / (bad + rejected + ok)      ⇒ 这个模型的格式能力够不够撑 N>1
    ② N 的分布                             ⇒ 它自己愿意/能够一次给几段
    ③ k 的分布（只在 ① 通过后才有意义）      ⇒ 回退能省几段

顺序不能反：p̂ 低时批次根本不产生，②③ 的分母接近 0，那时算出来的期望是在算空集合。

用法：
    export CXF_LLM_URL=… CXF_LLM_MODEL=… CXF_LLM_KEY=…
    python3 tools/seg_probe.py --root /path/to/repo --reps 3
    python3 tools/seg_probe.py --root … --tasks my_tasks.txt --reps 5 --trace-dir /tmp/seg

为什么要有 `--reps`：采样是有方差的，一次读数没有统计意义
（§4.18.16 十一那次 E[N]-E[k]=0 就是一次读数的全部意义 —— 没有）。

设计取舍：
  · **不并发**。并发会引入配额闸门（§4.17.8），而闸门下记录变成完成序 ——
    那正好污染我们想量的东西。这一段要的是"单任务干净读数"，吞吐不是目标。
  · **不判任务成功**。判据是 k/N，不是答案对不对；把答案对不对混进来，
    会把"模型答错"和"模型格式坏"搅成一个数（同族于 p̂ 分三格的理由）。
"""
import argparse
import os
import re
import subprocess
import sys

RE_BATCH = re.compile(r"\[batch\] ok (\d+) \| bad (\d+) \| rejected (\d+)")
RE_N     = re.compile(r"\[seg\] 批大小 n:(.*)")
RE_K     = re.compile(r"\[seg\] 失败位置 k:(.*)")
RE_PAIR  = re.compile(r"(\d+)=(\d+)")

# 默认任务集：**每一条都要求"一次给出多处查询"**，否则模型天然只会发一条 CALL，
# 量出来的 N 恒为 1 —— 那不是模型不行，是题出得不对（激励结构决定 N）。
# 全部只读（fs.grep，trust=IO），不触发写入族。
DEFAULT_TASKS = [
    "在这个仓库里分别找 fs_grep_exec 和 write_replace 定义在哪个文件，一次给出两个查询",
    "分别找 CXF_PHASE_INPUT 与 CXF_PHASE_LATE 出现的位置，一次给出两个查询",
    "分别找 blob_append_grow 与 blob_from 的调用点，一次给出两个查询",
    "分别找 chain_pick 与 chain_advance 的定义，一次给出两个查询",
    "分别找 agent_act_decode 与 agent_act_encode 的定义，一次给出两个查询",
]


def hist_of(line):
    """把 ` 2=1 3=2` 这样的片段解析成 {2:1, 3:2}。"""
    if not line or "(无" in line:
        return {}
    return {int(k): int(v) for k, v in RE_PAIR.findall(line)}


def run_one(root, task, env, trace_dir, idx):
    args = ["build/agentctl", "--root", root]
    if trace_dir:
        args += ["--trace", os.path.join(trace_dir, "run-%03d.trace" % idx)]
    p = subprocess.run(args, input=task + "\n", env=env,
                       capture_output=True, text=True)
    err = p.stderr
    mb = RE_BATCH.search(err)
    mn = RE_N.search(err)
    mk = RE_K.search(err)
    ok = bad = rej = 0
    if mb:
        ok, bad, rej = (int(mb.group(i)) for i in (1, 2, 3))
    return {
        "ok": ok, "bad": bad, "rejected": rej,
        "n": hist_of(mn.group(1) if mn else ""),
        "k": hist_of(mk.group(1) if mk else ""),
        "rc": p.returncode,
        "stderr": err,
    }


def merge(dst, src):
    for k, v in src.items():
        dst[k] = dst.get(k, 0) + v


def mean(h):
    n = sum(h.values())
    return (sum(k * v for k, v in h.items()) / n if n else 0.0), n


def solo(h):
    """直方图只有一个非零桶时返回那个值，否则 None。

    为什么需要它：`E[N] - E[k]` 只有在 **N 与 k 来自同一批** 时才是 E[N-k]。
    全局边际相减是两个不同分母上的数 —— `E[N]` 的分母是全部批次，
    `E[k]` 的分母是**有失败的**批次。相减的值没有意义（会让"失败率很低"看起来像
    "回退省很多"）。所以配对只认"一次运行里 N 唯一"的那种，配不上的单列。
    """
    vals = [k for k, v in h.items() if v]
    return vals[0] if len(vals) == 1 else None


def check_binary(repo):
    """build/agentctl 必须存在且不比源码旧。

    为什么值得写这段：`make` 的默认目标**不包含** `build/agentctl`（它是手工工具，
    不是测试），于是一个改过 `agent.c` 但只跑过 `make` 的仓库会拿**旧二进制**去测 ——
    读数看起来完美，测的是上一版的代码。本仓库为同类问题花过整轮时间（变异验证那次的
    "假绿"就是 make 按 mtime 没重建）。判据：**GREEN 先怀疑构建竞态，再怀疑测试空白。**

    ★ 注意 `repo` 是**本仓库目录**，不是 `--root`。`--root` 是 agentctl 的沙箱根
      （给 fs 能力用的目录），两者是两件事 —— 我第一版就拿 `--root` 去找二进制，
      于是只要沙箱根不在仓库里就必然报"没有 build/agentctl"。
    """
    exe = os.path.join(repo, "build/agentctl")
    if not os.path.exists(exe):
        sys.exit("没有 build/agentctl —— 先 `make build/agentctl`")
    t_exe = os.path.getmtime(exe)
    for d in ("tests", "src", "include"):
        p = os.path.join(repo, d)
        if not os.path.isdir(p):
            continue
        for fn in os.listdir(p):
            if not fn.endswith((".c", ".h")):
                continue
            # 跳过生成物：`make gen` 每次重写 .gen.*，内容一样但 mtime 抬高 ⇒
            # 这条检查会天天喊。**一个总在喊的检查等于没有检查** —— 人会学会忽略它，
            # 于是真正被忘记重建的那次也一起被忽略。
            if ".gen." in fn:
                continue
            if os.path.getmtime(os.path.join(p, fn)) > t_exe:
                sys.exit("build/agentctl 比 %s/%s 旧 —— 先 `make build/agentctl`，"
                         "否则测的是旧代码" % (d, fn))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--tasks")
    ap.add_argument("--trace-dir")
    a = ap.parse_args()

    for v in ("CXF_LLM_URL", "CXF_LLM_MODEL"):
        if not os.environ.get(v):
            sys.exit("缺 %s —— 探针不猜端点（同 agentctl：配置只能来自环境）" % v)
    check_binary(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    if a.trace_dir:
        os.makedirs(a.trace_dir, exist_ok=True)

    tasks = DEFAULT_TASKS
    if a.tasks:
        with open(a.tasks, encoding="utf-8") as f:
            tasks = [ln.strip() for ln in f if ln.strip()]

    env = dict(os.environ)
    N, K = {}, {}
    ok = bad = rej = 0
    runs = no_attempt = crash = 0
    pairs = []          # 只在"一次运行里 N 唯一且有失败"时成立：(n, k)
    unpaired = 0        # 有 N 也有 k，但 N 不唯一（跑了多批）⇒ 不配对，单列

    for rep in range(a.reps):
        for task in tasks:
            runs += 1
            r = run_one(a.root, task, env, a.trace_dir, runs)
            ok += r["ok"]; bad += r["bad"]; rej += r["rejected"]
            merge(N, r["n"]); merge(K, r["k"])
            if r["rc"] != 0:
                crash += 1
                tail = [ln for ln in r["stderr"].strip().splitlines() if ln.strip()]
                print("  run %3d  rep%d  **rc=%d**（进程本身失败，不计入统计）：%s"
                      % (runs, rep + 1, r["rc"], tail[-1] if tail else "（无 stderr）"))
                continue
            if r["ok"] == 0 and r["bad"] == 0 and r["rejected"] == 0:
                no_attempt += 1
            sn, sk = solo(r["n"]), solo(r["k"])
            if sn is not None and sn > 0:
                if r["k"]:
                    if sk is not None:
                        pairs.append((sn, sk))
                    else:
                        unpaired += 1
            print("  run %3d  rep%d  ok=%d bad=%d rej=%d  n=%s k=%s"
                  % (runs, rep + 1, r["ok"], r["bad"], r["rejected"],
                     r["n"] or "-", r["k"] or "-"))

    tried = ok + bad + rej
    print()
    print("runs %d（根本没尝试产出 %d，进程失败 %d）| 尝试 %d"
          % (runs, no_attempt, crash, tried))

    if tried == 0:
        # 三种"零"必须分开报，因为下一步动作完全不同（同族于 bad vs rejected）：
        #   crash  ⇒ 端点/构建/参数，**模型完全没被问到**，改提示词是白改
        #   no_attempt ⇒ 问到了，它一条 CALL 都不发 ⇒ 提示词激励 / 它认为不需要工具
        #   其余   ⇒ 上面已经 return
        if crash:
            print("结论：**%d/%d 次进程本身失败** ⇒ 话都没说到模型，不是模型问题。"
                  "先修端点和构建，别碰提示词。" % (crash, runs))
        else:
            print("结论：**没有任何产出尝试**（问到了但一条 CALL 都没发）"
                  " ⇒ p̂ 无定义。先改提示词的激励结构，别急着看 k。")
        return

    p = ok / tried
    print("① p̂ = %.3f  (ok %d / bad %d / rejected %d)" % (p, ok, bad, rej))
    if bad > rej * 2:
        print("   ↳ bad 占多：模型**格式能力**不够（处置 = 降 N 或退到文本单条通道）")
    elif rej > bad * 2:
        print("   ↳ rejected 占多：**目录/天花板**问题，不是模型问题（别拿换模型当解法）")

    eN, nN = mean(N)
    print("② N 分布：%s" % (N or "（无）"))
    print("   E[N] = %.2f（只有 N≥2 的批次才对段化有意义；N 恒 1 = 题没激励多段）" % eN)

    if not K:
        print("③ k：**本次没有失败样本** ⇒ k 无定义，回退开关暂不能定（加 reps 或加难度）")
        return

    eK, nK = mean(K)
    print("③ 失败批 %d / 有批次 %d（%.0f%%）" % (nK, nN, 100.0 * nK / nN if nN else 0.0))
    print("   k 分布：%s" % K)
    print("   E[k|失败] = %.2f" % eK)

    if pairs:
        eNf = sum(x for x, _ in pairs) / len(pairs)
        eKf = sum(y for _, y in pairs) / len(pairs)
        eS = sum(x - y for x, y in pairs) / len(pairs)
        print("   配对 %d 批（N 与 k 同批可见）⇒ E[N|失败] = %.2f | E[k|失败] = %.2f"
              " | **E[N-k] = %.2f**" % (len(pairs), eNf, eKf, eS))
    else:
        print("   配对 0 批 ⇒ **E[N-k] 无定义**（不要拿 E[N]-E[k] 顶替，见 solo() 的注释）")
    if unpaired:
        print("   另有 %d 批 N 不唯一（一次运行跑了多批）⇒ 不参与配对" % unpaired)

    print()
    print("读法：E[N-k] > 0 才说明「只重写后缀」能省段 —— 它是**逐批配对的均值**，")
    print("      不是两个边际均值相减：E[N] 的分母是全部批次，E[k] 的分母只有失败批次。")
    print("      p̂ < 0.90 时先降 N 再谈这个数 —— 否则样本本身是被原子性筛过一遍的。")


if __name__ == "__main__":
    main()
