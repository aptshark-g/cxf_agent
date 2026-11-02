#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#else
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>
#endif
#include "agent_sandbox.h"
#ifdef __APPLE__
/* XNU codesign.h / csr.h ABI, unavailable in the public SDK headers. */
extern int csops(pid_t,unsigned int,void *,size_t) __attribute__((weak_import));
extern int csr_check(unsigned int) __attribute__((weak_import));
static _Atomic int protected_pid;
static int has_environment(const char *data,size_t n) {
    if(n<sizeof(int))return -1;int argc;memcpy(&argc,data,sizeof argc);if(argc<0||argc>65536)return -1;
    const char *p=data+sizeof argc,*end=data+n,*q=memchr(p,0,(size_t)(end-p));if(!q)return -1;p=q+1;
    while(p<end&&!*p)p++;
    for(int i=0;i<argc;i++){q=memchr(p,0,(size_t)(end-p));if(!q)return -1;p=q+1;}
    return p<end&&*p?1:0;
}
#endif
cxf_result agent_sandbox_host_protect(void) {
#ifndef __APPLE__
    return CXF_ERR_DEP;
#else
    int pid=(int)getpid();if(atomic_load(&protected_pid)==pid)return CXF_OK;
    if(!csops||!csr_check)return CXF_ERR_DEP;
    errno=0;if(csr_check(1u<<5)==0||errno!=EPERM)return CXF_ERR_DEP;
    unsigned flags=0;if(csops(pid,8,NULL,0)||csops(pid,0,&flags,sizeof flags)||!(flags&0x800u))return CXF_ERR_DEP;
    /* A trusted probe checks the effective kernel behavior, not just a flag. */
    char *data=malloc(262144);if(!data)return CXF_ERR_NOMEM;int channel[2];if(pipe(channel)){free(data);return CXF_ERR_IO;}
    fcntl(channel[0],F_SETFD,FD_CLOEXEC);fcntl(channel[1],F_SETFD,FD_CLOEXEC);pid_t child=fork();
    if(child==0){close(channel[0]);size_t n=262144;int mib[3]={CTL_KERN,KERN_PROCARGS2,pid};int rc=sysctl(mib,3,data,&n,NULL,0);int result=rc?((errno==EPERM||errno==EACCES)?0:-1):has_environment(data,n);(void)write(channel[1],&result,sizeof result);_exit(0);}
    close(channel[1]);bool ok=false;
    if(child>0){struct pollfd p={channel[0],POLLIN,0};int result=-1,status=0;int ready=poll(&p,1,1000);if(ready>0&&read(channel[0],&result,sizeof result)==sizeof result&&result==0)ok=true;if(!ok)kill(child,SIGKILL);while(waitpid(child,&status,0)<0&&errno==EINTR){}if(!WIFEXITED(status)||WEXITSTATUS(status))ok=false;}
    close(channel[0]);free(data);if(!ok)return CXF_ERR_DEP;atomic_store(&protected_pid,pid);return CXF_OK;
#endif
}
static bool emit(AgentSandboxJob *j,const char *fmt,...) {
    va_list args;va_start(args,fmt);int n=vsnprintf(j->profile+j->used,sizeof j->profile-j->used,fmt,args);va_end(args);
    if(n<0||(uint32_t)n>=sizeof j->profile-j->used)return false;j->used+=(uint32_t)n;return true;
}
static bool quoted(AgentSandboxJob *j,const char *v) {
    if(!emit(j,"\""))return false;
    for(const unsigned char *p=(const unsigned char *)v;*p;p++){
        if(*p<32||*p==127)return false;
        if((*p=='"'||*p=='\\')&&!emit(j,"\\"))return false;
        if(!emit(j,"%c",*p))return false;
    }return emit(j,"\"");
}
static bool rule(AgentSandboxJob *j,const char *op,const char *filter,const char *path){return emit(j,"(%s (%s ",op,filter)&&quoted(j,path)&&emit(j,"))\n");}
static bool canonical(const char *path,char *out){char *v=realpath(path,NULL);if(!v)return false;size_t n=strlen(v);if(n>=AGENT_POLICY_PATH_CAP){free(v);return false;}memcpy(out,v,n+1);free(v);return true;}
static bool binding_dir(const AgentPolicy *policy,const char *path,char *out) {
    if(!path||path[0]!='/')return false;
    for(const unsigned char *p=(const unsigned char *)path;*p;p++)if(*p<32||*p==127||*p==':')return false;
    struct stat st;if(!canonical(path,out)||stat(out,&st)||!S_ISDIR(st.st_mode))return false;
    for(const unsigned char *p=(const unsigned char *)out;*p;p++)if(*p<32||*p==127||*p==':')return false;
    return agent_policy_path(policy,out,".",1,true);
}
static bool bind_environment(AgentSandboxJob *j,const TestSandbox *cfg) {
    if(!cfg->toolchain_bin&&!cfg->sdkroot)return true;
    char bin[AGENT_POLICY_PATH_CAP],sdk[AGENT_POLICY_PATH_CAP];
    if(!binding_dir(cfg->policy,cfg->toolchain_bin,bin)||!binding_dir(cfg->policy,cfg->sdkroot,sdk))return false;
    snprintf(j->pathenv,sizeof j->pathenv,"PATH=%s:/usr/bin:/bin:/usr/sbin:/sbin",bin);
    snprintf(j->sdkenv,sizeof j->sdkenv,"SDKROOT=%s",sdk);return true;
}
static bool child_environment(AgentSandboxJob *j,char *const *env,bool isolated) {
    unsigned n=0;bool bound=j->pathenv[0]!=0;
    for(unsigned i=0;env[i];i++) {
        if(bound&&(!strncmp(env[i],"PATH=",5)||!strncmp(env[i],"SDKROOT=",8)))continue;
        if(isolated&&(!strncmp(env[i],"TMPDIR=",7)||!strncmp(env[i],"HOME=",5)||!strncmp(env[i],"CLANG_MODULE_CACHE_PATH=",24)||!strncmp(env[i],"DYLD_",5)||!strncmp(env[i],"LD_",3)))continue;
        unsigned reserve=(isolated?3u:0u)+(bound?2u:0u);
        if(n>=511u-reserve)return false;j->env[n++]=env[i];
    }
    if(isolated){
        snprintf(j->tmpenv,sizeof j->tmpenv,"TMPDIR=%s/",j->tmp);snprintf(j->homeenv,sizeof j->homeenv,"HOME=%s",j->tmp);snprintf(j->cacheenv,sizeof j->cacheenv,"CLANG_MODULE_CACHE_PATH=%s/modules",j->tmp);
        j->env[n++]=j->tmpenv;j->env[n++]=j->homeenv;j->env[n++]=j->cacheenv;
    }
    if(bound){j->env[n++]=j->pathenv;j->env[n++]=j->sdkenv;}j->env[n]=NULL;return true;
}
static bool lock_parents(AgentSandboxJob *j,const char *path) {
    char copy[AGENT_POLICY_PATH_CAP];if(strlen(path)>=sizeof copy)return false;strcpy(copy,path);
    while(strrchr(copy,'/')){char *p=strrchr(copy,'/');if(p==copy)break;*p=0;if(!rule(j,"deny file-write-unlink","literal",copy))return false;}
    return true;
}
static bool scan(AgentSandboxJob *j,const AgentPolicy *p,int fd,const char *full,unsigned depth) {
    if(depth>64)return false;int copy=dup(fd);if(copy<0)return false;DIR *d=fdopendir(copy);if(!d){close(copy);return false;}
    bool ok=true;struct dirent *e;errno=0;
    while((e=readdir(d))) {
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        if(++j->entries>65536){ok=false;break;}
        char path[AGENT_POLICY_PATH_CAP];int n=snprintf(path,sizeof path,"%s/%s",full,e->d_name);struct stat st;
        if(n<0||n>=(int)sizeof path||fstatat(fd,e->d_name,&st,AT_SYMLINK_NOFOLLOW)){ok=false;break;}
        /* Names are checked again by the kernel regex, including future files. */
        if(!agent_policy_path(NULL,full,e->d_name,(uint32_t)strlen(e->d_name),S_ISDIR(st.st_mode))){errno=0;continue;}
        if(!agent_policy_stat(p,&st)){
            ok=rule(j,"deny file-read* file-write* file-map-executable","subpath",path)&&lock_parents(j,path);
            if(!ok)break;errno=0;continue;
        }
        if(S_ISDIR(st.st_mode)) {
            int sub=openat(fd,e->d_name,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
            if(sub<0){ok=false;break;}ok=scan(j,p,sub,path,depth+1);close(sub);if(!ok)break;
        }
        errno=0;
    }
    if(errno)ok=false;closedir(d);return ok;
}
static bool remove_contents(int fd,unsigned depth,uint32_t *count){
    if(depth>64)return false;DIR *d=fdopendir(dup(fd));if(!d)return false;bool ok=true;struct dirent *e;
    while((e=readdir(d))){if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;if(++*count>65536){ok=false;break;}
        struct stat st;if(fstatat(fd,e->d_name,&st,AT_SYMLINK_NOFOLLOW)){ok=false;break;}
        if(S_ISDIR(st.st_mode)){int sub=openat(fd,e->d_name,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(sub<0){ok=false;break;}bool done=remove_contents(sub,depth+1,count);close(sub);if(!done||unlinkat(fd,e->d_name,AT_REMOVEDIR)){ok=false;break;}}
        else if(unlinkat(fd,e->d_name,0)){ok=false;break;}
    }closedir(d);return ok;
}
bool agent_sandbox_cleanup(AgentSandboxJob *j){
    if(!j->tmp[0])return true;int fd=open(j->tmp,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return false;
    uint32_t count=0;bool ok=remove_contents(fd,0,&count);close(fd);if(ok)ok=rmdir(j->tmp)==0;if(ok)j->tmp[0]=0;return ok;
}
cxf_result agent_sandbox_prepare(AgentSandboxJob *j,const TestSandbox *cfg,const char *root,const char *command,char *const *env) {
    TestSandbox defaults={0};if(!cfg)cfg=&defaults;memset(j,0,sizeof *j);
    if(cfg->mode>TEST_SANDBOX_OFF||(cfg->net!=TEST_NET_DENY&&cfg->net!=TEST_NET_ALLOW)||cfg->reads>4||cfg->writes>4)return CXF_ERR_RANGE;
    if(!bind_environment(j,cfg))return CXF_ERR_RANGE;
    j->executable="/bin/sh";j->argv[0]="sh";j->argv[1]="-c";j->argv[2]=(char *)command;
    if(cfg->mode==TEST_SANDBOX_OFF)return child_environment(j,env,false)?CXF_OK:CXF_ERR_RANGE;
#ifndef __APPLE__
    (void)root;return CXF_ERR_DEP;
#else
    cxf_result host=agent_sandbox_host_protect();if(host!=CXF_OK)return host;
    if(access("/usr/bin/sandbox-exec",X_OK))return CXF_ERR_DEP;
    if(!agent_policy_path(cfg->policy,root,".",1,true))return CXF_ERR_RANGE;
    char real[AGENT_POLICY_PATH_CAP];if(!canonical(root,real))return CXF_ERR_RANGE;
    strcpy(j->tmp,"/private/tmp/cxf-exec-XXXXXX");if(!mkdtemp(j->tmp))return CXF_ERR_IO;
#define REQUIRE(v) do{if(!(v)){agent_sandbox_cleanup(j);return CXF_ERR_RANGE;}}while(0)
    REQUIRE(emit(j,"(version 1)\n(deny default)\n(allow process-exec process-fork file-read-metadata)\n"));
    REQUIRE(emit(j,"(allow sysctl-read (sysctl-name-prefix \"hw.\") (sysctl-name \"kern.argmax\") (sysctl-name \"kern.ostype\") (sysctl-name \"kern.osrelease\") (sysctl-name \"kern.osversion\") (sysctl-name \"kern.version\") (sysctl-name \"kern.maxfilesperproc\"))\n"));
    const char *system[]={"/usr/bin","/usr/lib","/usr/libexec","/usr/share","/bin","/sbin","/System/Library","/System/Volumes/Preboot/Cryptexes/OS/System/Library","/System/Volumes/Preboot/Cryptexes/OS/usr/lib","/Library/Developer/CommandLineTools","/Applications/Xcode.app/Contents",NULL};
    REQUIRE(rule(j,"allow file-read* file-map-executable","literal","/"));
    for(unsigned i=0;system[i];i++)REQUIRE(rule(j,"allow file-read* file-map-executable","subpath",system[i]));
    REQUIRE(rule(j,"allow file-read*","literal","/Library/Preferences/com.apple.dt.Xcode.plist"));
    const char *dev[]={"/dev/null","/dev/zero","/dev/random","/dev/urandom",NULL};
    for(unsigned i=0;dev[i];i++)REQUIRE(rule(j,"allow file-read* file-write*","literal",dev[i]));
    REQUIRE(rule(j,"allow file-read* file-map-executable","subpath",real));
    REQUIRE(rule(j,"allow file-read* file-write* file-map-executable","subpath",j->tmp));
    REQUIRE(rule(j,"deny file-write-unlink","literal",j->tmp));
    char grants[9][AGENT_POLICY_PATH_CAP];strcpy(grants[0],real);unsigned total=1;
    for(unsigned kind=0;kind<2;kind++)for(unsigned i=0;i<(kind?cfg->writes:cfg->reads);i++) {
        const char *path=kind?cfg->write[i]:cfg->read[i];
        REQUIRE(path&&canonical(path,grants[total])&&agent_policy_path(cfg->policy,grants[total],".",1,true));
        REQUIRE(rule(j,kind?"allow file-read* file-write* file-map-executable":"allow file-read* file-map-executable","subpath",grants[total]));total++;
    }
    if(cfg->net==TEST_NET_ALLOW)REQUIRE(emit(j,"(allow network*)\n"));
    /* Denials follow all grants: grants never relax protected names/paths. */
    char regex[512];for(uint32_t i=0;i<agent_policy_sensitive_patterns();i++) {
        REQUIRE(agent_policy_sensitive_regex(i,regex,sizeof regex));
        REQUIRE(emit(j,"(deny file-read* file-write* file-map-executable (regex #\"%s\"))\n",regex));
    }
    REQUIRE(emit(j,"(deny file-link)\n"));
    if(cfg->policy)for(unsigned i=0;i<cfg->policy->files;i++) {
        REQUIRE(rule(j,"deny file-read* file-write* file-map-executable","subpath",cfg->policy->file[i].path));
        REQUIRE(lock_parents(j,cfg->policy->file[i].path));
    }
    for(unsigned i=0;i<total;i++) {
        int fd=open(grants[i],O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);REQUIRE(fd>=0);
        bool ok=scan(j,cfg->policy,fd,grants[i],0);close(fd);REQUIRE(ok);
    }
    REQUIRE(child_environment(j,env,true));
    j->executable="/usr/bin/sandbox-exec";j->argv[0]="sandbox-exec";j->argv[1]="-p";j->argv[2]=j->profile;j->argv[3]="/bin/sh";j->argv[4]="-c";j->argv[5]=(char *)command;return CXF_OK;
#undef REQUIRE
#endif
}
