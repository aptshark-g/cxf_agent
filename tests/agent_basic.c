#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#else
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <spawn.h>
#include <sys/event.h>
#include <crt_externs.h>
#endif

#include "agent_basic.h"

/* Walk directory descriptors, never a resolved string that can be swapped for
 * a symlink between validation and open. Root itself is deployment-controlled. */
static int rooted_open(const char *root, const char *path, uint32_t len, bool dir) {
    char part[512];
    uint32_t pos = 0;
    int fd;
    if (!root || !path || !len || len >= sizeof part || path[0] == '/') return -1;
    for (uint32_t i = 0; i < len; i++)
        if ((unsigned char)path[i] < 32u || path[i] == 127) return -1;
    fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    if (dir && len == 1 && path[0] == '.') return fd;
    while (pos < len) {
        uint32_t end = pos;
        int next;
        while (end < len && path[end] != '/') end++;
        uint32_t n = end - pos;
        if (!n || (n == 1 && path[pos] == '.') ||
            (n == 2 && path[pos] == '.' && path[pos + 1] == '.')) break;
        memcpy(part, path + pos, n); part[n] = 0;
        next = openat(fd, part, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK |
                      ((end < len || dir) ? O_DIRECTORY : 0));
        close(fd);
        if (next < 0) return -1;
        fd = next;
        if (end == len) return fd;
        pos = end + 1;
    }
    close(fd);
    return -1;
}

static uint32_t utf8_end(const char *s, uint32_t n) {
    uint32_t start = n;
    if (!n) return 0;
    while (start && ((unsigned char)s[start - 1] & 0xc0) == 0x80) start--;
    if (start) start--;
    unsigned char c = (unsigned char)s[start];
    uint32_t need = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 :
                    (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 1;
    return n - start < need ? start : n;
}

cxf_result basic_read_exec(const void *ctx, const char *args, uint32_t len,
                           char *out, uint32_t cap, uint32_t *out_len) {
    const BasicFsCfg *cfg = ctx;
    uint32_t plen = 0, off = 0;
    struct stat st;
    char buf[BASIC_READ_BYTES];
    int fd, h;
    ssize_t got;
    *out_len = 0;
    if (!cfg || !args || !len) return CXF_ERR_FORMAT;
    while (plen < len && args[plen] != '\n') plen++;
    if (plen < len) {
        if (plen + 1 == len) return CXF_ERR_FORMAT;
        for (uint32_t i = plen + 1; i < len; i++) {
            unsigned char c = (unsigned char)args[i];
            if (c < '0' || c > '9' || off > (UINT32_MAX - (c - '0')) / 10)
                return CXF_ERR_FORMAT;
            off = off * 10 + c - '0';
        }
    }
    fd = rooted_open(cfg->root, args, plen, false);
    if (fd < 0) return CXF_ERR_IO;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (off_t)off > st.st_size) {
        close(fd); return CXF_ERR_RANGE;
    }
    do { got = pread(fd, buf, sizeof buf, (off_t)off); } while (got < 0 && errno == EINTR);
    close(fd);
    if (got < 0) return CXF_ERR_IO;
    if (memchr(buf, 0, (size_t)got)) return CXF_ERR_FORMAT;
    if (off && got && ((unsigned char)buf[0] & 0xc0) == 0x80) return CXF_ERR_RANGE;
    uint32_t n = utf8_end(buf, (uint32_t)got);
    bool more = (off_t)off + n < st.st_size;
    h = snprintf(out, cap, "#file %.*s offset=%u bytes=%u total=%lld next=%lld\n",
                 (int)plen, args, off, n, (long long)st.st_size,
                 more ? (long long)off + n : -1LL);
    if (h < 0 || (uint32_t)h >= cap || n > cap - (uint32_t)h) return CXF_ERR_FULL;
    memcpy(out + h, buf, n);
    *out_len = (uint32_t)h + n;
    return CXF_OK;
}

typedef struct ListEntry { char name[256]; char kind; } ListEntry;

