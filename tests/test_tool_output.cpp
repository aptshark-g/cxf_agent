#include "agent_tool_prepare.h"
#include "../src/llama_internal.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>
extern "C" void native_chat_reference(AgentNative *,const AgentLlmCfg *,const char *,uint32_t);
#define OK(x) do{auto r_=(x);if(r_!=CXF_OK){fprintf(stderr,"FAIL line %d %s rc=%d\n",__LINE__,#x,r_);abort();}}while(0)
using Fn=std::function<cxf_result(AgentNative *)>;
static void run(AgentToolPrepare *p,Fn fn){OK(agent_tool_prepare_run(p,[](AgentNative *n,void *v){return (*(Fn *)v)(n);},&fn));}
static void append(std::string &s,const char *p,uint32_t n){s+="#call test.run 0\n\n#result test.run "+std::to_string(n)+"B\n"+std::string(p,n)+"\n";}
static std::vector<int32_t> ids(AgentNative *n,int32_t *first=nullptr){cxf_llama_session h;auto *e=agent_native_engine(n,&h);cxf_llama_view v;OK(cxf_llama_session_view(e,h,&v));std::vector<int32_t> a(v.committed);OK(cxf_llama_read(e,h,0,a.data(),v.committed));if(first)OK(cxf_llama_greedy(e,h,v.revision,first));return a;}
static void fresh(AgentNative *n,int32_t expected){auto a=ids(n);auto *e=agent_native_engine(n,nullptr);cxf_llama_session h;OK(cxf_llama_session_new(e,&h));OK(cxf_llama_append(e,h,1,a.data(),a.size()));cxf_llama_view v;OK(cxf_llama_session_view(e,h,&v));uint32_t done;OK(cxf_llama_prefill(e,h,v.revision,UINT32_MAX,true,&done));int32_t token;OK(cxf_llama_greedy(e,h,v.revision,&token));assert(token==expected);OK(cxf_llama_session_close(e,h));}
int main(int argc,char **argv){
    OK(agent_sandbox_host_protect());
assert(argc==4);auto params=cxf_llama_default_params();params.model_path=argv[1];params.backend_path=argv[2];params.context_tokens=8192;params.max_sessions=4;params.batch_tokens=1024;params.threads=4;AgentToolPrepare *p;OK(agent_tool_prepare_open(&params,32,argv[3],1ull<<30,{TOOL_KV_RESIDENT,TOOL_PREPARE_TIMED,5},&p));std::string root=std::string(argv[3])+"/output-unit";std::filesystem::create_directory(root);
 AgentLlmCfg cfg={};strcpy(cfg.model,"native-qwen3");strcpy(cfg.system,"Run workspace tests.");char tools[2048];AgentToolSpec spec={"test.run","Run configured tests","empty","receipt"};cfg.tools_len=agent_llm_render_tools(&spec,1,tools,sizeof tools);cfg.tools=tools;
 std::string initial="Please run tests.\n",log=initial;unsigned checked=0;
 // Local suffix updates, Unicode boundary rejection, BPE tail reconciliation,
 // full independent template/tokenizer and fresh-KV greedy equivalence.
 run(p,[&](AgentNative *n){OK(agent_native_prepare(n,&cfg,log.data(),log.size()));OK(agent_native_tool_output_begin(n));assert(agent_native_tool_output_update(n,"\xe4\xb8",2)==CXF_ERR_FORMAT);for(const auto &s:{std::string("read"),std::string("reading "),std::string("reading 中文🙂\n")})OK(agent_native_tool_output_update(n,s.data(),s.size()));std::string body="reading 中文🙂\n\n#test exit=0 signal=0 timeout=0 io_error=0 bytes=19\n";append(log,body.data(),body.size());OK(agent_native_prepare(n,&cfg,log.data(),log.size()));native_chat_reference(n,&cfg,log.data(),log.size());int32_t first;ids(n,&first);fresh(n,first);checked++;return CXF_OK;});
 const char *commands[]={"printf 'hello\\000world\\n中文🙂\\n'; exit 3","true","i=0; while [ $i -lt 900 ]; do printf 'reading '; i=$((i+1)); done","printf before; sleep 1"};
 for(int policy:{0,13,15,16,18,20,21,22}){OK(agent_tool_output_policy(p,policy));for(unsigned c=0;c<4;c++){std::vector<int32_t> reference;std::string receipt;int32_t firstref=0;for(bool on:{false,true}){OK(agent_tool_prepare_reset(p));log=initial;run(p,[&](AgentNative *n){return agent_native_prepare(n,&cfg,log.data(),log.size());});TestRunCfg test={root.c_str(),commands[c],c==3?30u:3000u,0, NULL};char out[BASIC_OUT_HINT];uint32_t size;AgentToolOutputReport r;OK(agent_tool_prepare_output(p,&test,on,"",0,out,sizeof out,&size,&r));assert(!r.cancelled);std::string body(out,size);assert(body.find("#test exit=")>0);if(c==0)assert(body.find("hello?world")!=std::string::npos&&body.find("exit=3 ")!=std::string::npos);if(c==2)assert(body.find("#truncated")!=std::string::npos);if(c==3)assert(body.find("timeout=1")!=std::string::npos);append(log,out,size);run(p,[&](AgentNative *n){OK(agent_native_prepare(n,&cfg,log.data(),log.size()));native_chat_reference(n,&cfg,log.data(),log.size());int32_t token;auto a=ids(n,&token);if(!on){reference=a;receipt=body;firstref=token;fresh(n,token);}else if(c==3)fresh(n,token);else assert(a==reference&&receipt==body&&token==firstref);return CXF_OK;});checked++;}}}
 // Same canonical prefix under split/coalesced batches, including a budget
 // that refuses the whole tail, minimum-batch deferral, invalid atomic setting
 // and reset to the allocated capacity. Independent Native oracle supplies IDs.
 OK(agent_tool_prepare_reset(p));
 run(p,[&](AgentNative *n){std::string seed;for(int i=0;i<1000;i++)seed+="ctx ";OK(agent_native_prepare(n,&cfg,seed.data(),seed.size()));native_chat_reference(n,&cfg,seed.data(),seed.size());auto a=ids(n);assert(a.size()>700);a.resize(700);auto *e=agent_native_engine(n,nullptr);cxf_llama_trace_enable(e,true);int32_t expected=0;
  for(int mode=0;mode<4;mode++){OK(cxf_llama_chunk_policy(e,mode==3?0:512,mode==1||mode==2));assert(cxf_llama_chunk_policy(e,1025,false)==CXF_ERR_RANGE);cxf_llama_session h;OK(cxf_llama_session_new(e,&h));OK(cxf_llama_append(e,h,1,a.data(),a.size()));cxf_llama_view v;OK(cxf_llama_session_view(e,h,&v));cxf_llama_decode_trace before,after;cxf_llama_trace_get(e,&before);uint32_t done=0;
   if(mode==2){OK(cxf_llama_prefill(e,h,v.revision,0,true,&done));assert(done==0);OK(cxf_llama_prefill(e,h,v.revision,699,false,&done));assert(done==699);OK(cxf_llama_prefill(e,h,v.revision,1,false,&done));assert(done==0);OK(cxf_llama_prefill(e,h,v.revision,1,true,&done));assert(done==1);}
   else{OK(cxf_llama_prefill(e,h,v.revision,UINT32_MAX,true,&done));assert(done==700);}
   cxf_llama_trace_get(e,&after);assert(after.decode_calls-before.decode_calls==(mode==2?3:mode==0?2:1));OK(cxf_llama_session_view(e,h,&v));assert(v.evaluated==700);int32_t actual;OK(cxf_llama_greedy(e,h,v.revision,&actual));if(mode==0)expected=actual;else assert(actual==expected);OK(cxf_llama_session_close(e,h));
  }OK(cxf_llama_chunk_policy(e,0,false));cxf_llama_trace_enable(e,false);return CXF_OK;});checked++;
 // Capacity refusal cannot launch or rerun a side-effecting tool.
 TestSandbox sandbox={};sandbox.writes=1;sandbox.write[0]=root.c_str();
 TestRunCfg side={root.c_str(),"printf x >> once; printf body",3000,0, &sandbox};char out[BASIC_OUT_HINT];uint32_t size;AgentToolOutputReport r;assert(agent_tool_prepare_output(p,&side,true,"",0,out,32,&size,&r)==CXF_ERR_RANGE);assert(!std::filesystem::exists(root+"/once"));OK(agent_tool_prepare_reset(p));run(p,[&](AgentNative *n){return agent_native_prepare(n,&cfg,initial.data(),initial.size());});OK(agent_tool_prepare_output(p,&side,true,"",0,out,sizeof out,&size,&r));assert(std::filesystem::file_size(root+"/once")==1);checked++;
 // Cancel drains one real process and removes only provisional suffix.
 for(int policy:{0,13,22}){OK(agent_tool_output_policy(p,policy));OK(agent_tool_prepare_reset(p));uint32_t checkpoint=0;run(p,[&](AgentNative *n){OK(agent_native_prepare(n,&cfg,initial.data(),initial.size()));AgentNativeStats a;agent_native_stats(n,&a);checkpoint=a.checkpoint_tokens;return CXF_OK;});TestRunCfg slow={root.c_str(),"printf 'reading reading reading '; sleep 0.15; printf done",3000,0, NULL};std::thread thread([&]{OK(agent_tool_prepare_output(p,&slow,true,"",0,out,sizeof out,&size,&r));});bool cancelled=false;for(int i=0;i<200&&!cancelled;i++){std::this_thread::sleep_for(std::chrono::milliseconds(1));cancelled=agent_tool_prepare_cancel(p)==CXF_OK;}thread.join();assert(cancelled&&r.cancelled);run(p,[&](AgentNative *n){cxf_llama_session h;auto *e=agent_native_engine(n,&h);cxf_llama_view v;OK(cxf_llama_session_view(e,h,&v));assert(v.committed==checkpoint&&v.evaluated==checkpoint);return CXF_OK;});checked++;}
 // Changed environment forces normal guarded rebuild, never stale adoption.
 OK(agent_tool_prepare_reset(p));run(p,[&](AgentNative *n){OK(agent_native_prepare(n,&cfg,initial.data(),initial.size()));OK(agent_native_tool_output_begin(n));std::string b(1000,'a');OK(agent_native_tool_output_update(n,b.data(),b.size()));strcpy(cfg.system,"A revised environment.");log=initial;append(log,b.data(),b.size());OK(agent_native_prepare(n,&cfg,log.data(),log.size()));native_chat_reference(n,&cfg,log.data(),log.size());return CXF_OK;});checked++;
 TestRunTrace trace;OK(test_run_exec_traced(&side,"",0,out,sizeof out,&size,&trace));assert(!memcmp(out,"#test exit=",11));assert(std::filesystem::file_size(root+"/once")==2);checked++;
 agent_tool_prepare_close(p);for(auto &x:std::filesystem::recursive_directory_iterator(argv[3]))assert(x.path().extension()!=".kv");printf("{\"kind\":\"correctness\",\"passed\":true,\"cases\":%u,\"fresh_greedy\":true,\"cancel_drained\":true,\"no_snapshot_or_evict\":true}\n",checked);std::filesystem::remove_all(root);
}
