#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#else
#define _POSIX_C_SOURCE 200809L
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "agent_basic.h"

static char out[BASIC_OUT_HINT + 1];
static uint32_t n;

static void put(const char *root, const char *name, const char *s) {
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", root, name);
    FILE *f = fopen(p, "w"); assert(f);
    assert(fwrite(s, 1, strlen(s), f) == strlen(s)); assert(!fclose(f));
}

static void test_fs(const char *root) {
    BasicFsCfg cfg = { root };
    char p[1024], large[7000];
    put(root, "z.txt", "hello\nworld\n");
    put(root, "a.txt", "alpha\n");
    assert(basic_read_exec(&cfg, "z.txt", 5, out, sizeof out, &n) == CXF_OK);
    out[n] = 0;
    assert(strstr(out, "next=-1\nhello\nworld\n"));
    put(root, "empty.txt", "");
    assert(basic_read_exec(&cfg, "empty.txt", 9, out, sizeof out, &n) == CXF_OK);
    out[n] = 0; assert(strstr(out, "bytes=0 total=0 next=-1"));

    /* A page cuts through a three-byte character; next points to the first
     * unread character and every original byte is recoverable across pages. */
    for (unsigned i = 0; i < 2000; i++) memcpy(large + i * 3, "中", 3);
    large[6000] = 0; put(root, "large.txt", large);
    assert(basic_read_exec(&cfg, "large.txt", 9, out, sizeof out, &n) == CXF_OK);
    out[n] = 0;
    char *body = strchr(out, '\n') + 1;
    assert(strstr(out, "bytes=4095 total=6000 next=4095"));
    assert(n - (uint32_t)(body - out) == 4095);
    assert(!memcmp(body, large, 4095));
    assert(basic_read_exec(&cfg, "large.txt\n4095", 14, out, sizeof out, &n) == CXF_OK);
    out[n] = 0; body = strchr(out, '\n') + 1;
    assert(strstr(out, "bytes=1905 total=6000 next=-1"));
    assert(!memcmp(body, large + 4095, 1905));
    assert(basic_read_exec(&cfg, "large.txt\n4096", 14, out, sizeof out, &n) == CXF_ERR_RANGE);
    assert(basic_read_exec(&cfg, "large.txt\n-1", 12, out, sizeof out, &n) == CXF_ERR_FORMAT);
    assert(basic_read_exec(&cfg, "large.txt\n4294967296", 20, out, sizeof out, &n) == CXF_ERR_FORMAT);
    assert(basic_read_exec(&cfg, "z.txt", 5, out, 1, &n) == CXF_ERR_FULL);
    assert(n == 0);
    assert(basic_read_exec(&cfg, "../outside", 10, out, sizeof out, &n) != CXF_OK);
    assert(basic_read_exec(&cfg, "/etc/passwd", 11, out, sizeof out, &n) != CXF_OK);
    snprintf(p, sizeof p, "%s/link", root); assert(!symlink("z.txt", p));
    assert(basic_read_exec(&cfg, "link", 4, out, sizeof out, &n) != CXF_OK);
    snprintf(p, sizeof p, "%s/escape", root); assert(!symlink("/tmp", p));
    assert(basic_list_exec(&cfg, "escape", 6, out, sizeof out, &n) != CXF_OK);
    assert(basic_list_exec(&cfg, ".", 1, out, sizeof out, &n) == CXF_OK);
    out[n] = 0;
    assert(strstr(out, "f a.txt\n") < strstr(out, "f z.txt\n"));
    assert(strstr(out, "l link\n") && strstr(out, "#list shown=6 total=6 omitted=0\n"));
    /* Too many entries: choose the same lexicographic prefix, report loss. */
    for (unsigned i = 0; i < 135; i++) {
        char name[32]; snprintf(name, sizeof name, "item%03u", i); put(root, name, "");
    }
    assert(basic_list_exec(&cfg, ".", 1, out, sizeof out, &n) == CXF_OK);
    out[n] = 0;
    assert(strstr(out, "shown=128 total=141") && strstr(out, "#truncated"));
    for (unsigned i = 0; i < 135; i++) {
        snprintf(p, sizeof p, "%s/item%03u", root, i); assert(!unlink(p));
    }
    const char *names[] = { "a.txt", "z.txt", "empty.txt", "large.txt", "link", "escape" };
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
        snprintf(p, sizeof p, "%s/%s", root, names[i]); assert(!unlink(p));
    }
}