cxf_result basic_list_exec(const void *ctx, const char *args, uint32_t len,
                           char *out, uint32_t cap, uint32_t *out_len) {
    const BasicFsCfg *cfg = ctx;
    struct dirent *de;
    uint32_t used = 0, count = 0, omitted = 0, w = 0;
    int fd;
    DIR *d;
    ListEntry *entries;
    *out_len = 0;
    if (!cfg || !args || !len) return CXF_ERR_FORMAT;
    fd = rooted_open(cfg->root, args, len, true);
    if (fd < 0) return CXF_ERR_IO;
    d = fdopendir(fd);
    if (!d) { close(fd); return CXF_ERR_IO; }
    entries = calloc(BASIC_LIST_MAX, sizeof *entries);
    if (!entries) { closedir(d); return CXF_ERR_NOMEM; }
    errno = 0;
    while ((de = readdir(d))) {
        struct stat st;
        bool clean = true;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        for (const unsigned char *p = (const unsigned char *)de->d_name; *p; p++)
            if (*p < 32 || *p == 127) clean = false;
        if (!clean || strlen(de->d_name) >= sizeof entries[0].name) { omitted++; continue; }
        if (fstatat(fd, de->d_name, &st, AT_SYMLINK_NOFOLLOW)) {
            free(entries); closedir(d); return CXF_ERR_IO;
        }
        count++;
        uint32_t at = 0;
        while (at < used && strcmp(entries[at].name, de->d_name) < 0) at++;
        if (at < BASIC_LIST_MAX) {
            if (used < BASIC_LIST_MAX) used++;
            memmove(entries + at + 1, entries + at, (used - at - 1) * sizeof *entries);
            strcpy(entries[at].name, de->d_name);
            entries[at].kind = S_ISDIR(st.st_mode) ? 'd' : S_ISREG(st.st_mode) ? 'f' :
                               S_ISLNK(st.st_mode) ? 'l' : '?';
        }
        errno = 0;
    }
    int saved = errno;
    closedir(d);
    if (saved) { free(entries); return CXF_ERR_IO; }
    for (uint32_t i = 0; i < used; i++) {
        uint32_t need = (uint32_t)strlen(entries[i].name) + 3;
        if (cap - w < need + 80) break; /* reserve the completeness receipt */
        out[w++] = entries[i].kind; out[w++] = ' ';
        memcpy(out + w, entries[i].name, need - 3); w += need - 3;
        out[w++] = '\n';
    }
    free(entries);
    uint32_t shown = 0;
    for (uint32_t i = 0; i < w; i++) if (out[i] == '\n') shown++;
    int n = snprintf(out + w, cap - w, "#list shown=%u total=%u omitted=%u%s\n",
                     shown, count + omitted, omitted,
                     shown < count || omitted ? " #truncated" : "");
    if (n < 0 || (uint32_t)n >= cap - w) return CXF_ERR_FULL;
    *out_len = w + (uint32_t)n;
    return CXF_OK;
}

