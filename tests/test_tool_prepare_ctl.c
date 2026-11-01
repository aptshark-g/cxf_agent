/* Exercise the real CLI parser/registry/store/tool wrapper with deterministic
 * Native-parser actions. No model generation or performance claim. */
#include "cxf/base.h"
static cxf_result scripted_step(const void *,const char *,uint32_t,char *,uint32_t,uint32_t *);
#define CTL_STEP_EXEC scripted_step
#define CXF_NATIVE_LLAMA 1
#define main agentctl_production_main
#include "agentctl.c"
#undef main
#include <assert.h>

typedef struct ScriptInput {const AgentLlmCfg *cfg;const char *log;uint32_t len;char *out;uint32_t cap;uint32_t *written;const char *completion;} ScriptInput;
static cxf_result scripted_owner(AgentNative *native,void *opaque){
    ScriptInput *s=opaque;cxf_result rc=agent_native_prepare(native,s->cfg,s->log,s->len);
    if(rc!=CXF_OK)return rc;
    return agent_native_actions(native,s->completion,(uint32_t)strlen(s->completion),s->out,s->cap,s->written);
}
static cxf_result scripted_step(const void *ctx,const char *args,uint32_t n,char *out,uint32_t cap,uint32_t *written){
    static unsigned calls=0;assert(g_tool_prepare&&calls<2);bool receipt=false;
    const char match[]="#test exit=0 ";
    for(uint32_t i=0;i+sizeof(match)-1<=n;i++)if(!memcmp(args+i,match,sizeof(match)-1))receipt=true;
    if(calls)assert(receipt);
    ScriptInput input={(const AgentLlmCfg *)ctx,args,n,out,cap,written,calls?"Tests passed.":"<tool_call>\n{\"name\":\"test.run\",\"arguments\":{\"args\":\"\"}}\n</tool_call>"};
    calls++;g_calls++;return agent_tool_prepare_run(g_tool_prepare,scripted_owner,&input);
}
int main(int argc,char **argv){return agentctl_production_main(argc,argv);}
