#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#else
#define _POSIX_C_SOURCE 200809L
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
int main(int argc,char **argv){
 if(argc<2)return 2;
#ifdef __APPLE__
 if(!strcmp(argv[1],"parent")){char data[65536];size_t n=sizeof data;int mib[3]={CTL_KERN,KERN_PROCARGS2,atoi(argv[2])};int rc=sysctl(mib,3,data,&n,NULL,0),error=errno,found=0;if(!rc)for(size_t i=0;i+25<=n;i++)if(!memcmp(data+i,"PARENT_ENV_SYNTHETIC_71391",25))found=1;printf("PARENT_SCAN_RC=%d errno=%d FOUND_MARKER=%d\n",rc,rc?error:0,found);return found?7:0;}
#endif
 if(!strcmp(argv[1],"measure")){struct timespec a,b;clock_gettime(CLOCK_MONOTONIC,&a);pid_t child=fork();if(child<0)return 3;if(!child){execl("/bin/sh","sh","-c",argv[2],NULL);_exit(127);}int status;if(waitpid(child,&status,0)!=child)return 4;clock_gettime(CLOCK_MONOTONIC,&b);unsigned long long ns=(b.tv_sec-a.tv_sec)*1000000000ull+b.tv_nsec-a.tv_nsec;printf("INNER_NS=%llu\n",ns);return WIFEXITED(status)?WEXITSTATUS(status):5;}
 if(!strcmp(argv[1],"read")){int fd=open(argv[2],O_RDONLY);if(fd<0)return 3;char b[256];ssize_t n;while((n=read(fd,b,sizeof b))>0)if(write(1,b,n)!=n)return 4;close(fd);return n<0?5:0;}
 if(!strcmp(argv[1],"write")){int fd=open(argv[2],O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd<0)return 3;int ok=write(fd,"modified",8)==8;close(fd);return ok?0:4;}
 if(!strcmp(argv[1],"fd")){char b[256];ssize_t n=read(atoi(argv[2]),b,sizeof b);if(n>0){write(1,b,n);return 0;}return 3;}
 if(!strcmp(argv[1],"tcp")||!strcmp(argv[1],"udp")){int udp=!strcmp(argv[1],"udp"),fd=socket(AF_INET,udp?SOCK_DGRAM:SOCK_STREAM,0);if(fd<0)return 3;struct sockaddr_in a={0};a.sin_family=AF_INET;a.sin_port=htons((unsigned short)atoi(argv[2]));inet_pton(AF_INET,argc>3?argv[3]:"127.0.0.1",&a.sin_addr);int rc=0;if(udp)rc=sendto(fd,"probe",5,0,(struct sockaddr *)&a,sizeof a)==5?0:4;else if(connect(fd,(struct sockaddr *)&a,sizeof a))rc=4;else if(write(fd,"probe",5)!=5)rc=5;close(fd);return rc;}
 if(!strcmp(argv[1],"unix")){int fd=socket(AF_UNIX,SOCK_STREAM,0);if(fd<0)return 3;struct sockaddr_un a={0};a.sun_family=AF_UNIX;if(strlen(argv[2])>=sizeof a.sun_path)return 2;strcpy(a.sun_path,argv[2]);int rc=connect(fd,(struct sockaddr *)&a,sizeof a)?4:0;if(!rc&&write(fd,"probe",5)!=5)rc=5;close(fd);return rc;}
 if(!strcmp(argv[1],"child")){pid_t p=fork();if(p<0)return 3;if(!p){execl(argv[0],argv[0],"read",argv[2],NULL);_exit(127);}int status;if(waitpid(p,&status,0)!=p)return 4;return WIFEXITED(status)?WEXITSTATUS(status):5;}
 return 2;
}
