#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "agent_http.h"

/* SIGPIPE：对端先关闭时 write 会杀掉整个进程，而"对端先关闭"在网络上
 * 是正常事件不是错误。忽略它，让 write 返回 EPIPE，由调用方按 IO 错处理。 */
static void http_init_once(void) {
    static bool done = false;

    if (done) return;
    (void)signal(SIGPIPE, SIG_IGN);
    done = true;
}

static void set_timeout(int fd, uint32_t ms) {
    struct timeval tv;

    if (ms == 0) ms = AGENT_HTTP_TIMEOUT_MS;
    tv.tv_sec = (time_t)(ms / 1000u);
    tv.tv_usec = (suseconds_t)((ms % 1000u) * 1000u);
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, (socklen_t)sizeof(tv));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, (socklen_t)sizeof(tv));
}

/*
 * 连接。故意只做阻塞 connect：本地环回是即时的，而带超时的 connect 要
 * 非阻塞 + poll，多出二十行只为覆盖"公网连不上"—— 那不在当前部署里。
 * 真要覆盖时，改的是这个函数，上面两层不受影响。
 */
static int http_connect(const char *host, uint16_t port, uint32_t timeout_ms) {
    struct addrinfo hints, *res = NULL, *it;
    char portstr[8];
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    (void)snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0) return -1;

    for (it = res; it != NULL; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) continue;
        set_timeout(fd, timeout_ms);
        if (connect(fd, it->ai_addr, it->ai_addrlen) == 0) break;
        (void)close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static bool write_all(int fd, const char *p, uint32_t n) {
    uint32_t off = 0;

    while (off < n) {
        ssize_t w = send(fd, p + off, (size_t)(n - off), 0);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            return false;
        }
        off += (uint32_t)w;
    }
    return true;
}

/* 读到底（对端 close）或缓冲满。返回读到的字节数。 */
static uint32_t read_until_eof(int fd, char *buf, uint32_t cap) {
    uint32_t off = 0;

    while (off < cap) {
        ssize_t r = recv(fd, buf + off, (size_t)(cap - off), 0);
        if (r == 0) break;                       /* 对端关闭：正常结束 */
        if (r < 0) {
            if (errno == EINTR) continue;
            break;                               /* 超时 / 出错：把已有的当结果 */
        }
        off += (uint32_t)r;
    }
    return off;
}

static bool hdr_has(const char *hdr, uint32_t hlen, const char *needle) {
    uint32_t nlen = (uint32_t)strlen(needle);

    if (nlen == 0 || nlen > hlen) return false;
    for (uint32_t i = 0; i + nlen <= hlen; i++) {
        uint32_t k = 0;
        while (k < nlen) {
            char a = hdr[i + k], b = needle[k];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
            k++;
        }
        if (k == nlen) return true;
    }
    return false;
}

/*
 * chunked 就地解码。
 *
 * 为什么必须做：Go 的 net/http（relay-gateway 的语言）在响应体没被完整缓冲时
 * 会用 chunked。只在 mock 上测过、上线才发现响应"看起来是乱码"，
 * 这个坑的成本远高于这 40 行。
 *
 * 就地：输出永远不慢于输入，所以写游标追不上读游标，memmove 安全。
 */
static bool dechunk(char *p, uint32_t len, uint32_t *out_len) {
    uint32_t in = 0, out = 0;

    for (;;) {
        uint32_t sz = 0, digits = 0;

        while (in < len) {
            char d = p[in];
            uint32_t v;
            if (d >= '0' && d <= '9')      v = (uint32_t)(d - '0');
            else if (d >= 'a' && d <= 'f') v = (uint32_t)(d - 'a' + 10);
            else if (d >= 'A' && d <= 'F') v = (uint32_t)(d - 'A' + 10);
            else break;
            sz = sz * 16u + v;
            digits++;
            in++;
        }
        if (digits == 0) return false;

        while (in + 1 < len && !(p[in] == '\r' && p[in + 1] == '\n')) in++;
        if (in + 1 >= len) return false;
        in += 2;

        if (sz == 0) { *out_len = out; return true; }
        if (sz > len - in) return false;
        if (out != in) memmove(p + out, p + in, sz);
        out += sz;
        in += sz;
        if (in + 1 < len && p[in] == '\r' && p[in + 1] == '\n') in += 2;
    }
}

