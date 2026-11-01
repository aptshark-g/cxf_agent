/* Same prebuilt command/body in every mode. Timing sidecar is private. */
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#include <mach/mach_time.h>
#endif
static uint64_t ns(clockid_t c){struct timespec t;clock_gettime(c,&t);return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
int main(void){
 uint64_t begin=ns(CLOCK_MONOTONIC_RAW),cpu=ns(CLOCK_PROCESS_CPUTIME_ID),first=0,last=0,compute=0,output=0;struct rusage ra,rb;getrusage(RUSAGE_SELF,&ra);
 uint64_t run0=0,run1=0,pcpu0=0,pcpu1=0,cycles0=0,cycles1=0,instr0=0,instr1=0;unsigned numer=1,denom=1;int proc0=-1,proc1=-1;
#ifdef __APPLE__
 struct rusage_info_v4 a={0},b={0};mach_timebase_info_data_t tb;mach_timebase_info(&tb);numer=tb.numer;denom=tb.denom;
 proc0=proc_pid_rusage(getpid(),RUSAGE_INFO_V4,(rusage_info_t*)&a);run0=a.ri_runnable_time;pcpu0=a.ri_user_time+a.ri_system_time;cycles0=a.ri_cycles;instr0=a.ri_instructions;
#endif
 unsigned words;unsigned long long iterations;FILE *f=fopen("case.txt","r");if(!f||fscanf(f,"%u %llu",&words,&iterations)!=2||words>56)return 2;fclose(f);
 f=fopen("invocations","ab");if(!f)return 3;fputc('x',f);fclose(f);uint64_t setup=ns(CLOCK_MONOTONIC_RAW)-begin,x=17;
 for(unsigned line=0;line<8;line++){
  uint64_t t=ns(CLOCK_MONOTONIC_RAW);for(unsigned long long i=0;i<iterations;i++){x^=i+line;x=(x<<7)|(x>>57);x+=UINT64_C(0x9e3779b97f4a7c15);}compute+=ns(CLOCK_MONOTONIC_RAW)-t;t=ns(CLOCK_MONOTONIC_RAW);
  printf("section %u %08x ",line,(unsigned)x);for(unsigned j=0;j<words;j++)fputs("reading ",stdout);fputc('\n',stdout);fflush(stdout);last=ns(CLOCK_MONOTONIC_RAW);output+=last-t;if(!first)first=last;
 }
 getrusage(RUSAGE_SELF,&rb);
#ifdef __APPLE__
 proc1=proc_pid_rusage(getpid(),RUSAGE_INFO_V4,(rusage_info_t*)&b);run1=b.ri_runnable_time;pcpu1=b.ri_user_time+b.ri_system_time;cycles1=b.ri_cycles;instr1=b.ri_instructions;
#endif
 uint64_t end=ns(CLOCK_MONOTONIC_RAW),used=ns(CLOCK_PROCESS_CPUTIME_ID)-cpu;
 f=fopen("producer-metrics.jsonl","a");if(!f)return 4;
 fprintf(f,"{\"begin_ns\":%llu,\"first_ns\":%llu,\"last_ns\":%llu,\"end_ns\":%llu,\"cpu_ns\":%llu,\"setup_ns\":%llu,\"compute_ns\":%llu,\"output_ns\":%llu,\"runnable_ticks\":%llu,\"proc_cpu_ticks\":%llu,\"timebase_numer\":%u,\"timebase_denom\":%u,\"proc_start_rc\":%d,\"proc_end_rc\":%d,\"voluntary\":%ld,\"involuntary\":%ld,\"cycles\":%llu,\"instructions\":%llu}\n",(unsigned long long)begin,(unsigned long long)first,(unsigned long long)last,(unsigned long long)end,(unsigned long long)used,(unsigned long long)setup,(unsigned long long)compute,(unsigned long long)output,(unsigned long long)(run1-run0),(unsigned long long)(pcpu1-pcpu0),numer,denom,proc0,proc1,rb.ru_nvcsw-ra.ru_nvcsw,rb.ru_nivcsw-ra.ru_nivcsw,(unsigned long long)(cycles1-cycles0),(unsigned long long)(instr1-instr0));fclose(f);return 0;
}