static uint64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static uint64_t trace_ns(void) {
    struct timespec t;
#ifdef __APPLE__
    /* libc++ steady_clock uses the raw monotonic clock on Darwin. */
    clock_gettime(CLOCK_MONOTONIC_RAW, &t);
#else
    clock_gettime(CLOCK_MONOTONIC, &t);
#endif
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
#if defined(__APPLE__) && __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ >= 260000
static int spawn_command(const TestRunCfg *cfg,const int pipefd[2],pid_t *pid) {
    posix_spawn_file_actions_t actions;posix_spawnattr_t attr;
    int rc=posix_spawn_file_actions_init(&actions);if(rc)return rc;
    rc=posix_spawnattr_init(&attr);if(rc){posix_spawn_file_actions_destroy(&actions);return rc;}
#define SPAWN_ACTION(call) do{if(!rc)rc=(call);}while(0)
    SPAWN_ACTION(posix_spawn_file_actions_addclose(&actions,pipefd[0]));
    SPAWN_ACTION(posix_spawn_file_actions_addopen(&actions,STDIN_FILENO,"/dev/null",O_RDONLY,0));
    SPAWN_ACTION(posix_spawn_file_actions_adddup2(&actions,pipefd[1],STDOUT_FILENO));
    SPAWN_ACTION(posix_spawn_file_actions_adddup2(&actions,pipefd[1],STDERR_FILENO));
    if(pipefd[1]>STDERR_FILENO)SPAWN_ACTION(posix_spawn_file_actions_addclose(&actions,pipefd[1]));
    SPAWN_ACTION(posix_spawn_file_actions_addchdir(&actions,cfg->root));
    SPAWN_ACTION(posix_spawnattr_setflags(&attr,POSIX_SPAWN_SETPGROUP));
    SPAWN_ACTION(posix_spawnattr_setpgroup(&attr,0));
    char *argv[]={"sh","-c",(char *)cfg->command,NULL};
    if(!rc)rc=posix_spawn(pid,"/bin/sh",&actions,&attr,argv,*_NSGetEnviron());
    posix_spawnattr_destroy(&attr);posix_spawn_file_actions_destroy(&actions);
#undef SPAWN_ACTION
    return rc;
}
#endif
static cxf_result test_run_exec_full(const void *ctx,const char *args,uint32_t len,
                         char *out,uint32_t cap,uint32_t *out_len,TestRunTrace *trace,
                         TestRunBackend backend,TestRunProgressFn progress,void *progress_ctx,
                         TestRunOutputFn observe,void *observe_ctx,bool tail,int event_reap) {
    const TestRunCfg *cfg = ctx;
    int pipefd[2], status = 0, exit_queue = -1;
    bool event_attempted = false, exit_race = false;
    pid_t pid=-1;
    char buf[4096], output[4096];
    uint32_t used = 0;
    uint64_t total = 0;
    bool timed_out = false, eof = false, finished = false, io_error = false;
    (void)args;
    *out_len = 0;
    TestRunTrace local_trace;if(!trace&&progress)trace=&local_trace;
    if (trace) { memset(trace, 0, sizeof *trace); trace->entered_ns = trace_ns(); }
    if(backend<TEST_RUN_AUTO||backend>TEST_RUN_SPAWN)return CXF_ERR_RANGE;
#if defined(__APPLE__) && __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ >= 260000
    if(backend==TEST_RUN_AUTO)backend=TEST_RUN_SPAWN;
#else
    if(backend==TEST_RUN_SPAWN)return CXF_ERR_DEP;
    if(backend==TEST_RUN_AUTO)backend=TEST_RUN_FORK;
#endif
    if(trace)trace->backend=backend;
    if (len || !cfg || !cfg->root || !cfg->command || !cfg->command[0]) return CXF_ERR_FORMAT;
    /* A command may have side effects. Refuse undersized buffers before fork;
     * never return FULL after running it (the runtime would execute it again). */
    if (cap < BASIC_OUT_HINT) return CXF_ERR_RANGE;
    if (pipe(pipefd)) return CXF_ERR_IO;
    (void)fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
    if (trace) trace->spawn_begin_ns = trace_ns();
    int launch_error=0;
#if defined(__APPLE__) && __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ >= 260000
    if(backend==TEST_RUN_SPAWN){launch_error=spawn_command(cfg,pipefd,&pid);if(launch_error)pid=-1;}
    else
#endif
    pid = fork();
    if (pid != 0 && trace) trace->spawn_end_ns = trace_ns();
    if (pid < 0 && !launch_error) { close(pipefd[0]); close(pipefd[1]); return CXF_ERR_IO; }
    if (!pid) {
        (void)setpgid(0, 0);
        close(pipefd[0]);
        int input = open("/dev/null", O_RDONLY);
        if (input < 0 || dup2(input, STDIN_FILENO) < 0 ||
            dup2(pipefd[1], STDOUT_FILENO) < 0 || dup2(pipefd[1], STDERR_FILENO) < 0) _exit(126);
        if (input > STDERR_FILENO) close(input);
        if (pipefd[1] > STDERR_FILENO) close(pipefd[1]);
        if (chdir(cfg->root)) { perror("test.run cwd"); _exit(126); }
        execl("/bin/sh", "sh", "-c", cfg->command, (char *)NULL);
        _exit(127);
    }
    if(launch_error){
        status=126<<8;finished=true;
        int size=snprintf(output,sizeof output,"test.run launch: %s\n",strerror(launch_error));
        used=(uint32_t)size;total=used;
        if(trace){trace->launch_error=launch_error;trace->reaped_ns=trace->spawn_end_ns;}
    }else{
        (void)setpgid(pid,pid);
        if(progress)progress(progress_ctx,trace);
    }
    close(pipefd[1]);
    (void)fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    uint64_t deadline = now_ms() + (cfg->timeout_ms ? cfg->timeout_ms : 30000u);
    while (!finished || !eof) {
        ssize_t n;
        /* Drain incrementally; cap each pass so an infinite writer cannot
         * starve the timeout check. Keep only a bounded UTF-8-safe prefix. */
        for (unsigned reads = 0; reads < 16 && !eof; reads++) {
            n = read(pipefd[0], buf, sizeof buf);
            if (n > 0) {
                if (trace && !trace->first_output_ns) trace->first_output_ns = trace_ns();
                uint32_t keep = (uint32_t)n < sizeof output - used ? (uint32_t)n : (uint32_t)sizeof output - used;
                memcpy(output + used, buf, keep); used += keep; total += (uint64_t)n;
                if(observe&&keep){char text[4096];uint32_t size=utf8_end(output,used);memcpy(text,output,size);for(uint32_t i=0;i<size;i++)if(!text[i])text[i]='?';uint64_t cb=trace_ns();observe(observe_ctx,text,size,false);if(trace)trace->callback_ns+=trace_ns()-cb;}
            } else if (!n) { eof = true;if(trace)trace->eof_ns=trace_ns(); break; }
            else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                io_error = true; eof = true; break;
            } else break;
        }
        if (!finished) {
            pid_t result = waitpid(pid, &status, WNOHANG);
            if (result == pid) { finished = true; if (trace) trace->reaped_ns = trace_ns(); }
            else if (result < 0 && errno != EINTR) { io_error = true; break; }
        }
        if (finished && eof) break;
        if (now_ms() >= deadline) { timed_out = true; break; }
        struct pollfd p = { pipefd[0], POLLIN | POLLHUP, 0 };
        if (eof) {
            uint64_t t=trace_ns();bool event_wait=false;
#ifdef __APPLE__
            if(event_reap&&!event_attempted){
                event_attempted=true;exit_queue=kqueue();
                if(exit_queue>=0){
                    struct kevent change;EV_SET(&change,(uintptr_t)pid,EVFILT_PROC,EV_ADD|EV_ONESHOT,NOTE_EXIT,0,NULL);
                    if(fcntl(exit_queue,F_SETFD,FD_CLOEXEC)<0||kevent(exit_queue,&change,1,NULL,0,NULL)<0){exit_race=event_reap==2&&errno==ESRCH;close(exit_queue);exit_queue=-1;}
                    /* Child may have exited during registration; retry waitpid
                     * before any fallback sleep. */
                    continue;
                }
            }
            if(exit_queue>=0&&!exit_race){struct kevent event;struct timespec limit={0,10000000};int got=kevent(exit_queue,NULL,0,&event,1,&limit);event_wait=got>=0;if(got>0&&event_reap==2&&(event.fflags&NOTE_EXIT))exit_race=true;if(got<0){close(exit_queue);exit_queue=-1;}}
#else
            (void)event_reap;(void)event_attempted;
#endif
            if(!event_wait)(void)poll(NULL,0,exit_race?1:10);
            if(trace){trace->eof_polls++;trace->eof_poll_ns+=trace_ns()-t;}
        }
        else (void)poll(&p, 1, 10);
    }
    if (pid>0 && (timed_out || io_error)) {
        (void)kill(-pid, SIGKILL);
        if (!finished) (void)kill(pid, SIGKILL);
    }
    if (!finished) while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (trace && !trace->reaped_ns) trace->reaped_ns = trace_ns();
    if(exit_queue>=0)close(exit_queue);
    close(pipefd[0]);
    used = utf8_end(output, used);
    /* Commands emit text. Replace embedded NULs rather than prematurely ending
     * the tool message; the receipt still reports the actual byte count. */
    for (uint32_t i = 0; i < used; i++) if (!output[i]) output[i] = '?';
    if(trace){trace->total_output_bytes=total;trace->retained_output_bytes=used;}
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    int sig = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
    if(observe)observe(observe_ctx,output,used,true);
    uint32_t offset=0;if(tail){memcpy(out,output,used);out[used]='\n';offset=used+1;}
    int h = snprintf(out+offset, cap-offset, "#test exit=%d signal=%d timeout=%u io_error=%u bytes=%llu%s\n",
                     code, sig, timed_out, io_error, (unsigned long long)total,
                     total > used ? " #truncated" : "");
    if(!tail)memcpy(out + h, output, used);
    *out_len = tail?offset+(uint32_t)h:(uint32_t)h+used;
    if (trace) trace->finished_ns = trace_ns();
    return CXF_OK;
}
cxf_result test_run_exec_observed(const void *ctx,const char *args,uint32_t len,char *out,uint32_t cap,uint32_t *n,TestRunTrace *trace,TestRunBackend backend,TestRunProgressFn progress,void *opaque){return test_run_exec_full(ctx,args,len,out,cap,n,trace,backend,progress,opaque,NULL,NULL,false,false);}
cxf_result test_run_exec_output(const void *ctx,const char *args,uint32_t len,char *out,uint32_t cap,uint32_t *n,TestRunTrace *trace,TestRunOutputFn observe,void *opaque){return test_run_exec_full(ctx,args,len,out,cap,n,trace,TEST_RUN_AUTO,NULL,NULL,observe,opaque,true,false);}
cxf_result test_run_exec_output_event(const void *ctx,const char *args,uint32_t len,char *out,uint32_t cap,uint32_t *n,TestRunTrace *trace,TestRunOutputFn observe,void *opaque){return test_run_exec_full(ctx,args,len,out,cap,n,trace,TEST_RUN_AUTO,NULL,NULL,observe,opaque,true,true);}
cxf_result test_run_exec_output_race(const void *ctx,const char *args,uint32_t len,char *out,uint32_t cap,uint32_t *n,TestRunTrace *trace,TestRunOutputFn observe,void *opaque){return test_run_exec_full(ctx,args,len,out,cap,n,trace,TEST_RUN_AUTO,NULL,NULL,observe,opaque,true,2);}
cxf_result test_run_exec(const void *ctx, const char *args, uint32_t len,
                         char *out, uint32_t cap, uint32_t *out_len) {
    return test_run_exec_observed(ctx,args,len,out,cap,out_len,NULL,TEST_RUN_AUTO,NULL,NULL);
}
cxf_result test_run_exec_traced(const void *ctx,const char *args,uint32_t len,
                         char *out,uint32_t cap,uint32_t *out_len,TestRunTrace *trace){
    return test_run_exec_observed(ctx,args,len,out,cap,out_len,trace,TEST_RUN_AUTO,NULL,NULL);
}