cxf_result agent_http_post(const AgentHttpReq *req, char *buf, uint32_t cap,
                           AgentHttpResp *resp) {
    char head[AGENT_HTTP_HDR_CAP];
    uint32_t hlen, raw, hdr_end = 0, body_off = 0;
    int fd, n;
    int status = 0;
    bool chunked = false;

    if (req == NULL || buf == NULL || cap == 0 || resp == NULL) return CXF_ERR_RANGE;
    if (req->host == NULL || req->path == NULL) return CXF_ERR_RANGE;
    if (req->body_len > 0 && req->body == NULL) return CXF_ERR_RANGE;

    http_init_once();

    n = snprintf(head, sizeof(head),
                 "POST %s HTTP/1.1\r\n"
                 "Host: %s:%u\r\n"
                 "Content-Type: application/json\r\n"
                 "Content-Length: %u\r\n"
                 "Accept: application/json\r\n"
                 "Connection: close\r\n"
                 "%s%s%s"
                 "\r\n",
                 req->path, req->host, (unsigned)req->port, (unsigned)req->body_len,
                 req->auth != NULL ? "Authorization: " : "",
                 req->auth != NULL ? req->auth : "",
                 req->auth != NULL ? "\r\n" : "");
    if (n < 0 || (uint32_t)n >= sizeof(head)) return CXF_ERR_RANGE;
    hlen = (uint32_t)n;
    if (req->extra_hdr != NULL) {
        uint32_t e = (uint32_t)strlen(req->extra_hdr);
        /* extra_hdr 要插在空行之前 —— 这里直接追加在头部末尾（空行还没写） */
        if (hlen + e >= sizeof(head)) return CXF_ERR_RANGE;
        (void)memcpy(head + hlen - 2u, req->extra_hdr, e);   /* -2 覆盖已有 CRLF */
        (void)memcpy(head + hlen - 2u + e, "\r\n", 2u);
        hlen = hlen - 2u + e + 2u;
    }

    fd = http_connect(req->host, req->port, req->timeout_ms);
    if (fd < 0) return CXF_ERR_IO;

    if (!write_all(fd, head, hlen) ||
        (req->body_len > 0 && !write_all(fd, req->body, req->body_len))) {
        (void)close(fd);
        return CXF_ERR_IO;
    }

    raw = read_until_eof(fd, buf, cap);
    (void)close(fd);

    /* 头体分界。找不到说明连报文都不完整 —— 不猜，直接失败。 */
    for (uint32_t i = 0; i + 3 < raw; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            hdr_end = i + 4u;
            break;
        }
    }
    if (hdr_end == 0) return CXF_ERR_IO;

    /* "HTTP/1.1 200 OK" —— 状态码在第一个空格之后 */
    {
        uint32_t i = 0;
        while (i < hdr_end && buf[i] != ' ') i++;
        while (i < hdr_end && buf[i] == ' ') i++;
        for (uint32_t k = 0; k < 3u && i < hdr_end; k++, i++) {
            if (buf[i] < '0' || buf[i] > '9') break;
            status = status * 10 + (buf[i] - '0');
        }
    }
    if (status == 0) return CXF_ERR_IO;

    chunked = hdr_has(buf, hdr_end, "transfer-encoding: chunked");

    body_off = hdr_end;
    if (chunked) {
        if (!dechunk(buf + body_off, raw - body_off, &resp->body_len)) return CXF_ERR_IO;
        resp->body = buf + body_off;
    } else {
        resp->body = buf + body_off;
        resp->body_len = raw - body_off;
    }
    resp->status = status;
    /* cap 恰好装满 ⇒ 无法确认响应已结束。保守报警，见头文件。 */
    resp->truncated = (raw == cap);
    return CXF_OK;
}
