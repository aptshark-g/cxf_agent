#define _DARWIN_C_SOURCE 1
#include <stdio.h>
#include <string.h>
#include "agent_sandbox.h"
int main(int argc,char **argv){
    if(argc!=2)return 2;AgentSandboxJob job;char *env[]={"PATH=/usr/bin:/bin",NULL};
    if(agent_sandbox_prepare(&job,NULL,argv[1],"true",env)!=CXF_OK)return 3;
    const char *p=job.profile;while((p=strstr(p,"(regex #\""))){p+=9;const char *end=strchr(p,'"');if(!end)return 4;printf("%.*s\n",(int)(end-p),p);p=end+1;}
    return agent_sandbox_cleanup(&job)?0:5;
}
