#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#else
#define _POSIX_C_SOURCE 200809L
#endif
#include <assert.h>
#include <fcntl.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "agent_basic.h"
static unsigned checks;
#define CHECK(v) do{checks++;if(!(v)){fprintf(stderr,"sandbox check failed line %d\n",__LINE__);abort();}}while(0)
int main(void){
    regex_t rx[128];unsigned count=agent_policy_sensitive_patterns();CHECK(count<128);
    for(unsigned i=0;i<count;i++){char s[512];CHECK(agent_policy_sensitive_regex(i,s,sizeof s));CHECK(!regcomp(&rx[i],s,REG_EXTENDED|REG_NOSUB));}
    const char *bad[]={".env",".env.example","dir/.ENV.LOCAL","server.pem","key","keys.yml","secret.txt","credentials.ini","ID_RSA.pub",".ssh/a",".git/config","agentctl.json","a.local.json",".workbuddy/config"};
    const char *good[]={"ordinary.c","monkey.txt","keyboard.c","agentctl.example.json","dir/keynote.txt"};
    for(unsigned i=0;i<sizeof bad/sizeof bad[0];i++){bool hit=false;for(unsigned n=0;n<count;n++)if(!regexec(&rx[n],bad[i],0,NULL,0))hit=true;CHECK(hit);}
    for(unsigned i=0;i<sizeof good/sizeof good[0];i++)for(unsigned n=0;n<count;n++)CHECK(regexec(&rx[n],good[i],0,NULL,0)!=0);
    for(unsigned i=0;i<count;i++)regfree(&rx[i]);
    char root[]="/tmp/cxf-sandbox-unit-XXXXXX";CHECK(mkdtemp(root));char path[1024];snprintf(path,sizeof path,"%s/.env",root);
    int fd=open(path,O_RDWR|O_CREAT|O_EXCL,0600);CHECK(fd>=0);CHECK(write(fd,"FD_PRIVATE_DATA",15)==15);CHECK(lseek(fd,0,SEEK_SET)==0);
    char command[256],out[BASIC_OUT_HINT];uint32_t size;TestRunTrace trace;
    snprintf(command,sizeof command,"cat <&%d",fd);TestRunCfg cfg={root,command,1000,1,NULL};
    for(TestRunBackend b=TEST_RUN_FORK;b<=TEST_RUN_SPAWN;b++){
        cxf_result rc=test_run_exec_observed(&cfg,"",0,out,sizeof out,&size,&trace,b,NULL,NULL);
        if(rc==CXF_ERR_DEP)continue;CHECK(rc==CXF_OK);out[size]=0;CHECK(!strstr(out,"FD_PRIVATE_DATA"));CHECK(!strstr(out,"exit=0 "));
    }
    close(fd);CHECK(!unlink(path));
    AgentSandboxJob job;char *env[]={"PATH=/usr/bin:/bin","LD_PRELOAD=unsafe","DYLD_TEST_MARKER=unsafe",NULL};
    TestSandbox sandbox={0};cxf_result rc=agent_sandbox_prepare(&job,&sandbox,root,"true",env);
#ifdef __APPLE__
    CHECK(rc==CXF_OK);CHECK(access(job.tmp,F_OK)==0);for(unsigned i=0;job.env[i];i++)CHECK(strncmp(job.env[i],"LD_",3)&&strncmp(job.env[i],"DYLD_",5));
    char tmp[1024];strcpy(tmp,job.tmp);CHECK(agent_sandbox_cleanup(&job));CHECK(access(tmp,F_OK)!=0);
#else
    CHECK(rc==CXF_ERR_DEP);
#endif
    sandbox.reads=5;CHECK(agent_sandbox_prepare(&job,&sandbox,root,"touch owned",env)==CXF_ERR_RANGE);
    sandbox.reads=0;sandbox.net=TEST_NET_LOOPBACK_UNSUPPORTED;CHECK(agent_sandbox_prepare(&job,&sandbox,root,"touch owned",env)==CXF_ERR_RANGE);sandbox.net=0;sandbox.mode=99;CHECK(agent_sandbox_prepare(&job,&sandbox,root,"touch owned",env)==CXF_ERR_RANGE);
    sandbox=(TestSandbox){.mode=TEST_SANDBOX_OFF,.toolchain_bin=root};
    CHECK(agent_sandbox_prepare(&job,&sandbox,root,"true",env)==CXF_ERR_RANGE);
    sandbox.sdkroot=root;char *bound_env[]={"PATH=/bad","SDKROOT=/bad","KEEP=value",NULL};
    CHECK(agent_sandbox_prepare(&job,&sandbox,root,"true",bound_env)==CXF_OK);
    unsigned paths=0,sdks=0,kept=0;
    for(unsigned i=0;job.env[i];i++){if(!strncmp(job.env[i],"PATH=",5)){paths++;CHECK(!strcmp(job.env[i],job.pathenv));CHECK(strstr(job.env[i],root));}if(!strncmp(job.env[i],"SDKROOT=",8)){sdks++;CHECK(!strcmp(job.env[i],job.sdkenv));CHECK(strstr(job.env[i],root));}if(!strcmp(job.env[i],"KEEP=value"))kept++;}
    CHECK(paths==1&&sdks==1&&kept==1);CHECK(!job.tmp[0]);
    sandbox.sdkroot="relative";CHECK(agent_sandbox_prepare(&job,&sandbox,root,"true",env)==CXF_ERR_RANGE);
    sandbox.sdkroot="/tmp/invalid:path";CHECK(agent_sandbox_prepare(&job,&sandbox,root,"true",env)==CXF_ERR_RANGE);
    sandbox.sdkroot="/tmp/invalid\npath";CHECK(agent_sandbox_prepare(&job,&sandbox,root,"true",env)==CXF_ERR_RANGE);
    sandbox.sdkroot="/dev/null";CHECK(agent_sandbox_prepare(&job,&sandbox,root,"true",env)==CXF_ERR_RANGE);
    sandbox.sdkroot=root;char *large_env[513];for(unsigned i=0;i<512;i++)large_env[i]="KEEP=value";large_env[512]=NULL;
    CHECK(agent_sandbox_prepare(&job,&sandbox,root,"true",large_env)==CXF_ERR_RANGE);
#ifdef __APPLE__
    sandbox.mode=TEST_SANDBOX_ON;CHECK(agent_sandbox_prepare(&job,&sandbox,root,"true",bound_env)==CXF_OK);
    CHECK(strstr(job.profile,"(deny default)"));CHECK(!strstr(job.profile,"allow network"));CHECK(agent_sandbox_cleanup(&job));
    CHECK(agent_sandbox_prepare(&job,&sandbox,root,"true",large_env)==CXF_ERR_RANGE);CHECK(!job.tmp[0]);
#endif
    CHECK(!rmdir(root));printf("test.run sandbox C: %u checks passed\n",checks);return 0;
}