static void test_process(const char *root) {
    TestRunCfg cfg = { root, "printf 'TEST_OK\\n'; printf 'ERR_STREAM\\n' >&2", 1000 };
    assert(test_run_exec(&cfg, "", 0, out, sizeof out, &n) == CXF_OK);
    out[n] = 0;
    assert(strstr(out, "exit=0 signal=0 timeout=0 io_error=0"));
    assert(strstr(out, "TEST_OK") && strstr(out, "ERR_STREAM"));
    cfg.command = "printf failure; exit 7";
    assert(test_run_exec(&cfg, "", 0, out, sizeof out, &n) == CXF_OK);
    out[n] = 0; assert(strstr(out, "exit=7") && strstr(out, "failure"));
    cfg.command = "touch MUST_NOT_RUN";
    assert(test_run_exec(&cfg, "", 0, out, 1, &n) == CXF_ERR_RANGE);
    assert(test_run_exec(&cfg, "touch INJECTED", 14, out, sizeof out, &n) == CXF_ERR_FORMAT);
    char p[1024]; snprintf(p, sizeof p, "%s/MUST_NOT_RUN", root); assert(access(p, F_OK));
    snprintf(p, sizeof p, "%s/INJECTED", root); assert(access(p, F_OK));
    cfg.command = "pwd";
    assert(test_run_exec(&cfg, "", 0, out, sizeof out, &n) == CXF_OK);
    out[n] = 0; assert(strstr(out, root));
    cfg.command = "sleep 1; touch TOO_LATE"; cfg.timeout_ms = 30;
    assert(test_run_exec(&cfg, "", 0, out, sizeof out, &n) == CXF_OK);
    out[n] = 0; assert(strstr(out, "timeout=1"));
    sleep(1);
    snprintf(p, sizeof p, "%s/TOO_LATE", root); assert(access(p, F_OK));
    cfg.command = "i=0; while [ \"$i\" -lt 5000 ]; do printf x; i=$((i+1)); done";
    cfg.timeout_ms = 5000;
    assert(test_run_exec(&cfg, "", 0, out, sizeof out, &n) == CXF_OK);
    out[n] = 0; assert(strstr(out, "exit=0") && strstr(out, "#truncated"));
    cfg.command = "yes x"; cfg.timeout_ms = 100;
    assert(test_run_exec(&cfg, "", 0, out, sizeof out, &n) == CXF_OK);
    out[n] = 0; assert(strstr(out, "timeout=1"));
    assert(n < BASIC_OUT_HINT);
    cfg.command = "true"; cfg.root = "/tmp/cxf-does-not-exist-basic-test";
    assert(test_run_exec(&cfg, "", 0, out, sizeof out, &n) == CXF_OK);
    out[n] = 0; assert(strstr(out, "exit=126"));
}
static void test_process_trace(const char *root) {
    TestRunTrace trace;TestRunCfg cfg={root,"printf x >> trace-once; printf TRACE",1000};
    assert(test_run_exec_traced(&cfg,"",0,out,sizeof out,&n,&trace)==CXF_OK);
    assert(trace.entered_ns<=trace.spawn_begin_ns && trace.spawn_begin_ns<=trace.spawn_end_ns);
    assert(trace.spawn_end_ns<=trace.first_output_ns && trace.first_output_ns<=trace.finished_ns);
    assert(trace.spawn_end_ns<=trace.reaped_ns && trace.reaped_ns<=trace.finished_ns);
    char path[1024];snprintf(path,sizeof path,"%s/trace-once",root);FILE *f=fopen(path,"r");assert(f);
    assert(fgetc(f)=='x' && fgetc(f)==EOF);fclose(f);assert(!unlink(path));
    assert(test_run_exec_traced(&cfg,"",0,out,1,&n,&trace)==CXF_ERR_RANGE && !trace.spawn_begin_ns);
    assert(access(path,F_OK));
    cfg.command="printf FORK";
    assert(test_run_exec_observed(&cfg,"",0,out,sizeof out,&n,&trace,TEST_RUN_FORK,NULL,NULL)==CXF_OK);
    out[n]=0;assert(strstr(out,"exit=0") && trace.backend==TEST_RUN_FORK);
    assert(test_run_exec_observed(&cfg,"",0,out,sizeof out,&n,&trace,(TestRunBackend)9,NULL,NULL)==CXF_ERR_RANGE && !trace.spawn_begin_ns);
}

static void test_exit_event(const char *root, bool race) {
    cxf_result (*execute)(const void *,const char *,uint32_t,char *,uint32_t,uint32_t *,TestRunTrace *,TestRunOutputFn,void *)=race?test_run_exec_output_race:test_run_exec_output_event;
    TestRunTrace trace;TestRunCfg cfg={root,"printf x >> event-once; printf EVENT; exec 1>&- 2>&-; sleep 0.03; exit 7",1000};
    assert(execute(&cfg,"",0,out,sizeof out,&n,&trace,NULL,NULL)==CXF_OK);
    out[n]=0;assert(strstr(out,"EVENT") && strstr(out,"exit=7") && strstr(out,"timeout=0"));
    assert(trace.eof_ns<trace.reaped_ns && trace.eof_polls>0);
    char path[1024];snprintf(path,sizeof path,"%s/event-once",root);FILE *f=fopen(path,"r");assert(f);
    assert(fgetc(f)=='x' && fgetc(f)==EOF);fclose(f);assert(!unlink(path));
    assert(execute(&cfg,"",0,out,1,&n,&trace,NULL,NULL)==CXF_ERR_RANGE);assert(access(path,F_OK));
    cfg.command="exec 1>&- 2>&-; sleep 1; touch EVENT_TOO_LATE";cfg.timeout_ms=30;
    assert(execute(&cfg,"",0,out,sizeof out,&n,&trace,NULL,NULL)==CXF_OK);
    out[n]=0;assert(strstr(out,"timeout=1"));sleep(1);
    snprintf(path,sizeof path,"%s/EVENT_TOO_LATE",root);assert(access(path,F_OK));
    cfg.command="true";cfg.timeout_ms=1000;
    for(unsigned i=0;i<30;i++){assert(execute(&cfg,"",0,out,sizeof out,&n,&trace,NULL,NULL)==CXF_OK);out[n]=0;assert(strstr(out,"exit=0"));}
}

int main(void) {
    char root[] = "/tmp/cxf-basic-XXXXXX";
    assert(mkdtemp(root));
    test_fs(root);
    test_process(root);
    test_process_trace(root);
    test_exit_event(root,false);
    test_exit_event(root,true);
    assert(!rmdir(root));
    puts("all basic tool tests passed");
    return 0;
}
