/* Fixed benchmark tool: no command supplied by model, no compilation/sleeps. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static uint64_t stamp(void){struct timespec t;
#ifdef __APPLE__
clock_gettime(CLOCK_MONOTONIC_RAW,&t);
#else
clock_gettime(CLOCK_MONOTONIC,&t);
#endif
return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
static unsigned work(unsigned x,unsigned long n){for(unsigned long i=0;i<n;i++)x=(x<<7|x>>25)^(unsigned)i*2654435761u;return x;}
static void json_text(const char *s){putchar('"');for(;*s;s++){unsigned char c=(unsigned char)*s;if(c=='"'||c=='\\')putchar('\\');if(c=='\n')fputs("\\n",stdout);else if(c=='\r')fputs("\\r",stdout);else if(c=='\t')fputs("\\t",stdout);else putchar(c);}putchar('"');}
int main(int argc,char **argv){
    if(argc==2&&!strcmp(argv[1],"verify")){
        FILE *f=fopen("settings.ini","r");char line[128]={0};if(!f||!fgets(line,sizeof line,f))return 126;fclose(f);
        unsigned digest=work(17,2000000);int ok=!strcmp(line,"limit=32\n");printf("config=%s digest=%u\n",ok?"PASS":"FAIL",digest);return ok?0:1;
    }
    if(argc==4&&!strcmp(argv[1],"stream")){
        FILE *f=fopen(argv[2],"r");if(!f)return 126;unsigned long iterations=strtoul(argv[3],NULL,10);char text[8192];unsigned seq=0;
        while(fgets(text,sizeof text,f)){size_t n=strlen(text);if(!n||text[n-1]!='\n'){fclose(f);return 126;}text[n-1]=0;
            unsigned digest=work(17+seq,iterations);char prefix[128];snprintf(prefix,sizeof prefix,"unit=%u digest=%u ",seq+1,digest);
            char content[8400];snprintf(content,sizeof content,"%s%s",prefix,text);uint64_t produced=stamp();
            printf("{\"seq\":%u,\"kind\":\"text\",\"produced_ns\":%llu,\"text\":",++seq,(unsigned long long)produced);json_text(content);puts("}");fflush(stdout);
        }
        fclose(f);printf("{\"seq\":%u,\"kind\":\"end\",\"produced_ns\":%llu}\n",seq+1,(unsigned long long)stamp());fflush(stdout);return seq==12?0:126;
    }
    return 126;
}
