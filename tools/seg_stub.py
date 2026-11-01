#!/usr/bin/env python3
"""段化读数的**干跑对端**：一个只会念剧本的 chat-completions 服务（§4.18.16 十一）。

它不是 mock 能力，而是**协议层的对端** —— agentctl 走真实 HTTP、真实 JSON 解析、
真实 tool_calls 通道、真实 fs.grep。所以 `[seg]` 读数是从回路的真实路径里长出来的，
而不是从测试桩里直接塞进计数器的。

为什么需要它：k 的分布要真实模型，而模型不是随时都在（没 endpoint / 在省钱 / 换厂商）。
没有对端，插好的仪器就**永远没被跑过** —— 那和没插是一回事，只是看起来更完整。

用法：
    python3 tools/seg_stub.py 18099 &
    printf '找 TOK\\n' | CXF_LLM_URL=http://127.0.0.1:18099/v1/chat/completions \\
        CXF_LLM_MODEL=stub build/agentctl --root /tmp/segroot --trace /tmp/seg.trace

剧本（可改，改完就是另一个 k/N 样本）：
    请求体里**还没有** role=tool 消息 ⇒ 第 1 轮 ⇒ 两条 fs.grep：
        seq1 正常（NOSUCHTOKEN，0 命中 → OK）
        seq2 空参数（handler 报 FORMAT ⇒ #fail 2）
    请求体里**已有** role=tool 消息 ⇒ 第 2 轮 ⇒ DONE（回路收口）

★ 轮次由**请求体自己**决定，不由服务端计数器决定。
  这是本项目对 mock 的硬要求："重跑代价为 0 的实现必须不改变自身状态，mock 也不例外"。
  早先的写法用一个全局 ROUNDS 计数器，结果是**同一个服务只对第一次调用成立** ——
  探针连跑 15 次里 14 次在第一轮就收到 DONE，`[batch]` 分母恒 0，
  看起来像"模型不肯分片"，其实是**对端自己记住了上一次**。
  判据：对端的状态必须能从输入推出来，否则它测的是它自己的历史。

★ 它**不产生结论**：一次跑的 E[N]-E[k] 是 0 还是 1 都没有统计意义。
  它证明的是"仪器接得通、记录形状对"，仅此而已。
"""
import argparse
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer


def round_of(body):
    """从请求体推断这是第几轮：出现过 role=tool 就是第二轮及以后。

    为什么不用消息条数 / 计数器：条数依赖系统提示词是否插入、计数器跨进程存活。
    "有没有工具结果"是唯一**由协议本身保证**、且与实现无关的判据。
    """
    try:
        j = json.loads(body)
    except Exception:
        return 1
    for m in j.get("messages", ()):
        if m.get("role") == "tool":
            return 2
    return 1


def reply(payload):
    return json.dumps({"choices": [{"message": payload}]}, ensure_ascii=False)


def call(cid, name, args):
    """tool_calls 的一条。arguments 是**字符串里套 JSON**（见 agent_llm parse_one_call）。"""
    return {"id": cid, "type": "function",
            "function": {"name": name, "arguments": json.dumps({"args": args})}}


class H(BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(n) if n else b""
        rnd = round_of(body)

        log = self.server.trace
        if log:
            with open(log, "ab") as f:
                f.write(b"\n===== REQ round=%d =====\n" % rnd + body[:4000])

        sc = self.server.scenario
        if rnd == 1 and sc != "idle":
            good = call("c1", "fs.grep", "NOSUCHTOKEN")   # 0 命中 ⇒ OK
            bad = call("c2", "fs.grep", "")               # 空参数 ⇒ FORMAT
            # 为什么不是一个剧本：探针有**四条**印字分支，一条剧本只点亮一条。
            #   失败在最后一段 ⇒ 后缀为空 ⇒ E[N-k] 必然是 0（负样本）
            #   失败在第一段   ⇒ E[N-k] = 1（正样本）
            #   两段都成功     ⇒ K 为空 ⇒ 走"k 无定义"那条
            #   一条 CALL 都不发 ⇒ 走"没有任何产出尝试"那条
            # 没跑过的分支等于没写（本仓库的老账：M10 那条断言全绿是因为从没被跑到）。
            if sc == "fail-last":
                calls = [good, bad]
            elif sc == "fail-first":
                calls = [bad, good]
            else:                                    # ok
                calls = [good, good]
            out = reply({"content": "", "tool_calls": calls})
        else:
            out = reply({"content": "DONE\nstub 收口\n"})

        # --garbage：200 但正文不是 chat/completions 的形状。
        # 它点亮的是映射里唯一一条**真·模型不守格式**的分支 —— 那条分支
        # 与 401/429/5xx 走同一个 rc，只能靠 status 区分，所以必须单独跑过。
        if self.server.garbage:
            out = '{"not":"chat/completions"}'

        raw = out.encode()
        # --status N：把所有响应的状态码改成 N（正文仍是一段 JSON error）。
        # 用途只有一个：**证明 agentctl 那边 401/429/5xx 的映射真的对**。
        # 没有它，那段映射就是"写完没跑过"——本仓库的老账是"没被跑到的分支等于没写"。
        st = self.server.status or 200
        if st != 200:
            raw = json.dumps({"error": {"message": "stub forced status"}}).encode()
        self.send_response(st)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def log_message(self, *a):
        pass


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?", type=int, default=18099)
    ap.add_argument("trace", nargs="?", default=None)
    ap.add_argument("--scenario", default="fail-last",
                    choices=("fail-last", "fail-first", "ok", "idle"),
                    help="失败落在第几段 / 全部成功 / 一条都不发"
                         "（四条印字分支各要一个剧本才跑得全）")
    ap.add_argument("--status", type=int, default=0,
                    help="强制所有响应的 HTTP 状态码（0=正常 200）；"
                         "用来验证 401/429/5xx 的映射，否则那段映射没被跑过")
    ap.add_argument("--garbage", action="store_true",
                    help="200 但正文不是 chat/completions 形状（点亮「真·格式问题」分支）")
    a = ap.parse_args()
    srv = HTTPServer(("127.0.0.1", a.port), H)
    srv.trace = a.trace
    srv.scenario = a.scenario
    srv.status = a.status
    srv.garbage = a.garbage
    srv.serve_forever()
