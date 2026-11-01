#define _DARWIN_C_SOURCE 1
#include <sys/event.h>
#include <errno.h>
#include <stdio.h>
static int registrations,events,empty,errors,last_error;
static int traced_kevent(int k,const struct kevent*c,int nc,struct kevent*e,int ne,const struct timespec*t){int r=kevent(k,c,nc,e,ne,t);if(nc)registrations++;if(r>0)events+=r;if(!nc&&!r)empty++;if(r<0){errors++;last_error=errno;}return r;}
#define kevent(...) traced_kevent(__VA_ARGS__)
#include "../tests/agent_basic.c"
int main(void){for(int i=0;i<200;i++){TestRunCfg c={"/tmp","printf x",1000};char out[8192];uint32_t n;TestRunTrace t;registrations=events=empty=errors=last_error=0;int rc=(i%2?test_run_exec_output_race:test_run_exec_output_event)(&c,"",0,out,sizeof out,&n,&t,NULL,NULL);printf("%d %d %llu %.6f %d %d %d %d %d\n",i,rc,(unsigned long long)t.eof_polls,t.eof_poll_ns/1e6,registrations,events,empty,errors,last_error);}}
