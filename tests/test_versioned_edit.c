#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "agent_basic.h"
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

static char root[1024],out[BASIC_OUT_HINT+1];static uint32_t n;static unsigned cases;
static BasicFsCfg cfg;static AgentPolicy policy;
static void path(char *p,const char *name){snprintf(p,1200,"%s/%s",root,name);}
static void put(const char *name,const char *text){char p[1200];path(p,name);FILE *f=fopen(p,"w");assert(f);assert(fputs(text,f)>=0);assert(!fclose(f));}
static void bytes(const char *name,const char *expected){char p[1200],buf[70000];path(p,name);FILE *f=fopen(p,"rb");assert(f);size_t got=fread(buf,1,sizeof buf,f);assert(!ferror(f));fclose(f);assert(got==strlen(expected)&&!memcmp(buf,expected,got));}
static void clean(void){DIR *d=opendir(root);assert(d);struct dirent *e;while((e=readdir(d)))assert(strncmp(e->d_name,".cxf-patch-",11));closedir(d);}
static void pass(const char *name){clean();cases++;printf("{\"kind\":\"unit\",\"case\":\"%s\",\"passed\":true}\n",name);}
static void version(const char *name,char *token){assert(basic_read_exec(&cfg,name,(uint32_t)strlen(name),out,sizeof out,&n)==CXF_OK);out[n]=0;char *p=strstr(out," version=");assert(p);p+=9;char *end=strchr(p,' ');assert(end&&end-p<BASIC_VERSION_CAP);memcpy(token,p,(size_t)(end-p));token[end-p]=0;}
static void frame(char *args,size_t cap,const char *name,const char *token,const char *old,const char *replacement){int h=snprintf(args,cap,"%s\n%s\n<<<<<<< SEARCH\n%s\n=======\n%s\n>>>>>>> REPLACE",name,token,old,replacement);assert(h>0&&(size_t)h<cap);}
static cxf_result edit(const char *args){n=0;cxf_result rc=basic_patch_exec(&cfg,args,(uint32_t)strlen(args),out,sizeof out,&n);out[n]=0;return rc;}
static void normal(const char *label,const char *initial,const char *old,const char *replacement,const char *expected){char token[BASIC_VERSION_CAP],args[1600];put("a.txt",initial);version("a.txt",token);frame(args,sizeof args,"a.txt",token,old,replacement);assert(edit(args)==CXF_OK&&strstr(out,"status=applied"));bytes("a.txt",expected);assert(!strstr(out,"version=-"));pass(label);}
struct Fault {int stage,count;const char *external;};
static int fault(int stage,void *ctx){struct Fault *f=ctx;if(stage!=f->stage)return 0;if(f->external){put("a.txt",f->external);return 0;}if(stage==11)return f->count++?1:-1;return 1;}
int main(void){
    char temp[]="/tmp/cxf-versioned-edit-XXXXXX";assert(mkdtemp(temp));strcpy(root,temp);cfg=(BasicFsCfg){root,&policy};
    normal("multiline_lf","head\nold one\nold two\ntail\n","old one\nold two","new one\nnew two","head\nnew one\nnew two\ntail\n");
    normal("crlf_exact","head\r\nold one\r\nold two\r\ntail\r\n","old one\r\nold two","new one\r\nnew two","head\r\nnew one\r\nnew two\r\ntail\r\n");
    normal("no_final_newline","head\nold\ntail","old\ntail","new\nend","head\nnew\nend");
    normal("unicode","前\n旧\n尾\n","前\n旧","新\n中","新\n中\n尾\n");
    normal("delete_block_with_lf","a\nb\nc\n","b\n","","a\nc\n");
    char token[BASIC_VERSION_CAP],args[1600],p[1200],alias[1200];
    put("a.txt","one\ntwo\n");version("a.txt",token);frame(args,sizeof args,"a.txt",token,"one\ntwo","one\ntwo");assert(edit(args)==CXF_OK&&strstr(out,"status=same"));bytes("a.txt","one\ntwo\n");pass("same_no_write");
    frame(args,sizeof args,"a.txt",token,"missing","new");assert(edit(args)==CXF_OK&&strstr(out,"status=not_found"));bytes("a.txt","one\ntwo\n");pass("not_found_no_write");
    put("a.txt","repeat repeat repeat");version("a.txt",token);frame(args,sizeof args,"a.txt",token,"repeat","new");assert(edit(args)==CXF_OK&&strstr(out,"status=ambiguous")&&strstr(out,"matches=2"));bytes("a.txt","repeat repeat repeat");pass("ambiguous_capped");
    put("a.txt","old\n");version("a.txt",token);frame(args,sizeof args,"a.txt",token,"old","new");put("a.txt","external\n");assert(edit(args)==CXF_OK&&strstr(out,"reason=stale"));bytes("a.txt","external\n");pass("stale_inplace");
    put("a.txt","old\n");version("a.txt",token);frame(args,sizeof args,"a.txt",token,"old","new");put("b.txt","replacement\n");path(p,"a.txt");path(alias,"b.txt");assert(!rename(alias,p));assert(edit(args)==CXF_OK&&strstr(out,"reason=stale"));bytes("a.txt","replacement\n");pass("stale_replaced_inode");
    put("a.txt","old\n");path(p,"a.txt");assert(!chmod(p,0644));version("a.txt",token);frame(args,sizeof args,"a.txt",token,"old","new");assert(!chmod(p,0600));assert(edit(args)==CXF_OK&&strstr(out,"status=conflict"));bytes("a.txt","old\n");pass("stale_permission");
    version("a.txt",token);frame(args,sizeof args,"a.txt",token,"old","new");int fd=open(p,O_RDONLY);assert(fd>=0&&!flock(fd,LOCK_EX|LOCK_NB));assert(edit(args)==CXF_OK&&strstr(out,"reason=busy"));close(fd);bytes("a.txt","old\n");pass("busy_nonblocking");
    for(int stage=1;stage<=3;stage++){
        put("a.txt","old\n");version("a.txt",token);frame(args,sizeof args,"a.txt",token,"old","new");struct Fault f={stage,0,"external during edit\n"};basic_patch_test_hook(fault,&f);
        assert(edit(args)==CXF_OK&&strstr(out,"reason=changed"));basic_patch_test_hook(NULL,NULL);bytes("a.txt",f.external);
        char label[60];snprintf(label,sizeof label,"external_mutation_stage_%d",stage);pass(label);
    }
    for(int stage=10;stage<=15;stage++){
        put("a.txt","old\n");version("a.txt",token);frame(args,sizeof args,"a.txt",token,"old","new");struct Fault f={stage,0,NULL};basic_patch_test_hook(fault,&f);
        assert(edit(args)!=CXF_OK&&n==0);basic_patch_test_hook(NULL,NULL);bytes("a.txt","old\n");struct stat st;assert(!stat(p,&st)&&(st.st_mode&0777)==0600);
        char label[60];snprintf(label,sizeof label,"injected_precommit_failure_%d",stage);pass(label);
    }
    if(geteuid()!=0){assert(!chmod(root,0500));assert(edit(args)!=CXF_OK&&n==0);assert(!chmod(root,0700));bytes("a.txt","old\n");pass("real_create_permission_error");}
    version("a.txt",token);frame(args,sizeof args,"a.txt",token,"old","new");assert(basic_patch_exec(&cfg,args,(uint32_t)strlen(args),out,1,&n)==CXF_ERR_RANGE&&!n);bytes("a.txt","old\n");pass("small_receipt_before_io");
    const char *bad[]={"a.txt\nv1:0\n<<<<<<< SEARCH\n\n=======\nx\n>>>>>>> REPLACE","a.txt\nv1:0\n<<<<<<< SEARCH\nold\n=======\nnew\n>>>>>>> REPLACE\ntrailing","a.txt\nv1:0\n<<<<<<< SEARCH\nold\n=======\nx\n=======\ny\n>>>>>>> REPLACE"};
    for(unsigned i=0;i<sizeof bad/sizeof bad[0];i++)assert(edit(bad[i])==CXF_ERR_FORMAT&&!n);bytes("a.txt","old\n");pass("malformed_frames");
    const char *protected[]={".env","agentctl.json","key","keys.json","service.pem","app.local.yaml","../outside","/etc/passwd","a/../b","a//b"};
    for(unsigned i=0;i<sizeof protected/sizeof protected[0];i++){frame(args,sizeof args,protected[i],"v1:0","old","new");assert(edit(args)!=CXF_OK&&!n);}pass("protected_names_and_traversal");
    path(alias,"alias");assert(!symlink("a.txt",alias));frame(args,sizeof args,"alias",token,"old","new");assert(edit(args)!=CXF_OK&&!n);assert(!unlink(alias));bytes("a.txt","old\n");pass("symlink_denied");
    assert(!link(p,alias));frame(args,sizeof args,"a.txt",token,"old","new");assert(edit(args)!=CXF_OK&&!n);assert(!unlink(alias));pass("hardlink_denied");
    AgentPolicy guarded={0};assert(agent_policy_protect(&guarded,p));cfg.policy=&guarded;assert(edit(args)!=CXF_OK&&!n);cfg.policy=&policy;bytes("a.txt","old\n");pass("registered_config_inode");
    assert(agent_policy_secret(&policy,"secret-marker"));put("a.txt","secXret-marker\n");version("a.txt",token);frame(args,sizeof args,"a.txt",token,"X","");assert(edit(args)==CXF_ERR_RANGE&&!n);bytes("a.txt","secXret-marker\n");pass("assembled_secret_denied");
    put("a.txt","old\n");version("a.txt",token);frame(args,sizeof args,"a.txt",token,"old","secret-marker");assert(edit(args)==CXF_ERR_RANGE&&!n);bytes("a.txt","old\n");pass("input_secret_denied");
    char large[BASIC_EDIT_BYTES+2];memset(large,'a',BASIC_EDIT_BYTES);large[BASIC_EDIT_BYTES]=0;put("a.txt",large);version("a.txt",token);frame(args,sizeof args,"a.txt",token,"aaa","aaaa");assert(edit(args)==CXF_OK&&strstr(out,"status=ambiguous"));pass("large_ambiguous_no_write");
    large[0]='X';put("a.txt",large);version("a.txt",token);frame(args,sizeof args,"a.txt",token,"X","XX");assert(edit(args)==CXF_ERR_RANGE&&!n);bytes("a.txt",large);pass("result_size_limit");
    memset(args,'a',1025);args[1025]=0;assert(edit(args)==CXF_ERR_FORMAT&&!n);bytes("a.txt",large);pass("argument_limit");
    put("a.txt","old\n");version("a.txt",token);int go[2],result[2];assert(!pipe(go)&&!pipe(result));pid_t kids[2];
    for(int i=0;i<2;i++){kids[i]=fork();assert(kids[i]>=0);if(!kids[i]){close(go[1]);close(result[0]);char b;assert(read(go[0],&b,1)==1);frame(args,sizeof args,"a.txt",token,"old",i?"child B":"child A");assert(edit(args)==CXF_OK);b=strstr(out,"status=applied")?'A':strstr(out,"status=conflict")?'C':'?';assert(write(result[1],&b,1)==1);_exit(0);}}
    close(go[0]);close(result[1]);assert(write(go[1],"xx",2)==2);close(go[1]);char outcomes[2];assert(read(result[0],outcomes,1)==1&&read(result[0],outcomes+1,1)==1);close(result[0]);
    for(int i=0;i<2;i++){int status;assert(waitpid(kids[i],&status,0)==kids[i]&&WIFEXITED(status)&&WEXITSTATUS(status)==0);}assert((outcomes[0]=='A'&&outcomes[1]=='C')||(outcomes[0]=='C'&&outcomes[1]=='A'));pass("two_process_same_version_one_commit");
    assert(!unlink(p));assert(!rmdir(root));printf("{\"kind\":\"closed\",\"cases\":%u,\"passed\":true}\n",cases);
}
