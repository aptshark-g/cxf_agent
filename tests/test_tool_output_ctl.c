/* Actual agentctl registry/store/test.run loop; deterministic Native-parser
 * choices isolate scheduling from the small model's planning quality. */
#include "cxf/base.h"
#ifdef CXF_METAL_PROBE
#include "agent_metal_probe.h"
static const char *output_policy_override(const char *);
#define CTL_TOOL_OUTPUT_BEGIN() agent_metal_probe_begin(2)
#define CTL_TOOL_OUTPUT_END() agent_metal_probe_end()
#define CTL_OUTPUT_POLICY_OVERRIDE(arg) output_policy_override(arg)
#endif
static cxf_result output_step(const void *,const char *,uint32_t,char *,uint32_t,uint32_t *);
static void output_finished(unsigned,unsigned,unsigned,unsigned,bool);
#define CTL_STEP_EXEC output_step
#define CTL_TASK_FINISHED() output_finished(r.calls,r.executed,r.failed,r.capped,r.idle)
#define CXF_NATIVE_LLAMA 1
#define main agentctl_production_main
#include "agentctl.c"
#undef main
#include "sha256.h"
#include <assert.h>
#include <sys/resource.h>
#include "../src/llama_internal.h"
extern void native_chat_reference(AgentNative *,const AgentLlmCfg *,const char *,uint32_t);
static uint64_t output_task,output_oracle;static unsigned output_index;
static uint64_t output_tick(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC_RAW,&t);return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
static double output_cpu(void){struct rusage r;assert(!getrusage(RUSAGE_SELF,&r));return (r.ru_utime.tv_sec+r.ru_stime.tv_sec)*1000.0+(r.ru_utime.tv_usec+r.ru_stime.tv_usec)/1000.0;}
static double output_thread_cpu(void){struct timespec t;assert(!clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t));return t.tv_sec*1000.0+t.tv_nsec/1e6;}
static void output_hash(const void *data,size_t n,char hex[65]){unsigned char h[32];sha256_hash(h,data,n);for(int i=0;i<32;i++)snprintf(hex+2*i,3,"%02x",h[i]);}
typedef struct {const AgentLlmCfg *cfg;const char *log;uint32_t len;char *out;uint32_t cap,*written;unsigned index;} OutputStep;
static cxf_result output_owner(AgentNative *native,void *opaque){
    OutputStep *s=opaque;AgentNativeStats a,b;agent_native_stats(native,&a);cxf_llama *e=agent_native_engine(native,NULL);
#ifdef CXF_METAL_PROBE
    cxf_llama_trace_observer(e,agent_metal_probe_api);cxf_llama_trace_batch_observer(e,agent_metal_probe_batch);
#endif
    cxf_llama_decode_trace tx,ty;cxf_llama_trace_enable(e,true);cxf_llama_trace_get(e,&tx);cxf_llama_stats x,y;cxf_llama_get_stats(e,&x);double cpu=output_cpu(),thread_cpu=output_thread_cpu();uint64_t start=output_tick();
#ifdef CXF_METAL_PROBE
    agent_metal_probe_begin(1);
#endif
    cxf_result rc=agent_native_prepare(native,s->cfg,s->log,s->len);assert(rc==CXF_OK);uint64_t ready=output_tick();cpu=output_cpu()-cpu;thread_cpu=output_thread_cpu()-thread_cpu;cxf_llama_session h;agent_native_engine(native,&h);cxf_llama_view v;assert(cxf_llama_session_view(e,h,&v)==CXF_OK);int32_t token;assert(cxf_llama_greedy(e,h,v.revision,&token)==CXF_OK);uint64_t first=output_tick();cxf_llama_get_stats(e,&y);agent_native_stats(native,&b);cxf_llama_trace_get(e,&ty);
#ifdef CXF_METAL_PROBE
    agent_metal_probe_end();
#endif
    uint64_t check=output_tick();native_chat_reference(native,s->cfg,s->log,s->len);int32_t *ids=malloc(v.committed*sizeof *ids);assert(ids);assert(cxf_llama_read(e,h,0,ids,v.committed)==CXF_OK);char canonical[65],transcript[65];output_hash(ids,v.committed*sizeof *ids,canonical);output_hash(s->log,s->len,transcript);free(ids);uint64_t oracle=output_tick()-check;output_oracle+=oracle;
    const char *completion=s->index<3?"<tool_call>\n{\"name\":\"test.run\",\"arguments\":{\"args\":\"\"}}\n</tool_call>":"Tests passed.";
    rc=agent_native_actions(native,completion,(uint32_t)strlen(completion),s->out,s->cap,s->written);assert(rc==CXF_OK);
    fprintf(stderr,"[output-e2e] {\"kind\":\"step\",\"task\":%llu,\"step\":%u,\"enabled\":%s,\"correct\":true,\"start_ns\":%llu,\"ready_ns\":%llu,\"first_token_ns\":%llu,\"decode_ms\":%.6f,\"sync_ms\":%.6f,\"decode_cpu_ms\":%.6f,\"sync_cpu_ms\":%.6f,\"sync_calls\":%llu,\"prepare_cpu_ms\":%.6f,\"prepare_thread_cpu_ms\":%.6f,\"decode_calls\":%llu,\"prepare_ms\":%.6f,\"first_ms\":%.6f,\"oracle_ms\":%.6f,\"render_ms\":%.6f,\"tokenize_ms\":%.6f,\"prefill_ms\":%.6f,\"evaluated_tokens\":%llu,\"tokenized_tokens\":%llu,\"adopted_tokens\":%llu,\"discarded_tokens\":%llu,\"committed_tokens\":%u,\"greedy_token\":%d,\"canonical_sha256\":\"%s\",\"transcript_sha256\":\"%s\"}\n",(unsigned long long)g_context.task,s->index,g_tool_output_enabled?"true":"false",(unsigned long long)start,(unsigned long long)ready,(unsigned long long)first,(ty.decode_ns-tx.decode_ns)/1e6,(ty.sync_ns-tx.sync_ns)/1e6,(ty.decode_cpu_ns-tx.decode_cpu_ns)/1e6,(ty.sync_cpu_ns-tx.sync_cpu_ns)/1e6,(unsigned long long)(ty.sync_calls-tx.sync_calls),cpu,thread_cpu,(unsigned long long)(y.decode_calls-x.decode_calls),(ready-start)/1e6,(first-start)/1e6,oracle/1e6,(b.render_ns-a.render_ns)/1e6,(b.tokenize_ns-a.tokenize_ns)/1e6,(b.prefill_ns-a.prefill_ns)/1e6,(unsigned long long)(y.evaluated_tokens-x.evaluated_tokens),(unsigned long long)(b.tokenized_tokens-a.tokenized_tokens),(unsigned long long)(b.output_adopted-a.output_adopted),(unsigned long long)(b.output_discarded-a.output_discarded),v.committed,token,canonical,transcript);fflush(stderr);return rc;
}
static cxf_result output_step(const void *ctx,const char *args,uint32_t n,char *out,uint32_t cap,uint32_t *written){
    assert(g_tool_prepare&&g_tool_output_profile);if(output_task!=g_context.task){output_task=g_context.task;output_index=0;output_oracle=0;}assert(output_index<4);
    if(output_index){unsigned receipts=0;for(uint32_t i=0;i+13<n;i++)if(!memcmp(args+i,"#test exit=0 ",13))receipts++;assert(receipts==output_index);}
    OutputStep s={ctx,args,n,out,cap,written,output_index++};g_calls++;return agent_tool_prepare_run(g_tool_prepare,output_owner,&s);
}
static void output_finished(unsigned calls,unsigned executed,unsigned failed,unsigned capped,bool idle){
    assert(output_index==4&&calls==4&&executed==7&&failed==0&&capped==0&&idle);
    fprintf(stderr,"[output-e2e] {\"kind\":\"task_timing\",\"task\":%llu,\"enabled\":%s,\"correct\":true,\"task_wall_ms\":%.6f,\"excluded_oracle_ms\":%.6f,\"task_ms\":%.6f,\"calls\":%u,\"executed\":%u,\"failed\":%u,\"capped\":%u,\"idle\":true}\n",(unsigned long long)g_context.task,g_tool_output_enabled?"true":"false",g_wall_ns/1e6,output_oracle/1e6,(g_wall_ns-output_oracle)/1e6,calls,executed,failed,capped);fflush(stderr);
}
#ifdef CXF_METAL_PROBE
static const char *output_policy_override(const char *s){static char normalized[64];size_t n=strlen(s);bool bare=n>5&&!strcmp(s+n-5,"-bare"),notify=n>7&&!strcmp(s+n-7,"-notify");agent_metal_probe_enable(!bare);agent_metal_probe_completion(notify);if(!bare&&!notify)return s;size_t keep=n-(bare?5:7);assert(keep<sizeof normalized);memcpy(normalized,s,keep);normalized[keep]=0;return normalized;}
#endif
int main(int argc,char **argv){
#ifdef CXF_METAL_PROBE
 agent_metal_probe_init(getenv("CXF_METAL_PROBE_OUT"));
#endif
 return agentctl_production_main(argc,argv);
}
