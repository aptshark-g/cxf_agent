#include "agentctl_config.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define CFG_FILE_MAX 16384u

typedef struct Json {
    const char *p;
    const char *begin;
    const char *end;
    char *err;
    uint32_t err_cap;
} Json;

static bool jfail(Json *j, const char *fmt, ...) {
    va_list ap;
    size_t off = (size_t)(j->p - j->begin);
    int n = snprintf(j->err, j->err_cap, "JSON 字节 %lu：", (unsigned long)off);

    if (n < 0 || (uint32_t)n >= j->err_cap) return false;
    va_start(ap, fmt);
    (void)vsnprintf(j->err + n, j->err_cap - (uint32_t)n, fmt, ap);
    va_end(ap);
    return false;
}

static void jws(Json *j) {
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' ||
           *j->p == '\r' || *j->p == '\n')) j->p++;
}

static bool jtake(Json *j, char c) {
    jws(j);
    if (j->p >= j->end || *j->p != c) return jfail(j, "需要 '%c'", c);
    j->p++;
    return true;
}

static int jhex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool ju4(Json *j, uint32_t *v) {
    uint32_t n = 0;
    if ((size_t)(j->end - j->p) < 4u) return jfail(j, "\\u 缺少四位十六进制");
    for (uint32_t i = 0; i < 4u; i++) {
        int h = jhex(j->p[i]);
        if (h < 0) return jfail(j, "非法 Unicode 转义");
        n = (n << 4) | (uint32_t)h;
    }
    j->p += 4;
    *v = n;
    return true;
}

static bool jput(Json *j, char *out, uint32_t cap, uint32_t *w, uint32_t cp) {
    unsigned char b[4];
    uint32_t n;

    if (cp <= 0x7fu) { b[0] = (unsigned char)cp; n = 1u; }
    else if (cp <= 0x7ffu) {
        b[0] = 0xc0u | (unsigned char)(cp >> 6);
        b[1] = 0x80u | (unsigned char)(cp & 0x3fu); n = 2u;
    } else if (cp <= 0xffffu) {
        b[0] = 0xe0u | (unsigned char)(cp >> 12);
        b[1] = 0x80u | (unsigned char)((cp >> 6) & 0x3fu);
        b[2] = 0x80u | (unsigned char)(cp & 0x3fu); n = 3u;
    } else {
        b[0] = 0xf0u | (unsigned char)(cp >> 18);
        b[1] = 0x80u | (unsigned char)((cp >> 12) & 0x3fu);
        b[2] = 0x80u | (unsigned char)((cp >> 6) & 0x3fu);
        b[3] = 0x80u | (unsigned char)(cp & 0x3fu); n = 4u;
    }
    if (*w > cap || n >= cap - *w) return jfail(j, "字符串超过字段容量");
    for (uint32_t i = 0; i < n; i++) out[(*w)++] = (char)b[i];
    return true;
}

static bool jstring(Json *j, char *out, uint32_t cap) {
    uint32_t w = 0;

    jws(j);
    if (j->p >= j->end || *j->p != '"') return jfail(j, "需要字符串");
    j->p++;
    while (j->p < j->end) {
        unsigned char c = (unsigned char)*j->p++;
        if (c == '"') {
            if (w >= cap) return jfail(j, "字符串超过字段容量");
            out[w] = '\0';
            return true;
        }
        if (c < 0x20u) return jfail(j, "字符串含未转义控制字符");
        if (c == '\\') {
            uint32_t cp;
            if (j->p >= j->end) return jfail(j, "转义不完整");
            c = (unsigned char)*j->p++;
            switch (c) {
            case '"': case '\\': case '/': cp = c; break;
            case 'b': cp = '\b'; break;
            case 'f': cp = '\f'; break;
            case 'n': cp = '\n'; break;
            case 'r': cp = '\r'; break;
            case 't': cp = '\t'; break;
            case 'u':
                if (!ju4(j, &cp)) return false;
                if (cp >= 0xd800u && cp <= 0xdbffu) {
                    uint32_t lo;
                    if ((size_t)(j->end - j->p) < 6u || j->p[0] != '\\' || j->p[1] != 'u')
                        return jfail(j, "高代理缺少低代理");
                    j->p += 2;
                    if (!ju4(j, &lo)) return false;
                    if (lo < 0xdc00u || lo > 0xdfffu) return jfail(j, "非法低代理");
                    cp = 0x10000u + ((cp - 0xd800u) << 10) + (lo - 0xdc00u);
                } else if (cp >= 0xdc00u && cp <= 0xdfffu) {
                    return jfail(j, "孤立低代理");
                }
                break;
            default: return jfail(j, "未知转义");
            }
            if (!jput(j, out, cap, &w, cp)) return false;
        } else {
            if (w + 1u >= cap) return jfail(j, "字符串超过字段容量");
            out[w++] = (char)c;
        }
    }
    return jfail(j, "字符串未闭合");
}

