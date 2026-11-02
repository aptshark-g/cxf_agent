#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "agent_basic.h"
#include "agent_fs.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
static uint64_t tick(void){struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static uint64_t cpu(struct timeval t){return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_usec*1000;}
int main(int argc,char **argv){
    if(argc!=5)return 2;const char *mode=argv[1],*root=argv[2];unsigned size=(unsigned)strtoul(argv[3],NULL,10),iterations=(unsigned)strtoul(argv[4],NULL,10);
    assert(size>=1024&&size<=65536&&iterations>0&&iterations<=128);
    char *initial=malloc(size+1),*expected=malloc(size+1),*whole=malloc(size+32);assert(initial&&expected&&whole);
    memset(initial,'p',size);for(unsigned i=63;i<size;i+=64)initial[i]='\n';initial[size]=0;initial[255]='\n';
    bool multi=!strcmp(mode,"multi"),same=!strcmp(mode,"same"),stale=!strcmp(mode,"stale"),reading=!strcmp(mode,"read"),workflow=!strcmp(mode,"workflow");
    const char *old=multi?"old_one\nold_two":"old_line",*replacement=same?old:multi?"new_one\nnew_two":"new_line";
    memcpy(initial+256,old,strlen(old));initial[256+strlen(old)]='\n';memcpy(expected,initial,size+1);memcpy(expected+256,replacement,strlen(replacement));
    snprintf(whole,size+32,"x.txt\n%s",expected);char file[1600];snprintf(file,sizeof file,"%s/x.txt",root);
    BasicFsCfg cfg={root,NULL};FsRootCfg fs={root,NULL};(void)fs;char args[1600],out[BASIC_OUT_HINT+1];uint32_t written=0;uint64_t wall=0,user=0,system=0;unsigned argument_bytes=0;
    for(unsigned rep=0;rep<iterations;rep++){
        FILE *f=fopen(file,"wb");assert(f&&fwrite(initial,1,size,f)==size&&!fclose(f));
        if(!multi)snprintf(args,sizeof args,"x.txt\n%s\n%s",old,replacement);
#ifdef CXF_EDIT_CURRENT
        struct stat st;assert(!stat(file,&st));char version[BASIC_VERSION_CAP];basic_file_version(&st,version);
        if(stale)strcpy(version,"v1:0");
        snprintf(args,sizeof args,"x.txt\n%s\n<<<<<<< SEARCH\n%s\n=======\n%s\n>>>>>>> REPLACE",version,old,replacement);
#else
        assert(!stale);
#endif
        struct rusage before,after;assert(!getrusage(RUSAGE_SELF,&before));
        uint64_t started=tick();cxf_result rc=CXF_OK;
        if(reading||workflow){
            rc=basic_read_exec(&cfg,"x.txt",5,out,sizeof out,&written);
#ifdef CXF_EDIT_CURRENT
            if(workflow&&rc==CXF_OK){
                out[written]=0;char *v=strstr(out," version=");assert(v);v+=9;char *end=strchr(v,' ');assert(end);*end=0;
                snprintf(args,sizeof args,"x.txt\n%s\n<<<<<<< SEARCH\n%s\n=======\n%s\n>>>>>>> REPLACE",v,old,replacement);
            }
#endif
        }
        if(!reading&&rc==CXF_OK){
#ifdef CXF_EDIT_CURRENT
            rc=basic_patch_exec(&cfg,args,(uint32_t)strlen(args),out,sizeof out,&written);argument_bytes=(unsigned)strlen(args);
#else
            if(multi){rc=fs_write_apply_exec(&fs,whole,(uint32_t)strlen(whole),out,sizeof out,&written);argument_bytes=(unsigned)strlen(whole);}
            else {rc=basic_edit_exec(&cfg,args,(uint32_t)strlen(args),out,sizeof out,&written);argument_bytes=(unsigned)strlen(args);}
#endif
        }
        wall+=tick()-started;assert(!getrusage(RUSAGE_SELF,&after));user+=cpu(after.ru_utime)-cpu(before.ru_utime);system+=cpu(after.ru_stime)-cpu(before.ru_stime);
        assert(rc==CXF_OK);out[written]=0;
        if(!reading){
            if(stale)assert(strstr(out,"status=conflict")&&strstr(out,"reason=stale"));
            else if(same)assert(strstr(out,"status=same"));
            else {
#ifdef CXF_EDIT_CURRENT
                assert(strstr(out,"status=applied"));
#else
                assert(strstr(out,multi?"wrote ":"status=applied"));
#endif
            }
        }
        f=fopen(file,"rb");assert(f);char *actual=malloc(size+1);assert(actual);size_t n=fread(actual,1,size+1,f);assert(!ferror(f)&&!fclose(f));assert(n==size&&!memcmp(actual,reading||stale?initial:expected,size));free(actual);
    }
    assert(!unlink(file));printf("{\"kind\":\"timing\",\"case\":\"%s\",\"file_bytes\":%u,\"iterations\":%u,\"wall_ns\":%llu,\"user_ns\":%llu,\"system_ns\":%llu,\"argument_bytes\":%u,\"correct\":true}\n",mode,size,iterations,(unsigned long long)wall,(unsigned long long)user,(unsigned long long)system,argument_bytes);
    free(whole);free(expected);free(initial);return 0;
}