const IfaceDef basic_iface_view = { "fs.view", "workspace text or directory listing", IFACE_INT };
const IfaceDef basic_iface_test = { "test.result", "test exit status and captured output", IFACE_INT };
const CapDef basic_capdef_read = {
    "fs.read", "Read a workspace text file; paginate large files using next byte offset",
    "relative path, optionally followed by newline and decimal byte offset (default 0)",
    "#file receipt then original text; next=-1 means complete; otherwise read again at next",
    { IFACENAME_TASK, NULL }, "fs.view"
};
const CapDef basic_capdef_list = {
    "fs.list", "List a workspace directory in name order; use . for the root",
    "relative directory path", "f file / d directory / l symlink; #list receipt reports truncation",
    { IFACENAME_TASK, NULL }, "fs.view"
};
const CapDef basic_capdef_test = {
    "test.run", "Run the deployment-configured test command in the workspace",
    "empty string; command and working directory cannot be changed by the model",
    "#test exit/signal/timeout/io_error receipt plus combined stdout/stderr; exit=0 alone is not enough if timeout or io_error is set",
    { IFACENAME_TASK, NULL }, "test.result"
};
const CapImpl basic_impl_read = { "fs.read", "fs", CAP_TIER_SCRIPT, 1, CAP_TRUST_IO,
    basic_read_exec, NULL, BASIC_OUT_HINT, NULL };
const CapImpl basic_impl_list = { "fs.list", "fs", CAP_TIER_SCRIPT, 1, CAP_TRUST_IO,
    basic_list_exec, NULL, BASIC_OUT_HINT, NULL };
const CapImpl basic_impl_test = { "test.run", "process", CAP_TIER_SCRIPT, 1, CAP_TRUST_WRITE,
    test_run_exec, NULL, BASIC_OUT_HINT, NULL };
