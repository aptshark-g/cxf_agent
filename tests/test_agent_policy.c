#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "agent_policy.h"
#include "agent_basic.h"
#include "agent_fs.h"
static unsigned checks;
#define CHECK(v) do { checks++; if(!(v)){fprintf(stderr,"policy check failed at line %d\n",__LINE__);abort();} } while(0)
static void put(const char *root,const char *name){char path[1200];snprintf(path,sizeof path,"%s/%s",root,name);FILE *f=fopen(path,"wb");CHECK(f);CHECK(fputs("ordinary old line\n",f)>=0);CHECK(!fclose(f));}
int main(void){
    char root[]="/tmp/cxf-policy-unit-XXXXXX";CHECK(mkdtemp(root));
    AgentPolicy policy={0};BasicFsCfg basic={root,&policy};FsRootCfg write={root,&policy};
    const char *names[]={".env",".env.example",".ENV.local","service.key","cert.PEM","identity.p12","identity.pfx","store.jks","ID_RSA.pub","id_ed25519","key","keys.json","credentials.yml","secrets.toml","agentctl.json","agentctl.local.json","app.local.yaml",".npmrc",".netrc",".pypirc",".git-credentials",".aws",".ssh",".codex",".agents",".workbuddy"};
    char out[65536],args[1024],path[1200];uint32_t n;
    for(unsigned i=0;i<sizeof names/sizeof names[0];i++){
        put(root,names[i]);
        CHECK(!agent_policy_path(NULL,root,names[i],(uint32_t)strlen(names[i]),false));
        CHECK(basic_read_exec(&basic,names[i],(uint32_t)strlen(names[i]),out,sizeof out,&n)!=CXF_OK);
        CHECK(basic_list_exec(&basic,names[i],(uint32_t)strlen(names[i]),out,sizeof out,&n)!=CXF_OK);
        snprintf(args,sizeof args,"%s\nreplacement\n",names[i]);
        CHECK(fs_diff_exec(&write,args,(uint32_t)strlen(args),out,sizeof out,&n)!=CXF_OK);
        CHECK(fs_write_new_exec(&write,args,(uint32_t)strlen(args),out,sizeof out,&n)!=CXF_OK);
        CHECK(fs_write_apply_exec(&write,args,(uint32_t)strlen(args),out,sizeof out,&n)!=CXF_OK);
        CHECK(fs_append_exec(&write,args,(uint32_t)strlen(args),out,sizeof out,&n)!=CXF_OK);
        snprintf(args,sizeof args,"%s\nordinary old line\nreplacement",names[i]);
        CHECK(basic_edit_exec(&basic,args,(uint32_t)strlen(args),out,sizeof out,&n)!=CXF_OK);
        snprintf(path,sizeof path,"%s/%s",root,names[i]);FILE *f=fopen(path,"r");CHECK(f);CHECK(fgets(out,sizeof out,f)&&!strcmp(out,"ordinary old line\n"));fclose(f);CHECK(!unlink(path));
    }
    const char *bad[]={"../a","a/../b","a/./b","a//b","/tmp/a","a/","a\rb",".git/a","dir/.env.prod"};
    for(unsigned i=0;i<sizeof bad/sizeof bad[0];i++)CHECK(!agent_policy_path(NULL,root,bad[i],(uint32_t)strlen(bad[i]),false));
    const char *good[]={"monkey.txt","keyboard.c","agentctl.example.json","ordinary.txt"};
    for(unsigned i=0;i<sizeof good/sizeof good[0];i++)CHECK(agent_policy_path(NULL,root,good[i],(uint32_t)strlen(good[i]),false));
    put(root,"ordinary.txt");char alias[1200];snprintf(path,sizeof path,"%s/ordinary.txt",root);snprintf(alias,sizeof alias,"%s/alias",root);
    CHECK(!symlink(path,alias));CHECK(agent_policy_open(&policy,root,"alias",5,false,O_RDONLY)<0);CHECK(!unlink(alias));
    CHECK(!link(path,alias));CHECK(agent_policy_open(&policy,root,"alias",5,false,O_RDONLY)<0);CHECK(agent_policy_open(&policy,root,"ordinary.txt",12,false,O_RDONLY)<0);CHECK(!unlink(alias));
    CHECK(agent_policy_protect(&policy,path));CHECK(agent_policy_open(&policy,root,"ordinary.txt",12,false,O_RDONLY)<0);
    CHECK(basic_list_exec(&basic,".",1,out,sizeof out,&n)==CXF_OK);out[n]=0;CHECK(!strstr(out,"ordinary.txt"));
    FsCfg grep={root,0,0,&policy};CHECK(fs_grep_exec(&grep,"ordinary",8,out,sizeof out,&n)==CXF_OK);CHECK(n==0);CHECK(!unlink(path));
    CHECK(agent_policy_secret(&policy,"synthetic-value-42"));
    const char *leaks[]={"synthetic-value-42","c3ludGhldGljLXZhbHVlLTQy","73796e7468657469632d76616c75652d3432","73796E7468657469632D76616C75652D3432","api_key=long-value","password: value","-----BEGIN " "PRIVATE KEY-----","sk-" "synthetic012345678901234567890"};
    for(unsigned i=0;i<sizeof leaks/sizeof leaks[0];i++)CHECK(!agent_policy_text(&policy,leaks[i],(uint32_t)strlen(leaks[i])));
    CHECK(agent_policy_text(&policy,"normal api_key documentation",28));CHECK(agent_policy_text(&policy,"api_key=YOUR_VALUE",18));
    CHECK(!agent_policy_secret(&policy,"x"));
    CHECK(!agent_policy_egress(&policy,0,AGENT_DATA_TASK,"clean",5));
    CHECK(agent_policy_egress(&policy,AGENT_SCOPE_LOCAL,AGENT_DATA_WORKSPACE,"clean",5));
    policy.cloud_egress=AGENT_EGRESS_TASK;
    CHECK(agent_policy_egress(&policy,AGENT_SCOPE_CLOUD,AGENT_DATA_TASK,"clean",5));
    CHECK(!agent_policy_egress(&policy,AGENT_SCOPE_CLOUD,AGENT_DATA_WORKSPACE,"clean",5));
    policy.cloud_egress=AGENT_EGRESS_WORKSPACE;CHECK(agent_policy_egress(&policy,0,0,"clean",5));
    CHECK(!agent_policy_egress(&policy,1,0,leaks[0],(uint32_t)strlen(leaks[0])));
    CHECK(agent_policy_loopback("127.0.0.2"));CHECK(agent_policy_loopback("localhost"));CHECK(!agent_policy_loopback("localhost.example"));CHECK(!agent_policy_loopback("192.0.2.1"));
    for(unsigned i=policy.secrets;i<AGENT_POLICY_SECRETS;i++){snprintf(args,sizeof args,"unique-synthetic-%u",i);CHECK(agent_policy_secret(&policy,args));}
    CHECK(!agent_policy_secret(&policy,"one-more-synthetic"));
    for(unsigned i=policy.files;i<AGENT_POLICY_PATHS;i++) {
        snprintf(path,sizeof path,"%s/reserved-%u",root,i);CHECK(agent_policy_protect(&policy,path));
    }
    snprintf(path,sizeof path,"%s/overflow",root);CHECK(!agent_policy_protect(&policy,path));
    snprintf(path,sizeof path,"%s/.ssh",root);CHECK(!mkdir(path,0700));
    CHECK(!agent_policy_path(NULL,path,"ordinary",8,false));CHECK(!rmdir(path));
    CHECK(!rmdir(root));printf("agent data policy: %u checks passed\n",checks);return 0;
}
