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
#include <stdatomic.h>
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
static int rooted_open(const AgentPolicy *policy,const char *root,const char *path,uint32_t n,bool dir) {
    return agent_policy_open(policy,root,path,n,dir,O_RDONLY);
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

static bool read_version_same(const struct stat *a,const struct stat *b) {
#if defined(__APPLE__)
#define READ_MTIME st_mtimespec
#define READ_CTIME st_ctimespec
#else
#define READ_MTIME st_mtim
#define READ_CTIME st_ctim
#endif
    return a->st_dev==b->st_dev && a->st_ino==b->st_ino && a->st_size==b->st_size &&
        a->st_mode==b->st_mode && a->st_uid==b->st_uid && a->st_gid==b->st_gid && a->st_nlink==b->st_nlink &&
        a->READ_MTIME.tv_sec==b->READ_MTIME.tv_sec && a->READ_MTIME.tv_nsec==b->READ_MTIME.tv_nsec &&
        a->READ_CTIME.tv_sec==b->READ_CTIME.tv_sec && a->READ_CTIME.tv_nsec==b->READ_CTIME.tv_nsec;
#undef READ_MTIME
#undef READ_CTIME
}

cxf_result basic_read_versioned(const BasicFsCfg *cfg,const char *args,uint32_t len,
                               char *out,uint32_t cap,uint32_t *out_len,BasicReadVersion *version) {
    uint32_t plen = 0, off = 0;
    struct stat st;
    char buf[BASIC_READ_BYTES];
    int fd, h;
    ssize_t got;
    if (version) { memset(version,0,sizeof *version); }
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
    fd = rooted_open(cfg->policy, cfg->root, args, plen, false);
    if (fd < 0) return CXF_ERR_IO;
    if (fstat(fd, &st) || !agent_policy_stat(cfg->policy,&st) || !S_ISREG(st.st_mode) || (off_t)off > st.st_size) {
        close(fd); return CXF_ERR_RANGE;
    }
    do { got = pread(fd, buf, sizeof buf, (off_t)off); } while (got < 0 && errno == EINTR);
    if (version) {
        struct stat after;
        if (fstat(fd,&after) || !read_version_same(&st,&after)) {
            close(fd); return CXF_ERR_IO;
        }
        version->file=after;
    }
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
    if (version) version->valid=true;
    return CXF_OK;
}

cxf_result basic_read_exec(const void *ctx,const char *args,uint32_t len,
                          char *out,uint32_t cap,uint32_t *out_len) {
    return basic_read_versioned(ctx,args,len,out,cap,out_len,NULL);
}

bool basic_read_validate(const BasicFsCfg *cfg,const char *args,uint32_t len,const BasicReadVersion *version) {
    if (!cfg || !args || !len || !version || !version->valid) return false;
    uint32_t plen=0;
    while (plen<len && args[plen]!='\n') plen++;
    int fd=rooted_open(cfg->policy,cfg->root,args,plen,false);
    if (fd<0) return false;
    struct stat st;
    bool same=!fstat(fd,&st) && agent_policy_stat(cfg->policy,&st) && read_version_same(&version->file,&st);
    close(fd);
    return same;
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
    fd = rooted_open(cfg->policy, cfg->root, args, len, true);
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
        char child[512];
        int size = len == 1 && args[0] == '.' ? snprintf(child,sizeof child,"%s",de->d_name)
            : snprintf(child,sizeof child,"%.*s/%s",(int)len,args,de->d_name);
        if (size < 0 || size >= (int)sizeof child ||
            !agent_policy_path(cfg->policy,cfg->root,child,(uint32_t)size,S_ISDIR(st.st_mode)) ||
            !agent_policy_stat(cfg->policy,&st)) { errno = 0; continue; }
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
static char *const *test_env(const TestRunCfg *cfg) {
    static char *const minimal[] = {"PATH=/usr/bin:/bin:/usr/sbin:/sbin",
        "LANG=C", "LC_ALL=C", "TMPDIR=/tmp", NULL};
    if (cfg->env_mode == 1) return minimal;
#ifdef __APPLE__
    return *_NSGetEnviron();
#else
    extern char **environ;
    return environ;
#endif
}
#if defined(__APPLE__) && __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ >= 260000
static int spawn_command(const TestRunCfg *cfg,const AgentSandboxJob *job,const int pipefd[2],pid_t *pid) {
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
    SPAWN_ACTION(posix_spawnattr_setflags(&attr,POSIX_SPAWN_SETPGROUP|POSIX_SPAWN_CLOEXEC_DEFAULT));
    SPAWN_ACTION(posix_spawnattr_setpgroup(&attr,0));
    if(!rc)rc=posix_spawn(pid,job->executable,&actions,&attr,job->argv,job->env);
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
    if (cfg->env_mode > 1) return CXF_ERR_RANGE;
    char *const *envp = test_env(cfg); /* resolved before fork; child only execs */
    /* A command may have side effects. Refuse undersized buffers before fork;
     * never return FULL after running it (the runtime would execute it again). */
    if (cap < BASIC_OUT_HINT) return CXF_ERR_RANGE;
    AgentSandboxJob job;uint64_t setup=trace_ns();
    cxf_result sandbox_rc=agent_sandbox_prepare(&job,cfg->sandbox,cfg->root,cfg->command,envp);
    if(trace){trace->sandbox_prepare_ns=trace_ns()-setup;trace->sandbox_entries=job.entries;trace->sandbox_profile_bytes=job.used;}
    if(sandbox_rc!=CXF_OK)return sandbox_rc;
    if (pipe(pipefd)) { agent_sandbox_cleanup(&job);return CXF_ERR_IO; }
    (void)fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
    if (trace) trace->spawn_begin_ns = trace_ns();
    int launch_error=0;
    long close_limit=sysconf(_SC_OPEN_MAX);
    if(close_limit<3){close(pipefd[0]);close(pipefd[1]);agent_sandbox_cleanup(&job);return CXF_ERR_RANGE;}
#if defined(__APPLE__) && __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ >= 260000
    if(backend==TEST_RUN_SPAWN){launch_error=spawn_command(cfg,&job,pipefd,&pid);if(launch_error)pid=-1;}
    else
#endif
    pid = fork();
    if (pid != 0 && trace) trace->spawn_end_ns = trace_ns();
    if (pid < 0 && !launch_error) { close(pipefd[0]); close(pipefd[1]); agent_sandbox_cleanup(&job);return CXF_ERR_IO; }
    if (!pid) {
        (void)setpgid(0, 0);
        close(pipefd[0]);
        int input = open("/dev/null", O_RDONLY);
        if (input < 0 || dup2(input, STDIN_FILENO) < 0 ||
            dup2(pipefd[1], STDOUT_FILENO) < 0 || dup2(pipefd[1], STDERR_FILENO) < 0) _exit(126);
        if (input > STDERR_FILENO) close(input);
        if (pipefd[1] > STDERR_FILENO) close(pipefd[1]);
        if (chdir(cfg->root)) { perror("test.run cwd"); _exit(126); }
        for(int fd=3;fd<close_limit;fd++)close(fd);
        execve(job.executable, job.argv, job.env);
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
    uint64_t cleanup=trace_ns();if(!agent_sandbox_cleanup(&job))io_error=true;
    if(trace)trace->sandbox_cleanup_ns=trace_ns()-cleanup;
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

/* Guarded one-line editing. The held parent descriptor is also used for the
 * final rename; validation is never followed by a second pathname traversal. */
static bool edit_version(const struct stat *a, const struct stat *b) {
#ifdef __APPLE__
#define MT(st) (st)->st_mtimespec
#define CT(st) (st)->st_ctimespec
#else
#define MT(st) (st)->st_mtim
#define CT(st) (st)->st_ctim
#endif
    bool same = a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
        a->st_size == b->st_size && a->st_mode == b->st_mode &&
        a->st_nlink == b->st_nlink && MT(a).tv_sec == MT(b).tv_sec &&
        MT(a).tv_nsec == MT(b).tv_nsec && CT(a).tv_sec == CT(b).tv_sec &&
        CT(a).tv_nsec == CT(b).tv_nsec;
#undef MT
#undef CT
    return same;
}

cxf_result basic_edit_exec(const void *ctx, const char *args, uint32_t len,
                          char *out, uint32_t cap, uint32_t *out_len) {
    const BasicFsCfg *cfg = ctx;
    uint32_t path_end = 0, old_end, parent_end = 0;
    int parent = -1, source = -1, temporary = -1;
    char leaf[512], temp[80] = {0};
    char *data = NULL;
    struct stat before, after;
    cxf_result rc = CXF_ERR_IO;
    bool created = false;
    if (!out_len) return CXF_ERR_RANGE;
    *out_len = 0;
    if (!cfg || !cfg->root || !args || !out) return CXF_ERR_RANGE;
    if (cap < BASIC_EDIT_OUT_HINT) return CXF_ERR_RANGE;
    if (!len || memchr(args, 0, len)) return CXF_ERR_FORMAT;
    while (path_end < len && args[path_end] != '\n') path_end++;
    old_end = path_end + 1;
    while (old_end < len && args[old_end] != '\n') old_end++;
    if (!path_end || path_end >= sizeof leaf || old_end >= len || old_end == path_end + 1)
        return CXF_ERR_FORMAT;
    if (!agent_policy_path(cfg->policy,cfg->root,args,path_end,false)) return CXF_ERR_RANGE;
    uint32_t old_size = old_end - path_end - 1, new_size = len - old_end - 1;
    const char *old_line = args + path_end + 1, *new_line = args + old_end + 1;
    if (memchr(new_line, '\n', new_size) || memchr(new_line, '\r', new_size) ||
        memchr(old_line, '\r', old_size)) return CXF_ERR_FORMAT;
    for (uint32_t pos = 0; pos < path_end;) {
        uint32_t end = pos;
        while (end < path_end && args[end] != '/') {
            unsigned char ch = (unsigned char)args[end++];
            if (ch < 32 || ch == 127) return CXF_ERR_RANGE;
        }
        uint32_t n = end - pos;
        if (!n || (n == 1 && args[pos] == '.') ||
            (n == 2 && args[pos] == '.' && args[pos + 1] == '.')) return CXF_ERR_RANGE;
        if (end < path_end) {
            if (end + 1 == path_end) return CXF_ERR_RANGE;
            parent_end = end;
        }
        pos = end + 1;
    }
    uint32_t leaf_start = parent_end ? parent_end + 1 : 0;
    memcpy(leaf, args + leaf_start, path_end - leaf_start);
    leaf[path_end - leaf_start] = 0;
    parent = rooted_open(cfg->policy, cfg->root, parent_end ? args : ".", parent_end ? parent_end : 1, true);
    if (parent < 0) return CXF_ERR_IO;
    source = openat(parent, leaf, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (source < 0) goto done;
    if (fstat(source, &before)) goto done;
    rc = CXF_ERR_RANGE;
    if (!agent_policy_stat(cfg->policy,&before) || !S_ISREG(before.st_mode) || before.st_nlink != 1 || before.st_size < 0 ||
        before.st_size > BASIC_EDIT_BYTES) goto done;
    data = malloc(BASIC_EDIT_BYTES + 1);
    if (!data) { rc = CXF_ERR_NOMEM; goto done; }
    uint32_t size = 0;
    for (;;) {
        ssize_t got = read(source, data + size, BASIC_EDIT_BYTES + 1 - size);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) { rc = CXF_ERR_IO; goto done; }
        size += (uint32_t)got;
        if (size > BASIC_EDIT_BYTES) { rc = CXF_ERR_RANGE; goto done; }
        if (!got) break;
    }
    if (memchr(data, 0, size)) { rc = CXF_ERR_FORMAT; goto done; }
    if (fstat(source, &after) || !edit_version(&before, &after) || size != (uint64_t)before.st_size)
        goto done;
    uint32_t matches = 0, start = 0, selected = 0, selected_end = 0;
    while (start < size) {
        uint32_t end = start;
        while (end < size && data[end] != '\n') end++;
        uint32_t text_end = end;
        if (end < size && text_end > start && data[text_end - 1] == '\r') text_end--;
        if (text_end - start == old_size && !memcmp(data + start, old_line, old_size)) {
            matches++; selected = start; selected_end = text_end;
        }
        start = end + 1;
    }
    const char *status = !matches ? "not_found" : matches > 1 ? "ambiguous" : "same";
    uint32_t final_size = size;
    if (matches == 1 && (old_size != new_size || memcmp(old_line, new_line, old_size))) {
        if (new_size > BASIC_EDIT_BYTES - (size - old_size)) { rc = CXF_ERR_RANGE; goto done; }
        final_size = size - old_size + new_size;
        static _Atomic uint32_t serial;
        for (unsigned attempt = 0; attempt < 32; attempt++) {
            snprintf(temp, sizeof temp, ".cxf-edit-%ld-%u.tmp", (long)getpid(), atomic_fetch_add(&serial, 1));
            temporary = openat(parent, temp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (temporary >= 0) { created = true; break; }
            if (errno != EEXIST) break;
        }
        rc = CXF_ERR_IO;
        if (temporary < 0) goto done;
        const char *parts[] = {data, new_line, data + selected_end};
        uint32_t sizes[] = {selected, new_size, size - selected_end};
        for (unsigned part = 0; part < 3; part++) {
            uint32_t n = 0;
            while (n < sizes[part]) {
                ssize_t got = write(temporary, parts[part] + n, sizes[part] - n);
                if (got < 0 && errno == EINTR) continue;
                if (got <= 0) goto done;
                n += (uint32_t)got;
            }
        }
        if (fchmod(temporary, before.st_mode & 0777) || fsync(temporary)) goto done;
        int closed = close(temporary); temporary = -1;
        if (closed) goto done;
        if (fstatat(parent, leaf, &after, AT_SYMLINK_NOFOLLOW) || !agent_policy_stat(cfg->policy,&after) || !edit_version(&before, &after)) goto done;
        if (renameat(parent, temp, parent, leaf)) goto done;
        created = false; status = "applied";
    }
    int n = snprintf(out, cap, "#edit status=%s matches=%u bytes=%u\n", status, matches, final_size);
    *out_len = (uint32_t)n;
    rc = CXF_OK;
done:
    if (temporary >= 0) close(temporary);
    if (created) unlinkat(parent, temp, 0);
    if (source >= 0) close(source);
    if (parent >= 0) close(parent);
    free(data);
    return rc;
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
    "test.run", "Run fixed tests within deployment file and network permissions",
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

const CapDef basic_capdef_edit = {
    "fs.replace_line", "Replace exactly one matching complete line in an existing text file",
    "relative path + newline + nonempty old line + newline + new line (may be empty); no extra trailing newline",
    "#edit status=applied/same/not_found/ambiguous matches=N bytes=N; only applied changes the file",
    { IFACENAME_TASK, NULL }, "fs.view"
};
const CapImpl basic_impl_edit = { "fs.replace_line", "fs", CAP_TIER_SCRIPT, 1, CAP_TRUST_WRITE,
    basic_edit_exec, NULL, BASIC_EDIT_OUT_HINT, NULL };
