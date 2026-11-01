#ifndef LOOPBACK_H
#define LOOPBACK_H

/*
 * 环回 HTTP 服务端 —— 测试夹具，不是被测代码。
 *
 * 为什么必须真起一个 socket：把 `agent_http_post` mock 掉只能证明"上层调了它"，
 * 证明不了 chunked 解得开、证明不了请求真的按 HTTP/1.1 发出去、也证明不了
 * "对端不在时返回 IO 错而不是空答案"。而这三种恰好都是只在真实链路上暴露的。
 *
 * 两件事它必须做到：
 *
 *   1. 回固定字节。响应由调用方写死（含状态行与头），因为**"线上到底长什么样"
 *      正是要固定的东西**，不该由服务端现场生成 —— 那会变成"用同一套理解去测
 *      自己"。
 *
 *   2. 把收到的**请求体**（不含头）经 pipe 回传给父进程。那是"调用方到底发了
 *      什么"的唯一硬证据：断言"线上字节 == 协议层纯函数算出的那串"是确定性
 *      契约（prefix cache 的键）的落地检查，没有它就只剩"函数是纯的"这一半。
 *
 * 子进程带 alarm(5)：客户端若发来少于 nreq 个连接（说明某处行为与预期不同），
 * 子进程被杀死、lb_stop 返回 false —— **失败而不是挂住**。测试卡死比测试失败
 * 更贵：CI 上它表现为超时，而超时不给行号。
 *
 * 用法要求：包含本头的 .c 必须在**所有 include 之前**定义
 * `_POSIX_C_SOURCE 200809L`（pipe / fork / recv 都在它后面）。
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* 请求缓冲上限。够放一次 chat/completions 的 JSON（提示词另有 REQ_CAP 兜住）。 */
#define LB_REQ_CAP      16384u
/* 等客户端把连接发齐的上限（秒）。只用于让"行为不符预期"变成失败。 */
#define LB_ALARM_SEC    5u

typedef struct LbSrv {
    int      ls;      /* 监听 fd；父进程 spawn 之后即关闭 */
    uint16_t port;    /* 实际绑定的端口（临时端口，并发跑也不撞） */
    int      rp[2];   /* rp[0] 父读 / rp[1] 子写：每个请求 [4B LE 长度][字节] */
    pid_t    pid;
} LbSrv;

/* 头体分界位置（0 = 还没读到完整头）。 */
static inline uint32_t lb_hdr_end(const char *p, uint32_t n) {
    for (uint32_t i = 0; i + 3 < n; i++)
        if (p[i] == '\r' && p[i + 1] == '\n' && p[i + 2] == '\r' && p[i + 3] == '\n')
            return i + 4u;
    return 0u;
}

/* 请求是否读全（Content-Length 语义；客户端不带 body 时 hdr_end 即全长）。 */
static inline bool lb_req_complete(const char *p, uint32_t n, uint32_t *total) {
    uint32_t he = lb_hdr_end(p, n);
    uint32_t cl = 0;

    if (he == 0u) return false;

    for (uint32_t i = 0; i + 15u <= he; i++) {
        if ((p[i] == 'C' || p[i] == 'c') && memcmp(p + i, "Content-Length:", 15u) == 0) {
            uint32_t j = i + 15u;

            while (j < he && (p[j] == ' ' || p[j] == '\t')) j++;
            while (j < he && p[j] >= '0' && p[j] <= '9') {
                cl = cl * 10u + (uint32_t)(p[j] - '0');
                j++;
            }
            break;
        }
    }
    *total = he + cl;
    return n >= *total;
}

/*
 * 起服务端：串行应答 nreq 个连接，每个连接都回 res[0..res_len) 这份固定字节。
 * nreq 是**精确值**而不是上限：多一个少一个都说明客户端行为不符合预期。
 */
