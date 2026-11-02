#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#else
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "agent_policy.h"
static unsigned char lower(unsigned char c){return c>='A'&&c<='Z'?c+32:c;}
static bool equal(const char *a,const char *b){while(*a&&*b&&lower(*a)==lower(*b)){a++;b++;}return !*a&&!*b;}
static bool prefix_equal(const char *s,const char *p){while(*p){if(lower(*s++)!=lower(*p++))return false;}return true;}
static bool suffix(const char *s,const char *end){size_t a=strlen(s),b=strlen(end);return a>=b&&equal(s+a-b,end);}
static const char *names[]={".git",".ssh",".aws",".gnupg",".kube",".azure",".docker",".workbuddy",".codex",".agents",".netrc",".npmrc",".pypirc",".git-credentials","agentctl.json","agentctl.local.json","key","keys","secret","secrets","credentials",NULL};
static const char *ends[]={".pem",".key",".p12",".pfx",".jks",".local.json",".local.yaml",".local.yml",".local.toml",".local.ini",".local.conf",NULL};
static const char *ids[]={"id_rsa","id_dsa","id_ecdsa","id_ed25519",NULL};
static const char *stems[]={"key","keys","secret","secrets","credentials",NULL};
static const char *exts[]={".json",".yaml",".yml",".toml",".ini",".conf",".txt",NULL};
static bool component(const char *s,uint32_t n){
    char v[512];if(!n||n>=sizeof v)return false;
    memcpy(v,s,n);v[n]=0;

    for(unsigned i=0;names[i];i++)if(equal(v,names[i]))return false;
    if(equal(v,".env")||(n>5&&!memcmp(v,".env.",5)))return false;
    /* Case-insensitive environment prefix. */
    if(n>=5&&lower(v[0])=='.'&&lower(v[1])=='e'&&lower(v[2])=='n'&&lower(v[3])=='v'&&v[4]=='.')return false;

    for(unsigned i=0;ends[i];i++)if(suffix(v,ends[i]))return false;

    for(unsigned i=0;ids[i];i++)if(equal(v,ids[i])||(prefix_equal(v,ids[i])&&v[strlen(ids[i])]=='.'))return false;


    for(unsigned i=0;stems[i];i++)for(unsigned j=0;exts[j];j++){
        char full[64];snprintf(full,sizeof full,"%s%s",stems[i],exts[j]);if(equal(v,full))return false;
    }
    return true;
}
static bool components(const char *s,bool absolute,bool dot){
    if(!s||!*s)return false;
    if(dot&&equal(s,"."))return true;
    const char *p=s;if(absolute&&*p=='/')p++;
    if(!*p)return absolute;
    while(*p){const char *start=p;while(*p&&*p!='/'){if((unsigned char)*p<32||*p==127)return false;p++;}
        size_t n=(size_t)(p-start);
        if((n==1&&*start=='.')||(n==2&&start[0]=='.'&&start[1]=='.')||!component(start,(uint32_t)n))return false;
        if(*p&&!*++p)return false;
    }
    return true;
}
static bool canonical(const char *path,char *out,size_t cap){
    char *v=realpath(path,NULL);if(!v)return false;
    size_t n=strlen(v);if(n>=cap){free(v);return false;}
    memcpy(out,v,n+1);free(v);return true;
}
bool agent_policy_protect(AgentPolicy *p,const char *path){
    if(!p||!path||!*path)return false;
    char full[AGENT_POLICY_PATH_CAP],copy[AGENT_POLICY_PATH_CAP],parent[AGENT_POLICY_PATH_CAP];
    if(!canonical(path,full,sizeof full)){
        if(strlen(path)>=sizeof copy)return false;strcpy(copy,path);char *slash=strrchr(copy,'/');
        const char *leaf=slash?slash+1:copy;if(!*leaf)return false;
        if(slash){if(slash==copy){if(!canonical("/",parent,sizeof parent))return false;}else{*slash=0;if(!canonical(copy,parent,sizeof parent))return false;}}
        else if(!canonical(".",parent,sizeof parent))return false;
        if(snprintf(full,sizeof full,"%s/%s",parent,leaf)>=(int)sizeof full)return false;
    }
    unsigned at=0;while(at<p->files&&!equal(p->file[at].path,full))at++;
    if(at==p->files){if(at==AGENT_POLICY_PATHS)return false;p->files++;}
    AgentPolicyFile *f=&p->file[at];strcpy(f->path,full);struct stat st;
    f->has_inode=stat(full,&st)==0;if(f->has_inode){f->dev=st.st_dev;f->inode=st.st_ino;}return true;
}
bool agent_policy_secret(AgentPolicy *p,const char *value){
    if(!p||!value)return false;size_t n=strlen(value);if(!n)return true;if(n<4)return false;
    if(n>=AGENT_POLICY_SECRET_CAP)return false;
    for(unsigned i=0;i<p->secrets;i++)if(!strcmp(p->secret[i],value))return true;
    if(p->secrets==AGENT_POLICY_SECRETS)return false;strcpy(p->secret[p->secrets++],value);return true;
}
bool agent_policy_path(const AgentPolicy *p,const char *root,const char *path,uint32_t n,bool dir){
    char rel[512],real[AGENT_POLICY_PATH_CAP],full[AGENT_POLICY_PATH_CAP];
    if(!root||!path||!n||n>=sizeof rel||path[0]=='/'||memchr(path,0,n))return false;
    memcpy(rel,path,n);rel[n]=0;
    if(!components(rel,false,dir)||!canonical(root,real,sizeof real)||!components(real,true,false))return false;
    int size=snprintf(full,sizeof full,"%s%s%s",real,equal(rel,".")?"":"/",equal(rel,".")?"":rel);
    if(size<0||size>=(int)sizeof full)return false;
    if(p)for(unsigned i=0;i<p->files;i++){
        size_t len=strlen(p->file[i].path);
        if(!strncmp(full,p->file[i].path,len)&&(full[len]==0||full[len]=='/'))return false;
    }
    return true;
}
bool agent_policy_stat(const AgentPolicy *p,const struct stat *st){
    if(S_ISREG(st->st_mode)&&st->st_nlink!=1)return false;
    if(p)for(unsigned i=0;i<p->files;i++)if(p->file[i].has_inode&&p->file[i].dev==(uint64_t)st->st_dev&&p->file[i].inode==(uint64_t)st->st_ino)return false;
    return true;
}
cxf_result agent_policy_parent(const AgentPolicy *p,const char *root,const char *path,uint32_t n,AgentPolicyPath *out){
    out->parent=-1;if(!agent_policy_path(p,root,path,n,false))return CXF_ERR_RANGE;
    int fd=open(root,O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(fd<0)return CXF_ERR_IO;
    uint32_t pos=0;for(;;){uint32_t end=pos;while(end<n&&path[end]!='/')end++;
        char part[512];memcpy(part,path+pos,end-pos);part[end-pos]=0;
        if(end==n){strcpy(out->leaf,part);out->parent=fd;return CXF_OK;}
        int next=openat(fd,part,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);close(fd);
        if(next<0)return errno==ELOOP||errno==ENOTDIR?CXF_ERR_RANGE:CXF_ERR_IO;fd=next;pos=end+1;
    }
}
int agent_policy_open(const AgentPolicy *p,const char *root,const char *path,uint32_t n,bool dir,int flags){
    if(dir&&n==1&&path[0]=='.'){
        if(!agent_policy_path(p,root,path,n,true)){errno=EACCES;return -1;}
        return open(root,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    }
    AgentPolicyPath at;if(agent_policy_parent(p,root,path,n,&at)!=CXF_OK){errno=EACCES;return -1;}
    int fd=openat(at.parent,at.leaf,flags|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK|(dir?O_DIRECTORY:0));close(at.parent);
    if(fd<0)return -1;struct stat st;
    if(fstat(fd,&st)||(!dir&&!S_ISREG(st.st_mode))||!agent_policy_stat(p,&st)){close(fd);errno=EACCES;return -1;}
    return fd;
}
static bool contains(const char *s,uint32_t n,const char *v,size_t m){if(!m||m>n)return false;for(uint32_t i=0;i+m<=n;i++)if(!memcmp(s+i,v,m))return true;return false;}
static bool encoded(const char *s,uint32_t n,const char *v){
    size_t len=strlen(v);if(len<4)return false;
    if(contains(s,n,v,len))return true;
    char hex[AGENT_POLICY_SECRET_CAP*2+1],b64[AGENT_POLICY_SECRET_CAP*2];
    static const char h[]="0123456789abcdef",alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for(size_t i=0;i<len;i++){hex[i*2]=h[(unsigned char)v[i]>>4];hex[i*2+1]=h[(unsigned char)v[i]&15];}
    if(contains(s,n,hex,len*2))return true;
    for(size_t i=0;i<len*2;i++)if(hex[i]>='a'&&hex[i]<='f')hex[i]-=32;
    if(contains(s,n,hex,len*2))return true;
    unsigned w=0;for(size_t i=0;i<len;i+=3){unsigned a=(unsigned char)v[i],b=i+1<len?(unsigned char)v[i+1]:0,c=i+2<len?(unsigned char)v[i+2]:0;
        b64[w++]=alphabet[a>>2];b64[w++]=alphabet[((a&3)<<4)|(b>>4)];b64[w++]=i+1<len?alphabet[((b&15)<<2)|(c>>6)]:'=';b64[w++]=i+2<len?alphabet[c&63]:'=';}
    return contains(s,n,b64,w);
}
static bool tokenchar(unsigned char c){return(c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-';}
bool agent_policy_text(const AgentPolicy *p,const char *s,uint32_t n){
    if(!s&&n)return false;
    if(p)for(unsigned i=0;i<p->secrets;i++){
        if(encoded(s,n,p->secret[i]))return false;
        if(!strncmp(p->secret[i],"Bearer ",7)&&encoded(s,n,p->secret[i]+7))return false;
    }
    const char *marks[]={"-----BEGIN " "PRIVATE KEY-----","-----BEGIN " "RSA PRIVATE KEY-----","-----BEGIN " "EC PRIVATE KEY-----","-----BEGIN " "OPENSSH PRIVATE KEY-----","-----BEGIN " "ENCRYPTED PRIVATE KEY-----",NULL};
    for(unsigned i=0;marks[i];i++)if(contains(s,n,marks[i],strlen(marks[i])))return false;
    const char *prefix[]={"sk-","ghp_","gho_","ghu_","ghs_","ghr_","github_pat_","AKIA",NULL};
    for(uint32_t i=0;i<n;i++)for(unsigned j=0;prefix[j];j++){
        size_t z=strlen(prefix[j]);if(i+z>n||memcmp(s+i,prefix[j],z))continue;
        uint32_t k=i+(uint32_t)z;while(k<n&&tokenchar(s[k]))k++;
        if(k-i-z>=(j==7?16u:20u))return false;
    }
    const char *labels[]={"api_key","api-key","access_token","password","secret_key",NULL};
    for(uint32_t i=0;i<n;i++)for(unsigned j=0;labels[j];j++){
        size_t m=strlen(labels[j]);if(i+m>n)continue;bool match=true;
        for(size_t k=0;k<m;k++)if(lower(s[i+k])!=labels[j][k])match=false;
        if(!match||(i&&tokenchar(s[i-1])))continue;
        uint32_t k=i+(uint32_t)m;if(k<n&&(s[k]=='\''||s[k]=='"'))k++;
        while(k<n&&(s[k]==' '||s[k]=='\t'))k++;
        if(k==n||(s[k]!='='&&s[k]!=':'))continue;k++;
        while(k<n&&(s[k]==' '||s[k]=='\t'))k++;
        if(k<n&&(s[k]=='\''||s[k]=='"'))k++;
        uint32_t start=k;while(k<n&&s[k]&&s[k]!='"'&&s[k]!='\''&&s[k]!='\n'&&s[k]!='\r'&&s[k]!=' '&&s[k]!=','&&s[k]!='}')k++;
        if(k-start>=4&&s[start]!='$'&&s[start]!='<'&&!(k-start>=8&&!memcmp(s+start,"REPLACE_",8))&&!(k-start>=5&&!memcmp(s+start,"YOUR_",5)))return false;
    }
    return true;
}
bool agent_policy_egress(const AgentPolicy *p,uint32_t scope,uint32_t kind,const char *s,uint32_t n){
    if(scope>AGENT_SCOPE_CLOUD||!agent_policy_text(p,s,n))return false;
    if(scope==AGENT_SCOPE_LOCAL)return true;
    unsigned permission=p?p->cloud_egress:AGENT_EGRESS_DENY;
    return permission==AGENT_EGRESS_WORKSPACE||(permission==AGENT_EGRESS_TASK&&kind==AGENT_DATA_TASK);
}
bool agent_policy_loopback(const char *host){struct in_addr a;if(!host)return false;if(equal(host,"localhost"))return true;
    return inet_pton(AF_INET,host,&a)==1&&(ntohl(a.s_addr)>>24)==127;}

/* Regex grammar is deliberately a union of the same component tables. */
static bool regex_add(char *out,uint32_t cap,uint32_t *used,const char *v) {
    for(;*v;v++) {
        unsigned char c=(unsigned char)*v;char part[8];
        if(c>='a'&&c<='z')snprintf(part,sizeof part,"[%c%c]",c,c-32);
        else if(c>='A'&&c<='Z')snprintf(part,sizeof part,"[%c%c]",c+32,c);
        else if(strchr(".[](){}+*?^$|\\",c))snprintf(part,sizeof part,"\\%c",c);
        else{part[0]=(char)c;part[1]=0;}
        size_t n=strlen(part);if(n>=cap-*used)return false;memcpy(out+*used,part,n);*used+=(uint32_t)n;out[*used]=0;
    }return true;
}
static bool regex_raw(char *o,uint32_t cap,uint32_t *n,const char *s){size_t z=strlen(s);if(z>=cap-*n)return false;memcpy(o+*n,s,z);*n+=(uint32_t)z;o[*n]=0;return true;}
uint32_t agent_policy_sensitive_patterns(void) {
    return 1+(sizeof names/sizeof names[0]-1)+(sizeof ends/sizeof ends[0]-1)+(sizeof ids/sizeof ids[0]-1)+(sizeof stems/sizeof stems[0]-1)*(sizeof exts/sizeof exts[0]-1);
}
bool agent_policy_sensitive_regex(uint32_t index,char *o,uint32_t cap) {
    uint32_t n=0;if(!cap||index>=agent_policy_sensitive_patterns())return false;o[0]=0;
#define RAW(v) do{if(!regex_raw(o,cap,&n,v))return false;}while(0)
#define ESC(v) do{if(!regex_add(o,cap,&n,v))return false;}while(0)
    RAW("(^|/)(");
    if(!index){ESC(".env");RAW("(\\.[^/]*)?");}
    else {index--;
        unsigned a=(sizeof names/sizeof names[0]-1),b=(sizeof ends/sizeof ends[0]-1),c=(sizeof ids/sizeof ids[0]-1),d=(sizeof exts/sizeof exts[0]-1);
        if(index<a){ESC(names[index]);}
        else if((index-=a)<b){RAW("[^/]*");ESC(ends[index]);}
        else if((index-=b)<c){ESC(ids[index]);RAW("(\\.[^/]*)?");}
        else {index-=c;ESC(stems[index/d]);ESC(exts[index%d]);}
    }
    RAW(")(/|$)");return true;
#undef RAW
#undef ESC
}
