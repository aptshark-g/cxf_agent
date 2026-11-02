#define _DARWIN_C_SOURCE 1
#include <errno.h>
#include <mach/mach.h>
#include <pwd.h>
#include <servers/bootstrap.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
static unsigned long long ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (unsigned long long)t.tv_sec*1000000000ull+t.tv_nsec;}
int main(int argc,char **argv){
    unsigned n=argc>1?(unsigned)strtoul(argv[1],NULL,10):100;if(!n||n>10000)return 2;
    const char *services[]={"com.apple.system.opendirectoryd.libinfo","com.apple.FSEvents","com.apple.diagnosticd"};
    for(unsigned k=0;k<5;k++){
        unsigned success=0;int last=0;unsigned long long total=0,max=0;
        for(unsigned i=0;i<n;i++){
            unsigned long long start=ns();
            if(k<3){mach_port_t p=MACH_PORT_NULL;last=bootstrap_look_up(bootstrap_port,services[k],&p);if(last==KERN_SUCCESS){success++;mach_port_deallocate(mach_task_self(),p);}}
            else if(k==3){errno=0;size_t size=confstr(_CS_DARWIN_USER_CACHE_DIR,NULL,0);last=size?0:errno;if(size)success++;}
            else{struct passwd data,*result=NULL;char storage[16384];last=getpwuid_r(getuid(),&data,storage,sizeof storage,&result);if(!last&&result)success++;}
            unsigned long long elapsed=ns()-start;total+=elapsed;if(elapsed>max)max=elapsed;
        }
        printf("IPC_PROBE {\"case\":\"%s\",\"calls\":%u,\"success\":%u,\"last_rc\":%d,\"total_ms\":%.6f,\"max_ms\":%.6f}\n",k<3?services[k]:k==3?"darwin_cache_confstr":"own_uid_lookup",n,success,last,total/1e6,max/1e6);
    }return 0;
}