static inline bool lb_start(LbSrv *s, const char *res, uint32_t res_len, uint32_t nreq) {
    struct sockaddr_in a;
    socklen_t alen = (socklen_t)sizeof(a);
    int one = 1;

    memset(s, 0, sizeof(*s));
    s->ls = -1;
    s->rp[0] = s->rp[1] = -1;

    if (pipe(s->rp) != 0) return false;

    s->ls = socket(AF_INET, SOCK_STREAM, 0);
    if (s->ls < 0) return false;
    (void)setsockopt(s->ls, SOL_SOCKET, SO_REUSEADDR, &one, (socklen_t)sizeof(one));

    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    /* 127.0.0.1 写常量而非 INADDR_LOOPBACK：_POSIX_C_SOURCE 会裁掉旧 BSD 符号。 */
    a.sin_addr.s_addr = htonl(0x7f000001u);
    a.sin_port = 0;
    if (bind(s->ls, (struct sockaddr *)&a, (socklen_t)sizeof(a)) != 0) return false;
    if (listen(s->ls, 4) != 0) return false;
    if (getsockname(s->ls, (struct sockaddr *)&a, &alen) != 0) return false;
    s->port = ntohs(a.sin_port);

    fflush(NULL);   /* fork 之前把缓冲吐掉：否则子进程会把父进程的输出再打一遍 */

    s->pid = fork();
    if (s->pid < 0) return false;

    if (s->pid == 0) {
        static char req[LB_REQ_CAP];

        (void)signal(SIGPIPE, SIG_IGN);   /* 父进程先关读端时别被信号杀掉 */
        (void)alarm(LB_ALARM_SEC);

        for (uint32_t i = 0; i < nreq; i++) {
            uint32_t n = 0, total = 0, he, blen;
            uint8_t lbf[4];
            int c = accept(s->ls, NULL, NULL);

            if (c < 0) _exit(2);
            while (n < sizeof(req)) {
                ssize_t r = recv(c, req + n, sizeof(req) - n, 0);

                if (r <= 0) break;
                n += (uint32_t)r;
                if (lb_req_complete(req, n, &total)) break;
            }

            he = lb_hdr_end(req, n);
            blen = (he != 0u && total > he) ? total - he : 0u;
            lbf[0] = (uint8_t)(blen & 0xffu);
            lbf[1] = (uint8_t)((blen >> 8) & 0xffu);
            lbf[2] = (uint8_t)((blen >> 16) & 0xffu);
            lbf[3] = (uint8_t)((blen >> 24) & 0xffu);
            if (write(s->rp[1], lbf, 4u) != 4) _exit(3);
            if (blen != 0u && write(s->rp[1], req + he, blen) != (ssize_t)blen) _exit(3);

            (void)send(c, res, res_len, 0);
            (void)close(c);
        }
        (void)close(s->rp[1]);
        (void)close(s->ls);
        _exit(0);
    }

    (void)close(s->rp[1]);   /* 父进程只读 */
    s->rp[1] = -1;
    (void)close(s->ls);      /* 监听由子进程维持 */
    s->ls = -1;
    return true;
}

/*
 * 取下一个请求体，返回字节数（0 = 没取到或放不下）。
 *
 * 放不下时**把管道里的字节读掉再返回 0**，不返回一个截断的短串：
 * 一个"看起来成功"的短串会让上层的逐字节断言以为自己通过了。
 */
static inline uint32_t lb_body(LbSrv *s, char *buf, uint32_t cap) {
    static char junk[256];
    uint8_t lbf[4];
    uint32_t n, got = 0;

    if (s->rp[0] < 0) return 0u;
    if (read(s->rp[0], lbf, 4u) != 4) return 0u;

    n = (uint32_t)lbf[0] | ((uint32_t)lbf[1] << 8) |
        ((uint32_t)lbf[2] << 16) | ((uint32_t)lbf[3] << 24);

    if (n >= cap) {
        uint32_t left = n;

        while (left > 0u) {
            ssize_t k = read(s->rp[0], junk, left < sizeof(junk) ? left : sizeof(junk));
            if (k <= 0) break;
            left -= (uint32_t)k;
        }
        return 0u;
    }
    while (got < n) {
        ssize_t k = read(s->rp[0], buf + got, n - got);

        if (k <= 0) break;
        got += (uint32_t)k;
    }
    if (got != n) return 0u;
    buf[n] = '\0';
    return n;
}

/* 收尾。返回 false 表示子进程没正常退出（连接数不对 / 超时 / 写 pipe 失败）。 */
static inline bool lb_stop(LbSrv *s) {
    int st = 0;

    if (s->rp[0] >= 0) {
        (void)close(s->rp[0]);
        s->rp[0] = -1;
    }
    if (s->pid <= 0) return false;

    if (waitpid(s->pid, &st, 0) != s->pid) return false;
    s->pid = 0;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

#endif
