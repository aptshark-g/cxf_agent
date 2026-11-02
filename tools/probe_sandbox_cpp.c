#define _DARWIN_C_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "agent_basic.h"
static uint64_t now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
/* Diagnostic only. Uses the normal runner, paired deployment bindings and no grants. */
int main(int argc,char **argv){
    if(argc!=6)return 2;TestSandbox sb={0};sb.mode=atoi(argv[2])?TEST_SANDBOX_ON:TEST_SANDBOX_OFF;
    sb.toolchain_bin=argv[4];sb.sdkroot=argv[5];
    TestRunCfg cfg={argv[1],argv[3],30000,1,&sb};char out[BASIC_OUT_HINT];uint32_t n=0;TestRunTrace t={0};uint64_t start=now();
    int rc=test_run_exec_traced(&cfg,"",0,out,sizeof out,&n,&t);uint64_t wall=now()-start;fwrite(out,1,n,stdout);
    printf("\nCPP_METRICS {\"rc\":%d,\"wall_ms\":%.6f,\"prepare_ms\":%.6f,\"cleanup_ms\":%.6f,\"spawn_ms\":%.6f,\"eof_wait_ms\":%.6f,\"eof_polls\":%llu,\"entries\":%u,\"profile_bytes\":%u,\"job_struct_bytes\":%zu}\n",rc,wall/1e6,t.sandbox_prepare_ns/1e6,t.sandbox_cleanup_ns/1e6,(t.spawn_end_ns-t.spawn_begin_ns)/1e6,t.eof_poll_ns/1e6,(unsigned long long)t.eof_polls,t.sandbox_entries,t.sandbox_profile_bytes,sizeof(AgentSandboxJob));return rc?1:0;
}