static bool ju32(Json *j, uint32_t *out) {
    uint64_t v = 0;
    jws(j);
    if (j->p >= j->end || *j->p < '0' || *j->p > '9') return jfail(j, "需要非负整数");
    if (*j->p == '0' && j->p + 1 < j->end && j->p[1] >= '0' && j->p[1] <= '9')
        return jfail(j, "整数不能有前导零");
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') {
        v = v * 10u + (uint32_t)(*j->p - '0');
        if (v > UINT32_MAX) return jfail(j, "整数超过 uint32");
        j->p++;
    }
    *out = (uint32_t)v;
    return true;
}

static bool jfield(Json *j, uint32_t *present, uint32_t bit) {
    if ((*present & bit) != 0u) return jfail(j, "字段重复");
    *present |= bit;
    return true;
}

static bool jmore_or_end(Json *j, bool *more) {
    jws(j);
    if (j->p < j->end && *j->p == ',') { j->p++; *more = true; return true; }
    if (j->p < j->end && *j->p == '}') { j->p++; *more = false; return true; }
    return jfail(j, "对象字段后需要 ',' 或 '}'");
}

static bool japi(Json *j, CtlFileConfig *out) {
    bool more = false;
    if (!jtake(j, '{')) return false;
    jws(j);
    if (j->p < j->end && *j->p == '}') { j->p++; return true; }
    do {
        char key[32];
        if (!jstring(j, key, sizeof(key)) || !jtake(j, ':')) return false;
        if (!strcmp(key, "url")) {
            if (!jfield(j, &out->present, CTL_CFG_API_URL) ||
                !jstring(j, out->url, sizeof(out->url))) return false;
        } else if (!strcmp(key, "model")) {
            if (!jfield(j, &out->present, CTL_CFG_API_MODEL) ||
                !jstring(j, out->model, sizeof(out->model))) return false;
        } else if (!strcmp(key, "key")) {
            if (!jfield(j, &out->present, CTL_CFG_API_KEY) ||
                !jstring(j, out->key, sizeof(out->key))) return false;
        } else if (!strcmp(key, "timeout_ms")) {
            if (!jfield(j, &out->present, CTL_CFG_API_TIMEOUT) ||
                !ju32(j, &out->timeout_ms)) return false;
        } else return jfail(j, "未知 api 字段 '%s'", key);
        if (!jmore_or_end(j, &more)) return false;
    } while (more);
    return true;
}

static bool jagent(Json *j, CtlFileConfig *out) {
    bool more = false;
    if (!jtake(j, '{')) return false;
    jws(j);
    if (j->p < j->end && *j->p == '}') { j->p++; return true; }
    do {
        char key[32];
        uint32_t bit, *dst;
        if (!jstring(j, key, sizeof(key)) || !jtake(j, ':')) return false;
        if (!strcmp(key, "budget")) { bit = CTL_CFG_BUDGET; dst = &out->budget; }
        else if (!strcmp(key, "rounds")) { bit = CTL_CFG_ROUNDS; dst = &out->rounds; }
        else if (!strcmp(key, "conc")) { bit = CTL_CFG_CONC; dst = &out->conc; }
        else if (!strcmp(key, "rpm")) { bit = CTL_CFG_RPM; dst = &out->rpm; }
        else return jfail(j, "未知 agent 字段 '%s'", key);
        if (!jfield(j, &out->present, bit) || !ju32(j, dst)) return false;
        if (!jmore_or_end(j, &more)) return false;
    } while (more);
    return true;
}

static bool jroot(Json *j, CtlFileConfig *out) {
    bool more = false;
    if (!jtake(j, '{')) return false;
    jws(j);
    if (j->p < j->end && *j->p == '}') { j->p++; return true; }
    do {
        char key[16];
        if (!jstring(j, key, sizeof(key)) || !jtake(j, ':')) return false;
        if (!strcmp(key, "api")) {
            if (!japi(j, out)) return false;
        } else if (!strcmp(key, "agent")) {
            if (!jagent(j, out)) return false;
        } else return jfail(j, "未知顶层字段 '%s'", key);
        if (!jmore_or_end(j, &more)) return false;
    } while (more);
    jws(j);
    if (j->p != j->end) return jfail(j, "根对象后还有多余内容");
    return true;
}

CtlConfigReadResult ctl_config_read(const char *path, CtlFileConfig *out,
                                    char *err, uint32_t err_cap) {
    char buf[CFG_FILE_MAX + 1u];
    FILE *f;
    size_t n;
    Json j;

    memset(out, 0, sizeof(*out));
    if (err_cap > 0u) err[0] = '\0';
    f = fopen(path, "rb");
    if (f == NULL) {
        if (errno == ENOENT) return CTL_CONFIG_NOT_FOUND;
        (void)snprintf(err, err_cap, "打不开配置文件：%s", strerror(errno));
        return CTL_CONFIG_ERROR;
    }
    n = fread(buf, 1u, sizeof(buf), f);
    if (ferror(f)) {
        (void)snprintf(err, err_cap, "读取配置文件失败");
        fclose(f);
        return CTL_CONFIG_ERROR;
    }
    fclose(f);
    if (n > CFG_FILE_MAX) {
        (void)snprintf(err, err_cap, "配置文件超过 %u B", CFG_FILE_MAX);
        return CTL_CONFIG_ERROR;
    }
    j = (Json){ buf, buf, buf + n, err, err_cap };
    if (!jroot(&j, out)) return CTL_CONFIG_ERROR;
    return CTL_CONFIG_OK;
}
