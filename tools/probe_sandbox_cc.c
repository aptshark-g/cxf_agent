#define _DARWIN_C_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "agent_basic.h"
static uint64_t now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
/* Diagnostic deployment harness: only the caller supplies the fixed command. */
int main(int argc,char **argv){
    if(argc!=4)return 2;
    TestSandbox sb={0};sb.mode=atoi(argv[2])?TEST_SANDBOX_ON:TEST_SANDBOX_OFF;
    sb.writes=1;sb.write[0]=argv[1];
    TestRunCfg cfg={argv[1],argv[3],30000,1,&sb};
    char out[65536];uint32_t n=0;TestRunTrace t={0};uint64_t start=now();
    int rc=test_run_exec_traced(&cfg,"",0,out,sizeof out,&n,&t);uint64_t wall=now()-start;
    fwrite(out,1,n,stdout);
    printf("\nCC_METRICS {\"rc\":%d,\"wall_ms\":%.6f,\"prepare_ms\":%.6f,\"cleanup_ms\":%.6f,\"spawn_ms\":%.6f,\"to_first_output_ms\":%.6f,\"entries\":%u,\"profile_bytes\":%u}\n",rc,wall/1e6,t.sandbox_prepare_ns/1e6,t.sandbox_cleanup_ns/1e6,(t.spawn_end_ns-t.spawn_begin_ns)/1e6,t.first_output_ns?(t.first_output_ns-t.spawn_end_ns)/1e6:0,t.sandbox_entries,t.sandbox_profile_bytes);
    return rc?1:0;
}
