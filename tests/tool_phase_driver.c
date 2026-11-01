/* Trusted benchmark-only compiler/check driver. Fixed cwd/filenames/arguments. */
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <crt_externs.h>
#define ENVIRON (*_NSGetEnviron())
#else
extern char **environ;
#define ENVIRON environ
#endif
static uint64_t ns(void){struct timespec t;
#ifdef __APPLE__
clock_gettime(CLOCK_MONOTONIC_RAW,&t);
#else
clock_gettime(CLOCK_MONOTONIC,&t);
#endif
return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
static double ms(struct timeval t){return t.tv_sec*1000.0+t.tv_usec/1000.0;}
static int phase(FILE *f,const char *name,char *const argv[],int quiet){
    int running=strcmp(name,"compile")!=0;if(running)(void)remove("entry-metrics");
    posix_spawn_file_actions_t actions;int rc=posix_spawn_file_actions_init(&actions);if(rc)return 126;
    if(quiet)rc=posix_spawn_file_actions_addopen(&actions,STDOUT_FILENO,"/dev/null",O_WRONLY,0);
    uint64_t start=ns();pid_t pid;int status=0;struct rusage r={0};
    if(!rc)rc=posix_spawnp(&pid,argv[0],&actions,NULL,argv,ENVIRON);
    uint64_t spawned=ns();posix_spawn_file_actions_destroy(&actions);if(rc)return 126;
    pid_t got;do{got=wait4(pid,&status,0,&r);}while(got<0&&errno==EINTR);
    uint64_t end=ns();if(got!=pid)return 126;
    unsigned long long entry=0,computed=0;
    if(running){FILE *e=fopen("entry-metrics","r");if(e){if(fscanf(e,"%llu %llu",&entry,&computed)!=2){fclose(e);return 126;}fclose(e);}}
    fprintf(f,"%s %llu %llu %.6f %.6f %ld %ld %ld %ld %ld %ld %llu %llu %llu 1\n",name,(unsigned long long)start,(unsigned long long)end,ms(r.ru_utime),ms(r.ru_stime),r.ru_minflt,r.ru_majflt,r.ru_inblock,r.ru_oublock,r.ru_nvcsw,r.ru_nivcsw,(unsigned long long)spawned,entry,computed);
    fflush(f);return WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status);
}
static uint64_t modified(const struct stat *s){
#ifdef __APPLE__
return (uint64_t)s->st_mtimespec.tv_sec*1000000000ull+s->st_mtimespec.tv_nsec;
#else
return (uint64_t)s->st_mtim.tv_sec*1000000000ull+s->st_mtim.tv_nsec;
#endif
}
int main(int argc,char **argv){
    int repeat=argc>1,incremental=argc>1&&!strcmp(argv[1],"--incremental");
    FILE *f=fopen("phase-metrics","w");if(!f)return 126;
    char *cc[]={"cc","-O2","calc.c","verify.c","-o","check",NULL};char *run[]={"./check",NULL};
    struct stat target,a,b;int cached=incremental&&!stat("check",&target)&&S_ISREG(target.st_mode)&&!access("check",X_OK)&&!stat("calc.c",&a)&&!stat("verify.c",&b)&&modified(&target)>=modified(&a)&&modified(&target)>=modified(&b);
    int rc=0;if(cached){uint64_t t=ns();fprintf(f,"compile %llu %llu 0 0 0 0 0 0 0 0 %llu 0 0 0\n",(unsigned long long)t,(unsigned long long)t,(unsigned long long)t);fflush(f);}else rc=phase(f,"compile",cc,0);
    if(!rc){rc=phase(f,"run",run,0);if(repeat){int second=phase(f,"rerun",run,1);if(second!=rc)rc=126;}}
    if(fclose(f))return 126;return rc;
}
