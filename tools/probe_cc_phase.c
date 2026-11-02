#define _DARWIN_C_SOURCE 1
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static unsigned long long ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (unsigned long long)t.tv_sec*1000000000ull+t.tv_nsec;}
/* Runs one argv vector inside the same sandbox, without shell interpolation. */
int main(int argc,char **argv){
    if(argc<3)return 2;unsigned long long start=ns();pid_t p=fork();if(p<0)return 3;
    if(!p){execvp(argv[2],argv+2);_exit(127);}
    int status=0;struct rusage usage={0};pid_t got;
    do{got=wait4(p,&status,0,&usage);}while(got<0&&errno==EINTR);
    if(got!=p)return 4;int rc=WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status);
    printf("\nCC_PHASE {\"label\":\"%s\",\"wall_ms\":%.6f,\"user_ms\":%.6f,\"sys_ms\":%.6f,\"exit\":%d}\n",argv[1],(ns()-start)/1e6,usage.ru_utime.tv_sec*1000.+usage.ru_utime.tv_usec/1000.,usage.ru_stime.tv_sec*1000.+usage.ru_stime.tv_usec/1000.,rc);fflush(stdout);return rc;
}
