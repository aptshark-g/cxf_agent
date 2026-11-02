#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#else
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "agent_basic.h"
static uint64_t ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
int main(int argc,char **argv){
 if(argc!=4)return 2;const char *root=argv[1];unsigned files=(unsigned)atoi(argv[2]),reps=(unsigned)atoi(argv[3]);
 const char *commands[]={"true","grep -q needle corpus.txt","d=$(mktemp -d) || exit 2; DEVELOPER_DIR=/Library/Developer/CommandLineTools cc unit.c -o \"$d/unit\" && \"$d/unit\"; rc=$?; rm -rf \"$d\"; exit $rc","cc unit.c -o unit && ./unit"};
 const char *names[]={"noop","read","clt_compile_tmp","system_cc_compile_write"};
 AgentPolicy policy={0};char path[1024];snprintf(path,sizeof path,"%s/runtime.json",root);if(!agent_policy_protect(&policy,path))return 2;
 for(unsigned rep=0;rep<reps+2;rep++) {
 for(unsigned c=0;c<4;c++) {
 for(unsigned order=0;order<2;order++) {
  unsigned on=(rep+c+files+order)%2;TestSandbox sandbox={0};sandbox.mode=on?TEST_SANDBOX_ON:TEST_SANDBOX_OFF;sandbox.policy=&policy;if(c==3){sandbox.writes=1;sandbox.write[0]=root;}
  char command[1024];snprintf(command,sizeof command,"./probe measure '%s'",commands[c]);TestRunCfg cfg={root,command,30000,1,&sandbox};char out[BASIC_OUT_HINT];uint32_t n;TestRunTrace t;uint64_t begin=ns();int rc=test_run_exec_traced(&cfg,"",0,out,sizeof out,&n,&t);uint64_t wall=ns()-begin;
  if(n>=sizeof out)return 3;out[n]=0;char *p=strstr(out,"INNER_NS=");unsigned long long inner=p?strtoull(p+9,NULL,10):0;bool passed=rc==0&&strstr(out,"exit=0 ")&&inner;
  printf("{\"kind\":\"sandbox_timing\",\"case\":\"%s\",\"files\":%u,\"rep\":%u,\"phase\":\"%s\",\"order\":%u,\"mode\":\"%s\",\"rc\":%d,\"passed\":%s,\"wall_ms\":%.6f,\"inner_ms\":%.6f,\"prepare_ms\":%.6f,\"cleanup_ms\":%.6f,\"spawn_ms\":%.6f,\"entries\":%u,\"profile_bytes\":%u,\"job_struct_bytes\":%zu}\n",names[c],files,rep,rep<2?"warmup":"evaluation",order,on?"on":"off",rc,passed?"true":"false",wall/1e6,inner/1e6,t.sandbox_prepare_ns/1e6,t.sandbox_cleanup_ns/1e6,(t.spawn_end_ns-t.spawn_begin_ns)/1e6,t.sandbox_entries,t.sandbox_profile_bytes,sizeof(AgentSandboxJob));fflush(stdout);if(!passed){fprintf(stderr,"benchmark command failed case=%s mode=%u rc=%d\n",names[c],on,rc);return 1;}
 }
 }
 }
 return 0;
}
