#define _DARWIN_C_SOURCE 1
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static unsigned long long ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (unsigned long long)t.tv_sec*1000000000ull+t.tv_nsec;}
/* Benchmark interposer only: never used by the normal recipe unless CXX selects it. */
int main(int argc,char **argv){
    const char *compiler=getenv("CPP_BENCH_COMPILER"),*label="link",*output=NULL;if(!compiler||!*compiler)return 2;
    for(int i=1;i<argc;i++){if(!strcmp(argv[i],"-c"))label="object";if(!strcmp(argv[i],"c++-header"))label="pch";if(!strcmp(argv[i],"-o")&&i+1<argc)output=argv[i+1];}
    unsigned long long start=ns();pid_t p=fork();if(p<0)return 3;
    if(!p){argv[0]=(char *)compiler;execv(compiler,argv);_exit(127);}
    int status=0;struct rusage usage={0};pid_t got;do{got=wait4(p,&status,0,&usage);}while(got<0&&errno==EINTR);if(got!=p)return 4;
    int rc=WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status);struct stat st;unsigned long long bytes=output&&!stat(output,&st)?(unsigned long long)st.st_size:0;
    printf("CC_PHASE {\"label\":\"%s\",\"wall_ms\":%.6f,\"user_ms\":%.6f,\"sys_ms\":%.6f,\"output_file_bytes\":%llu,\"exit\":%d}\n",label,(ns()-start)/1e6,usage.ru_utime.tv_sec*1000.+usage.ru_utime.tv_usec/1000.,usage.ru_stime.tv_sec*1000.+usage.ru_stime.tv_usec/1000.,bytes,rc);fflush(stdout);return rc;
}
