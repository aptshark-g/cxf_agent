#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "agent_basic.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>
static uint64_t tick(void){struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
int main(int argc,char **argv){
    assert(argc==2);char path[1600];snprintf(path,sizeof path,"%s/x.txt",argv[1]);
    for(unsigned size=1024;size<=65536;size=size==1024?16384:size==16384?65536:65537){
        char *text=malloc(size);assert(text);memset(text,'p',size);for(unsigned i=63;i<size;i+=64)text[i]='\n';
        int fd=open(path,O_CREAT|O_TRUNC|O_RDWR,0600);assert(fd>=0&&write(fd,text,size)==size);struct stat st;assert(!fstat(fd,&st));
        AgentPolicy policies[3]={{0}};unsigned counts[]={0,1,16};
        for(unsigned p=0;p<3;p++)for(unsigned i=0;i<counts[p];i++){char secret[80];snprintf(secret,sizeof secret,"synthetic-diagnostic-value-%03u",i);assert(agent_policy_secret(&policies[p],secret));}
        for(unsigned block=0;block<10;block++)for(unsigned stage=0;stage<5;stage++){
            unsigned iterations=stage<2?256:8;uint64_t start=tick();char version[BASIC_VERSION_CAP];
            for(unsigned i=0;i<iterations;i++){
                if(stage==0){assert(!fstat(fd,&st));basic_file_version(&st,version);assert(version[0]=='v');}
                else if(stage==1){assert(!flock(fd,LOCK_EX|LOCK_NB));assert(!flock(fd,LOCK_UN));}
                else {assert(agent_policy_text(&policies[stage-2],text,size));}
            }
            uint64_t elapsed=tick()-start;const char *names[]={"metadata_version","flock_pair","policy_0","policy_1","policy_16"};
            printf("{\"kind\":\"timing\",\"case\":\"%s\",\"file_bytes\":%u,\"block\":%u,\"warmup\":%s,\"iterations\":%u,\"wall_ns\":%llu,\"correct\":true}\n",names[stage],size,block,block==0?"true":"false",iterations,(unsigned long long)elapsed);
        }
        BasicFsCfg cfg={argv[1],NULL};char out[BASIC_OUT_HINT];uint32_t n=0;assert(basic_read_exec(&cfg,"x.txt",5,out,sizeof out,&n)==CXF_OK);
        char *end=memchr(out,'\n',n);assert(end);unsigned body=(unsigned)(n-(uint32_t)(end-out)-1),header=(unsigned)(end-out+1);
        char old[256];int old_n=snprintf(old,sizeof old,"#file x.txt offset=0 bytes=%u total=%u next=%d\n",body,size,body<size?(int)body:-1);
        printf("{\"kind\":\"receipt_size\",\"file_bytes\":%u,\"current_header_bytes\":%u,\"baseline_header_bytes_calculated\":%d,\"additional_bytes\":%d}\n",size,header,old_n,(int)header-old_n);
        close(fd);free(text);assert(!unlink(path));
    }
}
