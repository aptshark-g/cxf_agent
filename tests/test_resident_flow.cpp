#include "agent_tool_prepare.h"
extern "C" {
#include "agent_fs.h"
#include "sha256.h"
}
#include "../src/llama_internal.h"
#include "chat.h"
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <vector>
extern "C" void native_chat_reference(AgentNative *,const AgentLlmCfg *,const char *,uint32_t);
#define OK(x) do{auto rc_=(x);if(rc_!=CXF_OK){std::fprintf(stderr,"FAIL %d %s rc=%d\n",__LINE__,#x,rc_);std::abort();}}while(0)
static uint64_t tick(){return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
static double ms(uint64_t n){return n/1e6;}
static std::string hash(const void *p,size_t n){unsigned char h[32];sha256_hash(h,(const unsigned char *)p,n);const char *hex="0123456789abcdef";std::string s;for(auto x:h){s+=hex[x>>4];s+=hex[x&15];}return s;}
static void emit(common_json j){puts(j.dump().c_str());fflush(stdout);}
using Fn=std::function<cxf_result(AgentNative *)>;
static void run(AgentToolPrepare *p,Fn f){OK(agent_tool_prepare_run(p,[](AgentNative *n,void *c){return (*(Fn *)c)(n);},&f));}
static cxf_llama_view view(cxf_llama *e,cxf_llama_session h){cxf_llama_view v;OK(cxf_llama_session_view(e,h,&v));return v;}
static void no_files(const std::string &root){for(const auto &p:std::filesystem::recursive_directory_iterator(root))assert(p.path().extension()!=".kv");}
struct Ref{std::vector<int32_t> ids;int32_t token=0;};
struct Fixture{
 std::string root,tools,initial,log;AgentLlmCfg cfg={};BasicFsCfg read;FsRootCfg write;TestRunCfg test;TestSandbox sandbox={};double cold_ms=0,warm_ms=0;std::array<Ref,6> refs;
 Fixture(const std::string &out,int words):root(out+"/resident-project"){
  std::filesystem::create_directory(root);std::filesystem::copy_file(out+"/resident-tool",root+"/resident-tool",std::filesystem::copy_options::overwrite_existing);
  read={root.c_str()};write={root.c_str()};sandbox.writes=1;sandbox.write[0]=root.c_str();test={root.c_str(),"./resident-tool verify",10000,0,&sandbox};
  strcpy(cfg.model,"native-qwen3");strcpy(cfg.system,"Use the configured workspace tools. Verify actual receipts.");
  AgentToolSpec specs[]={{"fs.read","Read configuration","path","file"},{"fs.write_apply","Fix configuration","path and content","receipt"},{"test.run","Run fixed validator","empty","receipt"}};char data[8192];auto n=agent_llm_render_tools(specs,3,data,sizeof data);assert(n&&n<sizeof data);tools.assign(data,n);cfg.tools=tools.c_str();cfg.tools_len=n;
  initial="Read settings.ini, validate, fix limit to32, validate and read back, then validate again.\n";for(int i=0;i<words;i++)initial+="context ";initial+='\n';reset();
  char result[BASIC_OUT_HINT];uint32_t count;auto t=tick();OK(test_run_exec(&test,"",0,result,sizeof result,&count));cold_ms=ms(tick()-t);t=tick();OK(test_run_exec(&test,"",0,result,sizeof result,&count));warm_ms=ms(tick()-t);
 }
 ~Fixture(){std::filesystem::remove_all(root);}
 void reset(){std::ofstream(root+"/settings.ini")<<"limit=0\n";log=initial;}
 void append(const char *name,const std::string &args,const char *out,uint32_t n){log+="#call "+std::string(name)+" "+std::to_string(args.size())+"\n"+args+"\n#result "+name+" "+std::to_string(n)+"B\n"+std::string(out,n)+"\n";}
 Ref fresh(AgentNative *native){
  auto *e=agent_native_engine(native,nullptr);auto need=agent_llm_build_request(&cfg,log.data(),(uint32_t)log.size(),nullptr,0);std::string request(need,'\0');agent_llm_build_request(&cfg,log.data(),(uint32_t)log.size(),request.data(),need);
  auto j=common_json::parse(request);common_chat_templates_inputs input;input.messages=common_chat_msgs_parse_oaicompat(j.at("messages"));input.tools=common_chat_tools_parse_oaicompat(j.at("tools"));input.enable_thinking=false;input.reasoning_format=COMMON_REASONING_FORMAT_DEEPSEEK;
  auto tm=common_chat_templates_init(cxf_llama_native_model(e),"");auto prompt=common_chat_templates_apply(tm.get(),input).prompt;uint32_t n=0;auto rc=cxf_llama_tokenize(e,prompt.data(),(uint32_t)prompt.size(),false,nullptr,0,&n);assert(rc==CXF_OK||rc==CXF_ERR_FULL);Ref r;r.ids.resize(n);OK(cxf_llama_tokenize(e,prompt.data(),(uint32_t)prompt.size(),false,r.ids.data(),n,&n));
  cxf_llama_session h;OK(cxf_llama_session_new(e,&h));OK(cxf_llama_append(e,h,1,r.ids.data(),n));uint32_t done;OK(cxf_llama_prefill(e,h,view(e,h).revision,UINT32_MAX,true,&done));OK(cxf_llama_greedy(e,h,view(e,h).revision,&r.token));OK(cxf_llama_session_close(e,h));return r;
 }
};
static void trial(AgentToolPrepare *p,Fixture &f,int words,bool wrapped,int rep,bool reference){
 OK(agent_tool_prepare_reset(p));f.reset();auto initial=tick();run(p,[&](AgentNative *n){return agent_native_prepare(n,&f.cfg,f.log.data(),(uint32_t)f.log.size());});double initial_ms=ms(tick()-initial);
 uint64_t start=0,end=0,excluded=0;double tool_sum=0,wait_sum=0,prefill_sum=0;uint64_t evaluated_sum=0;
 for(int step=0;step<6;step++){
  const char *name=(step==0||step==4)?"fs.read":step==2?"fs.write_apply":"test.run";std::string args=(step==0||step==4)?"settings.ini":step==2?"settings.ini\nlimit=32\n":"";
  AgentToolExec fn=(step==0||step==4)?basic_read_exec:step==2?fs_write_apply_exec:test_run_exec;
  const void *ctx=(step==0||step==4)?(const void *)&f.read:step==2?(const void *)&f.write:(const void *)&f.test;
  uint32_t committed_before=0,evaluated_before=0,checkpoint=0;
  run(p,[&](AgentNative *n){cxf_llama_session h;auto *e=agent_native_engine(n,&h);auto v=view(e,h);AgentNativeStats a;agent_native_stats(n,&a);committed_before=v.committed;evaluated_before=v.evaluated;checkpoint=a.checkpoint_tokens;assert(v.evaluated==v.committed&&checkpoint);return CXF_OK;});
  char out[BASIC_OUT_HINT];uint32_t count=0;AgentToolPrepareReport report={};auto entered=tick();if(!step)start=entered;uint64_t tool_start,tool_end;
  if(wrapped){OK(agent_tool_prepare_execute(p,step+1,fn,ctx,args.data(),(uint32_t)args.size(),out,sizeof out,&count,&report));tool_start=report.tool_start_ns;tool_end=report.tool_end_ns;
   assert(!report.save_ns&&!report.evict_ns&&!report.prepare_start_ns&&!report.kv.restored_bytes&&report.prepare_rc==CXF_OK&&!report.cancelled);
  }else{tool_start=tick();OK(fn(ctx,args.data(),(uint32_t)args.size(),out,sizeof out,&count));tool_end=tick();}
  auto returned=tick();uint32_t kept=0;run(p,[&](AgentNative *n){cxf_llama_session h;auto *e=agent_native_engine(n,&h);auto v=view(e,h);kept=v.evaluated;assert(v.evaluated>=checkpoint);return CXF_OK;});
  if(fn==test_run_exec){std::string result(out,count);assert(result.find(step==1?"exit=1 ":"exit=0 ")!=std::string::npos);assert(result.find("timeout=0 io_error=0")!=std::string::npos);}
  f.append(name,args,out,count);uint64_t ready=0,first=0,oracle=0,delta_eval=0;AgentNativeStats a={},b={};uint32_t committed=0;int32_t token;std::string canonical;
  run(p,[&](AgentNative *n){auto *e=agent_native_engine(n,nullptr);cxf_llama_stats before,after;cxf_llama_get_stats(e,&before);agent_native_stats(n,&a);OK(agent_native_prepare(n,&f.cfg,f.log.data(),(uint32_t)f.log.size()));ready=tick();cxf_llama_session h;agent_native_engine(n,&h);auto v=view(e,h);committed=v.committed;OK(cxf_llama_greedy(e,h,v.revision,&token));first=tick();cxf_llama_get_stats(e,&after);agent_native_stats(n,&b);delta_eval=after.evaluated_tokens-before.evaluated_tokens;
   auto t=tick();native_chat_reference(n,&f.cfg,f.log.data(),(uint32_t)f.log.size());if(reference)f.refs[step]=f.fresh(n);std::vector<int32_t> ids(v.committed);OK(cxf_llama_read(e,h,0,ids.data(),v.committed));assert(ids==f.refs[step].ids&&token==f.refs[step].token);canonical=hash(ids.data(),ids.size()*sizeof(int32_t));oracle=tick()-t;return CXF_OK;});
  double prefill=ms(b.prefill_ns-a.prefill_ns);tool_sum+=ms(tool_end-tool_start);wait_sum+=ms(first-tool_end);prefill_sum+=prefill;evaluated_sum+=delta_eval;end=first;if(step<5)excluded+=oracle;
  emit({{"kind","timing"},{"scenario","resident_multitool"},{"words",words},{"mode",wrapped?"resident_wrapper":"resident_direct"},{"rep",rep},{"step",step},{"correct",true},{"tool",name},{"entered_ns",entered},{"tool_start_ns",tool_start},{"tool_end_ns",tool_end},{"wrapper_return_ns",returned},{"ready_ns",ready},{"first_token_ns",first},{"tool_ms",ms(tool_end-tool_start)},{"post_tool_first_ms",ms(first-tool_end)},{"call_ms",ms(first-entered)},{"wrapper_ms",ms(returned-entered)-ms(tool_end-tool_start)},{"prefill_ms",prefill},{"render_ms",ms(b.render_ns-a.render_ns)},{"tokenize_ms",ms(b.tokenize_ns-a.tokenize_ns)},{"guard_ms",ms(b.guard_ns-a.guard_ns)},{"evaluated_tokens",delta_eval},{"tokenized_tokens",b.tokenized_tokens-a.tokenized_tokens},{"reused_prefix_tokens",b.reused_prefix_tokens-a.reused_prefix_tokens},{"committed_before",committed_before},{"evaluated_before",evaluated_before},{"checkpoint_before",checkpoint},{"retained_during_tool",kept},{"committed_tokens",committed},{"save_ms",0},{"evict_ms",0},{"restore_ms",0},{"speculative_prepare_ms",0},{"oracle_ms",ms(oracle)},{"canonical_sha256",canonical},{"receipt_sha256",hash(out,count)},{"greedy_token",token}});
 }
 no_files(f.root);emit({{"kind","task_timing"},{"scenario","resident_multitool"},{"words",words},{"mode",wrapped?"resident_wrapper":"resident_direct"},{"rep",rep},{"correct",true},{"calls",6},{"task_wall_ms",ms(end-start)},{"excluded_oracle_ms",ms(excluded)},{"task_ms",ms(end-start-excluded)},{"tool_sum_ms",tool_sum},{"post_tool_sum_ms",wait_sum},{"prefill_sum_ms",prefill_sum},{"evaluated_tokens",evaluated_sum},{"initial_history_ms",initial_ms},{"fixture_first_exec_ms",f.cold_ms},{"fixture_warm_exec_ms",f.warm_ms},{"kv_files",0}});
}
int main(int argc,char **argv){
 assert(argc==6);bool bench=!strcmp(argv[4],"bench");int repeats=bench?atoi(argv[5]):1;assert(repeats>=1&&repeats<=30);auto params=cxf_llama_default_params();params.model_path=argv[1];params.backend_path=argv[2];params.context_tokens=8192;params.max_sessions=4;params.threads=4;AgentToolPrepare *p;OK(agent_tool_prepare_open(&params,16,argv[3],1024ull*1024*1024,{TOOL_KV_RESIDENT,TOOL_PREPARE_TIMED,5},&p));
 emit({{"kind","metadata"},{"context_tokens",8192},{"batch_tokens",512},{"prefill_min_tokens",128},{"threads",4},{"repeats",repeats},{"snapshot_operations",0},{"build_during_task",false},{"actions","scripted actual tools"}});
 for(int words:{512,3072}){Fixture f(argv[3],words);for(int rep=0;rep<repeats;rep++)for(int i=0;i<2;i++){int mode=(rep+i)%2;trial(p,f,words,mode,rep,rep==0&&mode==0);}}
 emit({{"kind","correctness"},{"passed",true},{"fresh_reference_prompts",12},{"resident_prefix_preserved",true},{"real_config_repair",true},{"no_snapshot_or_evict",true}});agent_tool_prepare_close(p);no_files(argv[3]);emit({{"kind","closed"},{"passed",true},{"private_kv_files",0}});
}
