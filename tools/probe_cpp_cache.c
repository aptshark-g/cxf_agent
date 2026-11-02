#define _DARWIN_C_SOURCE 1
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static unsigned files,pcm,pch;static unsigned long long bytes;
static int walk(const char *path,unsigned depth){
    if(depth>64)return 1;DIR *d=opendir(path);if(!d)return 1;int bad=0;struct dirent *e;
    while((e=readdir(d))){if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;char p[4096];struct stat s;
        int n=snprintf(p,sizeof p,"%s/%s",path,e->d_name);if(n<0||(size_t)n>=sizeof p||lstat(p,&s)){bad=1;break;}
        if(S_ISDIR(s.st_mode)){if(walk(p,depth+1)){bad=1;break;}}
        else if(S_ISREG(s.st_mode)){if(++files>65536){bad=1;break;}bytes+=(unsigned long long)s.st_size;const char *ext=strrchr(p,'.');if(ext&&!strcmp(ext,".pcm"))pcm++;if(ext&&!strcmp(ext,".pch"))pch++;}
    }closedir(d);return bad;
}
int main(int argc,char **argv){if(argc!=2||walk(argv[1],0))return 2;printf("CPP_CACHE {\"files\":%u,\"pcm_files\":%u,\"pch_files\":%u,\"file_bytes\":%llu}\n",files,pcm,pch,bytes);return 0;}
