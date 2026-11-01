/* Standalone macOS unit calibration, not an LLM latency benchmark. */
#define _DARWIN_C_SOURCE 1
#ifdef __APPLE__
#include <libproc.h>
#include <mach/mach_time.h>
#include <sys/resource.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
static uint64_t ns(clockid_t c){struct timespec t;assert(!clock_gettime(c,&t));return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
int main(void){mach_timebase_info_data_t tb;assert(!mach_timebase_info(&tb));for(int busy=1;busy>=0;busy--){struct rusage_info_v4 a={0},b={0};assert(!proc_pid_rusage(getpid(),RUSAGE_INFO_V4,(rusage_info_t*)&a));uint64_t t=ns(CLOCK_MONOTONIC_RAW),cpu=ns(CLOCK_THREAD_CPUTIME_ID);volatile uint64_t x=1;if(busy){do{x=x*17+3;}while(ns(CLOCK_THREAD_CPUTIME_ID)-cpu<100000000);}else{struct timespec delay={0,100000000};assert(!nanosleep(&delay,NULL));}uint64_t used=ns(CLOCK_THREAD_CPUTIME_ID)-cpu,wall=ns(CLOCK_MONOTONIC_RAW)-t;assert(!proc_pid_rusage(getpid(),RUSAGE_INFO_V4,(rusage_info_t*)&b));printf("{\"kind\":\"clock_calibration\",\"busy\":%d,\"wall_ns\":%llu,\"cpu_clock_ns\":%llu,\"proc_cpu_ticks\":%llu,\"runnable_ticks\":%llu,\"timebase_numer\":%u,\"timebase_denom\":%u}\n",busy,(unsigned long long)wall,(unsigned long long)used,(unsigned long long)(b.ri_user_time+b.ri_system_time-a.ri_user_time-a.ri_system_time),(unsigned long long)(b.ri_runnable_time-a.ri_runnable_time),tb.numer,tb.denom);}}
#else
int main(void){return 77;}
#endif
