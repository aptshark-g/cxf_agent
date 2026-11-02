#include "agent_tool_prepare.h"
extern "C" {
#include "sha256.h"
#include "agent_basic.h"
#include "agent_fs.h"
}
#include "../src/llama_internal.h"
#include "chat.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

extern "C" void native_chat_reference(AgentNative *,const AgentLlmCfg *,const char *,uint32_t);
#define OK(x) do{auto rc_=(x);if(rc_!=CXF_OK){std::fprintf(stderr,"FAIL %d %s rc=%d\n",__LINE__,#x,rc_);std::abort();}}while(0)
static uint64_t tick(){return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
static double ms(uint64_t n){return n/1e6;}
using Fn=std::function<cxf_result(AgentNative *)>;
static void run(AgentToolPrepare *p,Fn fn){OK(agent_tool_prepare_run(p,[](AgentNative *n,void *c){return (*(Fn *)c)(n);},&fn));}
static std::string hash_bytes(const void *data,size_t n){unsigned char hash[32];sha256_hash(hash,(const unsigned char *)data,n);static const char hex[]="0123456789abcdef";std::string result;for(auto v:hash){result.push_back(hex[v>>4]);result.push_back(hex[v&15]);}return result;}
static void emit(common_json value){std::puts(value.dump().c_str());std::fflush(stdout);}
static common_json resources(const AgentResourceSample &s){return {{"valid",s.valid},{"user_ns",s.user_ns},{"system_ns",s.system_ns},{"minor_faults",s.minor_faults},{"major_faults",s.major_faults},{"in_ops",s.in_ops},{"out_ops",s.out_ops},{"voluntary",s.voluntary},{"involuntary",s.involuntary},{"resident_bytes",s.resident_bytes},{"footprint_bytes",s.footprint_bytes},{"pageins",s.pageins},{"pageouts",s.pageouts},{"compressions",s.compressions},{"decompressions",s.decompressions},{"swapins",s.swapins},{"swapouts",s.swapouts}};}
struct Reference {std::vector<int32_t> ids;int32_t next=0;};
struct Fixture {
    std::string root,tools,command,initial,log;AgentLlmCfg cfg={};bool taskcost=false,phases=false,launch_profile=false;
    BasicFsCfg read={};TestRunCfg test={};TestSandbox sandbox={};FsRootCfg write={};
    std::array<Reference,3> reference;
    explicit Fixture(const std::string &base,int words,bool compile,int loops=30000000,bool new_task=false,bool phase_driver=false,bool launch=false):root(base),taskcost(new_task),phases(phase_driver),launch_profile(launch){
        std::filesystem::create_directory(root);
        std::ofstream(root+"/verify.c")<<"#include <stdio.h>\nint calc(int);\nint main(void){unsigned x=7;for(unsigned i=0;i<"<<loops<<";i++)x=x*1664525u+1013904223u;int ok=calc(3)==4;printf(\"check=%s digest=%u\\n\",ok?\"PASS\":\"FAIL\",x);return ok?0:1;}\n";
        if(taskcost)std::ofstream(root+"/verify.c")<<"#include <stdio.h>\nint calc(int);\nint main(void){unsigned x=17;for(unsigned i=0;i<"<<loops<<";i++){x=(x<<7)|(x>>25);x^=i*2654435761u;}int ok=calc(3)==7&&calc(9)==19;printf(\"arithmetic=%s digest=%u\\n\",ok?\"PASS\":\"FAIL\",x);return ok?0:1;}\n";
        if(launch_profile)std::ofstream(root+"/verify.c")<<"#include <stdio.h>\n#include <stdint.h>\n#include <time.h>\nint calc(int);\nstatic uint64_t stamp(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC_RAW,&t);return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}\nint main(void){uint64_t entry=stamp();unsigned x=17;for(unsigned i=0;i<"<<loops<<";i++){x=(x<<7)|(x>>25);x^=i*2654435761u;}uint64_t done=stamp();int ok=calc(3)==7&&calc(9)==25;printf(\"arithmetic=%s digest=%u\\n\",ok?\"PASS\":\"FAIL\",x);FILE *f=fopen(\"entry-metrics\",\"w\");if(!f)return 126;fprintf(f,\"%llu %llu\\n\",(unsigned long long)entry,(unsigned long long)done);if(fclose(f))return 126;return ok?0:1;}\n";
        command="cc -O2 calc.c verify.c -o check && ./check";
        if(phases){std::filesystem::copy_file(std::filesystem::path(base).parent_path()/"tool-phase-driver",root+"/phase-driver",std::filesystem::copy_options::overwrite_existing);command="./phase-driver";}
        read={root.c_str()};write={root.c_str()};sandbox.writes=1;sandbox.write[0]=root.c_str();test={root.c_str(),command.c_str(),10000,0,&sandbox};
        std::strcpy(cfg.model,"native-qwen3");std::strcpy(cfg.system,"Use workspace tools. Report actual test receipts.");
        AgentToolSpec specs[]={{"fs.read","Read file","path","file"},{"test.run","Run tests","empty","receipt"},{"fs.write_apply","Replace file","path and content","receipt"}};
        char data[8192];auto n=agent_llm_render_tools(specs,3,data,sizeof data);assert(n&&n<sizeof data);tools.assign(data,n);cfg.tools=tools.c_str();cfg.tools_len=n;
        initial=compile?"Check and repair calc.c, then verify twice.\n":"Read calc.c three times.\n";
        if(taskcost)initial=compile?"Repair the affine calculation and check both inputs, then verify twice.\n":"Inspect the affine calculation file three times.\n";
        for(int i=0;i<words;i++)initial+="context ";initial+='\n';
    }
    ~Fixture(){std::filesystem::remove_all(root);}
    void reset(){std::ofstream(root+"/calc.c")<<(taskcost?"int calc(int x){return x*2;}\n":"int calc(int x){return x+0;}\n");if(phases)std::filesystem::remove(root+"/phase-metrics");if(launch_profile)std::filesystem::remove(root+"/check");log=initial;}
    common_json metrics(){
        common_json result=common_json::object();if(!phases)return result;
        std::ifstream in(root+"/phase-metrics");std::string phase;uint64_t begin,end,spawn,entry,computed;double user,system;long minor,major,io_in,io_out,vol,inv;int executed;
        while(in>>phase>>begin>>end>>user>>system>>minor>>major>>io_in>>io_out>>vol>>inv>>spawn>>entry>>computed>>executed){assert(phase=="compile"||phase=="run"||phase=="rerun");result[phase]={{"start_ns",begin},{"end_ns",end},{"wall_ms",ms(end-begin)},{"user_ms",user},{"system_ms",system},{"minor_faults",minor},{"major_faults",major},{"in_ops",io_in},{"out_ops",io_out},{"voluntary",vol},{"involuntary",inv},{"spawn_end_ns",spawn},{"main_ns",entry},{"computed_ns",computed},{"executed",executed}};}
        assert(result.size()==(launch_profile?3:2));return result;
    }
    void append(const char *name,const std::string &args,const char *result,uint32_t n){log+="#call "+std::string(name)+" "+std::to_string(args.size())+"\n"+args+"\n#result "+name+" "+std::to_string(n)+"B\n"+std::string(result,n)+"\n";}
};
static cxf_llama_view view(cxf_llama *e,cxf_llama_session h){cxf_llama_view v;OK(cxf_llama_session_view(e,h,&v));return v;}
static Reference fresh(AgentNative *native,const Fixture &f){
    auto *e=agent_native_engine(native,nullptr);
    auto need=agent_llm_build_request(&f.cfg,f.log.data(),(uint32_t)f.log.size(),nullptr,0);assert(need);
    std::string text(need,'\0');agent_llm_build_request(&f.cfg,f.log.data(),(uint32_t)f.log.size(),&text[0],need);
    auto json=common_json::parse(text);common_chat_templates_inputs input;
    input.messages=common_chat_msgs_parse_oaicompat(json.at("messages"));input.tools=common_chat_tools_parse_oaicompat(json.at("tools"));
    input.enable_thinking=false;input.reasoning_format=COMMON_REASONING_FORMAT_DEEPSEEK;
    auto templates=common_chat_templates_init(cxf_llama_native_model(e),"");auto prompt=common_chat_templates_apply(templates.get(),input).prompt;
    uint32_t count=0;auto rc=cxf_llama_tokenize(e,prompt.data(),(uint32_t)prompt.size(),false,nullptr,0,&count);assert(rc==CXF_ERR_FULL||rc==CXF_OK);
    Reference r;r.ids.resize(count);OK(cxf_llama_tokenize(e,prompt.data(),(uint32_t)prompt.size(),false,r.ids.data(),count,&count));
    cxf_llama_session h;OK(cxf_llama_session_new(e,&h));OK(cxf_llama_append(e,h,1,r.ids.data(),count));uint32_t done;
    OK(cxf_llama_prefill(e,h,view(e,h).revision,UINT32_MAX,true,&done));assert(done==count);
    OK(cxf_llama_greedy(e,h,view(e,h).revision,&r.next));OK(cxf_llama_session_close(e,h));return r;
}
struct Mode {const char *name;AgentToolPrepareOptions options;TestRunBackend backend=TEST_RUN_FORK;bool event=true;bool incremental=false;};
static const Mode MODES[]={
    {"resident",{TOOL_KV_RESIDENT,TOOL_PREPARE_LATE,5}},
    {"prefill_late",{TOOL_KV_RECOMPUTE,TOOL_PREPARE_LATE,5}},
    {"prefill_immediate",{TOOL_KV_RECOMPUTE,TOOL_PREPARE_IMMEDIATE,5}},
    {"prefill_timed",{TOOL_KV_RECOMPUTE,TOOL_PREPARE_TIMED,5}},
    {"restore_late",{TOOL_KV_RESTORE,TOOL_PREPARE_LATE,5}},
    {"restore_immediate",{TOOL_KV_RESTORE,TOOL_PREPARE_IMMEDIATE,5}},
    {"restore_timed",{TOOL_KV_RESTORE,TOOL_PREPARE_TIMED,5}}
};
struct Seed {double tool=0,rate=0,post_spawn=0;};
struct TracedTool {const TestRunCfg *cfg;AgentToolPrepare *host;AgentToolPrepareReport *report;TestRunBackend backend;TestRunTrace trace={};bool event=true;uint64_t callback_start=0,callback_end=0;bool resource_trace=false;AgentResourceSample before={},after={};};
static void tool_progress(void *ctx,const TestRunTrace *trace){auto &t=*(TracedTool *)ctx;t.callback_start=tick();OK(agent_tool_prepare_progress(t.host,t.report->epoch,trace->spawn_end_ns));t.callback_end=tick();}
static cxf_result traced_tool(const void *ctx,const char *args,uint32_t n,char *out,uint32_t cap,uint32_t *written){auto &t=*(TracedTool *)ctx;if(t.resource_trace)t.before=agent_resource_sample();auto rc=test_run_exec_observed(t.cfg,args,n,out,cap,written,&t.trace,t.backend,t.event?tool_progress:nullptr,(void *)&t);if(t.resource_trace)t.after=agent_resource_sample();return rc;}
static std::array<Seed,3> trial(AgentToolPrepare *p,Fixture &f,bool compile,int words,const Mode &mode,const char *phase,int rep,bool references,uint64_t keybase=0){
    OK(agent_tool_prepare_options(p,mode.options));OK(agent_tool_prepare_reset(p));f.reset();
    if(f.launch_profile){f.command=mode.incremental?"./phase-driver --incremental":"./phase-driver --repeat";f.test.command=f.command.c_str();}
    auto initial_start=tick();run(p,[&](AgentNative *n){return agent_native_prepare(n,&f.cfg,f.log.data(),(uint32_t)f.log.size());});auto initial_ns=tick()-initial_start;
    uint64_t task_start=0,last_ready=0,oracle_before_last=0,inter_tool_ns=0;
    double wait_sum=0,tool_sum=0,save_sum=0,prepare_sum=0,overlap_sum=0,hold_sum=0;
    std::array<Seed,3> seeds;
    for(int step=0;step<3;step++){
        if(compile&&step==1){auto start=tick();char result[FS_WRITE_OUT_HINT];uint32_t nr;std::string args="calc.c\nint calc(int x){return x+1;}\n";
            if(f.taskcost)args="calc.c\nint calc(int x){return x*2+1;}\n";
            if(f.launch_profile)args="calc.c\nint calc(int x){return x*3-2;}\n";
            OK(fs_write_apply_exec(&f.write,args.data(),(uint32_t)args.size(),result,sizeof result,&nr));f.append("fs.write_apply",args,result,nr);
            run(p,[&](AgentNative *n){return agent_native_prepare(n,&f.cfg,f.log.data(),(uint32_t)f.log.size());});inter_tool_ns+=tick()-start;}
        char output[BASIC_OUT_HINT];uint32_t written=0;AgentToolPrepareReport r;
        std::string args=compile?"":"calc.c";TracedTool tool{&f.test,p,&r,mode.backend,{},mode.event};
        tool.resource_trace=mode.options.trace_resources;
        OK(agent_tool_prepare_execute(p,keybase+step+1,compile?traced_tool:basic_read_exec,compile?(const void *)&tool:(const void *)&f.read,args.data(),(uint32_t)args.size(),output,sizeof output,&written,&r));
        assert(r.tool_rc==CXF_OK&&r.prepare_rc==CXF_OK&&!r.cancelled);
        if(compile){std::string result(output,written);assert(result.find(step==0?"exit=1 ":"exit=0 ")!=std::string::npos);assert(result.find("timeout=0 io_error=0")!=std::string::npos);}
        auto wrapper_return=tick();auto phases=f.metrics();auto assembly_start=tick();
        f.append(compile?"test.run":"fs.read",args,output,written);auto assembly_end=tick();
        uint64_t ready=0,greedy_ready=0,oracle_ns=0;uint64_t evaluated=0,rebuilds=0;uint32_t committed=0;std::string canonical_hash;int32_t next_token=0;AgentNativeStats delta={};
        run(p,[&](AgentNative *n){
            auto *e=agent_native_engine(n,nullptr);cxf_llama_stats before,after;cxf_llama_get_stats(e,&before);AgentNativeStats a,b;agent_native_stats(n,&a);
            OK(agent_native_prepare(n,&f.cfg,f.log.data(),(uint32_t)f.log.size()));ready=tick();
            cxf_llama_session h;agent_native_engine(n,&h);auto v=view(e,h);int32_t token;OK(cxf_llama_greedy(e,h,v.revision,&token));greedy_ready=tick();
            cxf_llama_get_stats(e,&after);evaluated=after.evaluated_tokens-before.evaluated_tokens;committed=v.committed;agent_native_stats(n,&b);rebuilds=b.rebuilds-a.rebuilds;delta.render_ns=b.render_ns-a.render_ns;delta.tokenize_ns=b.tokenize_ns-a.tokenize_ns;delta.guard_ns=b.guard_ns-a.guard_ns;delta.commit_ns=b.commit_ns-a.commit_ns;delta.prefill_ns=b.prefill_ns-a.prefill_ns;
            auto start=tick();native_chat_reference(n,&f.cfg,f.log.data(),(uint32_t)f.log.size());
            if(references)f.reference[step]=fresh(n,f);
            std::vector<int32_t> ids(v.committed);OK(cxf_llama_read(e,h,0,ids.data(),v.committed));assert(ids==f.reference[step].ids&&token==f.reference[step].next);canonical_hash=hash_bytes(ids.data(),ids.size()*sizeof(int32_t));next_token=token;
            oracle_ns=tick()-start;return CXF_OK;
        });
        if(!step)task_start=r.entered_ns;last_ready=greedy_ready;
        uint64_t overlap=0,hold=0;if(r.prepare_start_ns&&r.prefix_ready_ns){auto a=std::max(r.prepare_start_ns,r.tool_start_ns),b=std::min(r.prefix_ready_ns,r.tool_end_ns);if(b>a)overlap=b-a;if(r.tool_end_ns>r.prefix_ready_ns)hold=r.tool_end_ns-r.prefix_ready_ns;}
        auto row=common_json{{"kind","timing"},{"phase",phase},{"scenario",compile?"compile_test":"read"},{"words",words},{"mode",mode.name},{"rep",rep},{"step",step},
            {"correct",true},{"canonical_sha256",canonical_hash},{"receipt_sha256",hash_bytes(output,written)},{"greedy_token",next_token},{"entered_ns",r.entered_ns},{"tool_start_ns",r.tool_start_ns},{"tool_end_ns",r.tool_end_ns},{"prefix_start_ns",r.prepare_start_ns},{"prefix_ready_ns",r.prefix_ready_ns},{"request_ready_ns",ready},{"first_token_ns",greedy_ready},
            {"tool_ms",ms(r.tool_end_ns-r.tool_start_ns)},{"post_tool_ready_ms",ms(ready-r.tool_end_ns)},{"post_tool_first_ms",ms(greedy_ready-r.tool_end_ns)},
            {"total_first_ms",ms(greedy_ready-r.entered_ns)},{"checkpoint_ms",ms(r.checkpoint_ns)},{"save_ms",ms(r.save_ns)},{"evict_ms",ms(r.evict_ns)},
            {"prepare_ms",r.prepare_start_ns?ms(r.prefix_ready_ns-r.prepare_start_ns):0},{"restore_ms",ms(r.kv.restore_ns)},{"restored_bytes",r.kv.restored_bytes},{"restored_tokens",r.kv.restored_tokens},
            {"overlap_ms",ms(overlap)},{"early_hold_ms",ms(hold)},{"predicted_tool_ms",r.predicted_tool_ms},{"predicted_prepare_ms",r.predicted_prepare_ms},{"planned_delay_ms",r.planned_delay_ms},
            {"estimate_key",keybase+step+1},{"tool_observations_before",r.tool_observations_before},{"prepare_observations_before",r.prepare_observations_before},{"prefix_tokens",r.prefix_tokens},{"committed_tokens",committed},{"suffix_evaluated_tokens",evaluated},
            {"wasted_tokens",r.wasted_tokens},{"oracle_ms",ms(oracle_ns)},{"rebuilds",rebuilds},
            {"process_backend",tool.trace.backend},{"configured_backend",(int)mode.backend},{"epoch",r.epoch},{"progress_ns",r.progress_ns},{"predicted_post_spawn_ms",r.predicted_post_spawn_ms},{"progress_observations_before",r.progress_observations_before},{"initial_tool_estimate_ms",r.initial_tool_estimate_ms},{"initial_delay_ms",r.initial_delay_ms},
            {"thread_enter_ns",r.thread_enter_ns},{"timer_deadline_ns",r.timer_deadline_ns},{"timer_wake_ns",r.timer_wake_ns},
            {"prepare_api_return_ns",r.prepare_api_return_ns},{"join_return_ns",r.join_return_ns},{"drop_start_ns",r.drop_start_ns},{"drop_end_ns",r.drop_end_ns},
            {"kv_queued_ns",r.kv.queued_ns},{"kv_started_ns",r.kv.started_ns},{"kv_restore_start_ns",r.kv.restore_start_ns},{"kv_restore_end_ns",r.kv.restore_end_ns},{"kv_ready_ns",r.kv.ready_ns},
            {"kv_finish_enter_ns",r.kv.finish_enter_ns},{"kv_finish_owner_ns",r.kv.finish_owner_ns},{"kv_finished_ns",r.kv.finished_ns},{"kv_finish_return_ns",r.kv.finish_return_ns},{"kv_verify_ms",ms(r.kv.verify_ns)},
            {"tool_wrapper_finished_ns",r.finished_ns},{"wrapper_return_ns",wrapper_return},{"assembly_start_ns",assembly_start},{"assembly_end_ns",assembly_end},
            {"tool_trace_enter_ns",tool.trace.entered_ns},{"spawn_begin_ns",tool.trace.spawn_begin_ns},{"spawn_end_ns",tool.trace.spawn_end_ns},{"first_output_ns",tool.trace.first_output_ns},{"reaped_ns",tool.trace.reaped_ns},{"tool_trace_finished_ns",tool.trace.finished_ns},
            {"final_render_ms",ms(delta.render_ns)},{"final_tokenize_ms",ms(delta.tokenize_ns)},{"final_guard_ms",ms(delta.guard_ns)},{"final_commit_ms",ms(delta.commit_ns)},{"final_prefill_ms",ms(delta.prefill_ns)},
            {"thread_launch_start_ns",r.thread_launch_start_ns},{"thread_launch_end_ns",r.thread_launch_end_ns},{"tool_fn_start_ns",r.tool_fn_start_ns},
            {"callback_start_ns",tool.callback_start},{"callback_end_ns",tool.callback_end},{"event_enabled",mode.event},
            {"early_drop_start_ns",r.early_drop_start_ns},{"early_drop_end_ns",r.early_drop_end_ns},{"early_drop_rc",r.early_drop_rc},{"drop_rc",r.drop_rc},{"release_snapshot_early",mode.options.release_snapshot_early},
            {"hold_snapshot_file",mode.options.hold_snapshot_file},{"trace_resources",mode.options.trace_resources},{"phases",phases},
            {"tool_resource_before",resources(tool.before)},{"tool_resource_after",resources(tool.after)},
            {"early_drop_before",resources(r.early_drop_before)},{"early_drop_after",resources(r.early_drop_after)},{"drop_before",resources(r.drop_before)},{"drop_after",resources(r.drop_after)}};
        if(compile)assert(r.tool_start_ns<=tool.trace.entered_ns&&tool.trace.entered_ns<=tool.trace.spawn_begin_ns&&tool.trace.spawn_begin_ns<=tool.trace.spawn_end_ns&&tool.trace.spawn_end_ns<=r.tool_end_ns);
        emit(row);
        wait_sum+=ms(greedy_ready-r.tool_end_ns);tool_sum+=ms(r.tool_end_ns-r.tool_start_ns);save_sum+=ms(r.save_ns);
        prepare_sum+=r.prepare_start_ns?ms(r.prefix_ready_ns-r.prepare_start_ns):0;overlap_sum+=ms(overlap);hold_sum+=ms(hold);
        seeds[step]={ms(r.tool_end_ns-r.tool_start_ns),r.prepare_start_ns?ms(r.prefix_ready_ns-r.prepare_start_ns)/r.prefix_tokens:0,tool.trace.spawn_end_ns?ms(r.tool_end_ns-tool.trace.spawn_end_ns):0};
        if(step<2)oracle_before_last+=oracle_ns;
    }
    emit({{"kind","task_timing"},{"phase",phase},{"scenario",compile?"compile_test":"read"},{"words",words},{"mode",mode.name},{"rep",rep},{"correct",true},
        {"task_wall_ms",ms(last_ready-task_start)},{"excluded_in_task_oracle_ms",ms(oracle_before_last)},{"task_ms",ms(last_ready-task_start-oracle_before_last)},
        {"initial_history_ms",ms(initial_ns)},{"inter_tool_ms",ms(inter_tool_ns)},{"post_tool_sum_ms",wait_sum},{"tool_sum_ms",tool_sum},{"save_sum_ms",save_sum},{"prepare_sum_ms",prepare_sum},{"overlap_sum_ms",overlap_sum},{"early_hold_sum_ms",hold_sum},{"calls",3}});
    return seeds;
}
struct Gate {std::mutex mutex;std::condition_variable cv;bool entered=false,release=false;int calls=0;};
static cxf_result gated(const void *ctx,const char *,uint32_t,char *out,uint32_t,uint32_t *n){auto &g=*(Gate *)ctx;std::unique_lock<std::mutex> lock(g.mutex);g.calls++;g.entered=true;g.cv.notify_all();g.cv.wait(lock,[&]{return g.release;});out[0]='x';*n=1;return CXF_OK;}
struct ProgressGate {AgentToolPrepare *host;AgentToolPrepareReport *report;Gate gate;};
static cxf_result progress_gate(const void *ctx,const char *args,uint32_t n,char *out,uint32_t cap,uint32_t *written){
    auto &g=*(ProgressGate *)ctx;uint64_t now=tick();
    assert(agent_tool_prepare_progress(g.host,g.report->epoch+1,now)==CXF_ERR_BAD_HANDLE);
    assert(agent_tool_prepare_progress(g.host,g.report->epoch,g.report->tool_start_ns-1)==CXF_ERR_RANGE);
    OK(agent_tool_prepare_progress(g.host,g.report->epoch,now));
    assert(agent_tool_prepare_progress(g.host,g.report->epoch,now)==CXF_ERR_RANGE);
    return gated(&g.gate,args,n,out,cap,written);
}
static void correctness(AgentToolPrepare *p,Fixture &f){
    run(p,[&](AgentNative *n){auto *e=agent_native_engine(n,nullptr);AgentNative *borrowed=nullptr;OK(agent_native_bind(e,8192,8,&borrowed));agent_native_close(borrowed);cxf_llama_stats s;cxf_llama_get_stats(e,&s);return CXF_OK;});
    assert(agent_tool_prepare_options(p,{TOOL_KV_RECOMPUTE,TOOL_PREPARE_TIMED,-1})==CXF_ERR_RANGE);
    assert(agent_tool_prepare_seed(p,1,NAN,1)==CXF_ERR_RANGE);
    for(int release:{0,1,2})for(int cancel_stage:{0,1,2}){
        bool early=release!=0,hold=release==2;
        bool inflight=cancel_stage!=0;
        OK(agent_tool_prepare_reset(p));f.reset();run(p,[&](AgentNative *n){return agent_native_prepare(n,&f.cfg,f.log.data(),(uint32_t)f.log.size());});
        OK(agent_tool_prepare_options(p,{early?TOOL_KV_RESTORE:TOOL_KV_RECOMPUTE,TOOL_PREPARE_TIMED,5,early,hold}));OK(agent_tool_prepare_seed(p,1,inflight?0:5000,.001));
        Gate gate;AgentToolPrepareReport r;char output[8192];uint32_t written=0;
        std::thread host([&]{OK(agent_tool_prepare_execute(p,1,gated,&gate,"",0,output,sizeof output,&written,&r));});
        {std::unique_lock<std::mutex> lock(gate.mutex);gate.cv.wait(lock,[&]{return gate.entered;});}
        if(inflight)std::this_thread::sleep_for(std::chrono::milliseconds(cancel_stage==2?1500:10));
        OK(agent_tool_prepare_cancel(p));{std::lock_guard<std::mutex> lock(gate.mutex);gate.release=true;}gate.cv.notify_one();host.join();
        assert(gate.calls==1&&r.cancelled&&r.prepare_rc==CXF_ERR_BAD_HANDLE);
        if(inflight)assert(r.prepare_start_ns&&r.prefix_ready_ns);else assert(!r.prepare_start_ns);
        if(cancel_stage==2)assert(r.prefix_ready_ns<r.tool_end_ns);
        if(early){assert(r.early_drop_rc==CXF_OK);if(hold)assert(r.drop_rc==CXF_OK&&r.drop_start_ns>=r.tool_end_ns);else assert(!r.drop_start_ns);if(cancel_stage==2)assert(r.early_drop_end_ns<r.tool_end_ns);}
        run(p,[&](AgentNative *n){cxf_llama_session h;auto *e=agent_native_engine(n,&h);assert(!view(e,h).evaluated);return CXF_OK;});
    }
    assert(agent_tool_prepare_cancel(p)==CXF_ERR_EMPTY);
    {
        OK(agent_tool_prepare_reset(p));f.reset();run(p,[&](AgentNative *n){return agent_native_prepare(n,&f.cfg,f.log.data(),(uint32_t)f.log.size());});
        OK(agent_tool_prepare_options(p,{TOOL_KV_RECOMPUTE,TOOL_PREPARE_PROGRESS,5}));
        OK(agent_tool_prepare_seed(p,1,100,1));OK(agent_tool_prepare_seed_progress(p,1,5000));
        AgentToolPrepareReport report;ProgressGate g{p,&report,{}};char output[8192];uint32_t written;
        std::thread host([&]{OK(agent_tool_prepare_execute(p,1,progress_gate,&g,"",0,output,sizeof output,&written,&report));});
        {std::unique_lock<std::mutex> lock(g.gate.mutex);g.gate.cv.wait(lock,[&]{return g.gate.entered;});}
        OK(agent_tool_prepare_cancel(p));{std::lock_guard<std::mutex> lock(g.gate.mutex);g.gate.release=true;}g.gate.cv.notify_one();host.join();
        assert(report.cancelled&&g.gate.calls==1&&report.progress_ns&&report.prepare_rc==CXF_ERR_BAD_HANDLE);
        assert(agent_tool_prepare_progress(p,report.epoch,tick())==CXF_ERR_BAD_HANDLE);
    }
    OK(agent_tool_prepare_options(p,{TOOL_KV_RESIDENT,TOOL_PREPARE_LATE,5}));
    for(uint64_t i=1;i<=32;i++)OK(agent_tool_prepare_seed(p,i,1,1));assert(agent_tool_prepare_seed(p,33,1,1)==CXF_ERR_FULL);
    OK(agent_tool_prepare_reset(p));f.reset();run(p,[&](AgentNative *n){return agent_native_prepare(n,&f.cfg,f.log.data(),(uint32_t)f.log.size());});
    char result[8192];uint32_t written;AgentToolPrepareReport r;
    assert(agent_tool_prepare_execute(p,1,test_run_exec,&f.test,"bad",3,result,sizeof result,&written,&r)==CXF_ERR_FORMAT);
    assert(r.tool_rc==CXF_ERR_FORMAT);
    assert(agent_tool_prepare_execute(p,1,test_run_exec,&f.test,"",0,result,8,&written,&r)==CXF_ERR_RANGE);
    OK(agent_tool_prepare_options(p,{TOOL_KV_RESTORE,TOOL_PREPARE_IMMEDIATE,5,true}));
    OK(agent_tool_prepare_execute(p,1,[](const void *,const char *,uint32_t,char *,uint32_t,uint32_t *)->cxf_result{throw 1;},nullptr,"",0,result,sizeof result,&written,&r)==CXF_ERR_IO?CXF_OK:CXF_ERR_FORMAT);
    assert(r.tool_rc==CXF_ERR_IO&&r.early_drop_rc==CXF_OK&&!r.drop_start_ns);
    std::strcpy(f.cfg.system,"Environment changed; read actual tool receipts.");
    run(p,[&](AgentNative *n){OK(agent_native_prepare(n,&f.cfg,f.log.data(),(uint32_t)f.log.size()));native_chat_reference(n,&f.cfg,f.log.data(),(uint32_t)f.log.size());return CXF_OK;});
    std::strcpy(f.cfg.system,"Use workspace tools. Report actual test receipts.");
    emit({{"kind","correctness"},{"passed",true},{"borrowed_engine_survives",true},{"cancel_timer_and_native",true},{"tool_error_capacity_once",true},{"environment_change",true},{"progress_epoch_and_cancel",true},{"early_drop_cancel_and_exception",true},{"key_capacity",32}});
}
int main(int argc,char **argv){
    assert(argc==6);bool launch=!std::strcmp(argv[4],"launch")||!std::strcmp(argv[4],"launch-test");bool isolate=!std::strcmp(argv[4],"isolate")||!std::strcmp(argv[4],"isolate-test");bool taskcost=!std::strcmp(argv[4],"taskcost")||!std::strcmp(argv[4],"taskcost-test");bool timing=!std::strcmp(argv[4],"timing");bool balanced=!std::strcmp(argv[4],"balanced")||!std::strcmp(argv[4],"balanced-test");bool bench=!std::strcmp(argv[4],"launch")||!std::strcmp(argv[4],"isolate")||timing||!std::strcmp(argv[4],"balanced")||!std::strcmp(argv[4],"taskcost")||!std::strcmp(argv[4],"bench");int repeats=std::atoi(argv[5]);assert(repeats>=1&&repeats<=30);
    auto params=cxf_llama_default_params();params.model_path=argv[1];params.backend_path=argv[2];params.context_tokens=8192;params.max_sessions=4;params.threads=4;
    AgentToolPrepare *p=nullptr;OK(agent_tool_prepare_open(&params,16,argv[3],1024ull*1024*1024,MODES[0].options,&p));
    emit({{"kind","metadata"},{"context_tokens",8192},{"batch_tokens",params.batch_tokens},{"cpu_threads",4},{"alpha",.09512492197250393},{"margin_ms",5},{"repeats",repeats},{"scripted_actions",true},{"actual_tools",true}});
    {Fixture f(std::string(argv[3])+"/tool-acceptance",2048,true);correctness(p,f);}
    if(launch){
        const Mode calibration[]={
            {"cal_resident",{TOOL_KV_RESIDENT,TOOL_PREPARE_LATE,5},TEST_RUN_SPAWN},
            {"cal_restore",{TOOL_KV_RESTORE,TOOL_PREPARE_LATE,5},TEST_RUN_SPAWN}};
        const Mode modes[]={
            {"resident",{TOOL_KV_RESIDENT,TOOL_PREPARE_LATE,5},TEST_RUN_SPAWN},
            {"restore_progress",{TOOL_KV_RESTORE,TOOL_PREPARE_PROGRESS,5},TEST_RUN_SPAWN},
            {"restore_progress_early",{TOOL_KV_RESTORE,TOOL_PREPARE_PROGRESS,5,true},TEST_RUN_SPAWN},
            {"restore_progress_hold",{TOOL_KV_RESTORE,TOOL_PREPARE_PROGRESS,5,true,true},TEST_RUN_SPAWN},
            {"resident_incremental",{TOOL_KV_RESIDENT,TOOL_PREPARE_LATE,5},TEST_RUN_SPAWN,true,true}};
        Fixture f(std::string(argv[3])+"/tool-fixture",768,true,12000000,true,true,true);
        std::array<std::array<Seed,3>,2> seeds={};
        for(int k=0;k<2;k++){
            std::array<std::vector<double>,3> tools,rates,post;
            for(int rep=0;rep<3;rep++){auto values=trial(p,f,true,768,calibration[k],"calibration",rep,k==0&&rep==0);for(int step=0;step<3;step++){tools[step].push_back(values[step].tool);rates[step].push_back(values[step].rate);post[step].push_back(values[step].post_spawn);}}
            for(int step=0;step<3;step++){std::sort(tools[step].begin(),tools[step].end());std::sort(rates[step].begin(),rates[step].end());std::sort(post[step].begin(),post[step].end());seeds[k][step]={tools[step][1],rates[step][1],post[step][1]};}
        }
        for(int rep=0;rep<(bench?repeats:1);rep++)for(int i=0;i<5;i++){
            int index=(i+rep)%5;const auto &mode=modes[index];int k=index==0||index==4?0:1;auto keybase=(uint64_t)index*3;
            if(!rep)for(int step=0;step<3;step++){OK(agent_tool_prepare_seed(p,keybase+step+1,seeds[k][step].tool,seeds[k][step].rate));OK(agent_tool_prepare_seed_progress(p,keybase+step+1,seeds[k][step].post_spawn));}
            trial(p,f,true,768,mode,bench?"evaluation":"acceptance",rep,false,keybase);
        }
    }else if(isolate){
        const Mode calibration[]={
            {"cal_resident",{TOOL_KV_RESIDENT,TOOL_PREPARE_LATE,5,false,false,true},TEST_RUN_SPAWN},
            {"cal_restore",{TOOL_KV_RESTORE,TOOL_PREPARE_LATE,5,false,false,true},TEST_RUN_SPAWN}};
        const Mode modes[]={
            {"resident",{TOOL_KV_RESIDENT,TOOL_PREPARE_LATE,5,false,false,true},TEST_RUN_SPAWN},
            {"restore_late",{TOOL_KV_RESTORE,TOOL_PREPARE_LATE,5,false,false,true},TEST_RUN_SPAWN},
            {"restore_progress",{TOOL_KV_RESTORE,TOOL_PREPARE_PROGRESS,5,false,false,true},TEST_RUN_SPAWN},
            {"restore_progress_early",{TOOL_KV_RESTORE,TOOL_PREPARE_PROGRESS,5,true,false,true},TEST_RUN_SPAWN},
            {"restore_progress_hold",{TOOL_KV_RESTORE,TOOL_PREPARE_PROGRESS,5,true,true,true},TEST_RUN_SPAWN}};
        Fixture f(std::string(argv[3])+"/tool-fixture",512,true,18000000,true,true);
        std::array<std::array<Seed,3>,2> seeds={};
        for(int k=0;k<2;k++){
            std::array<std::vector<double>,3> tools,rates,post;
            for(int rep=0;rep<3;rep++){auto values=trial(p,f,true,512,calibration[k],"calibration",rep,k==0&&rep==0);for(int step=0;step<3;step++){tools[step].push_back(values[step].tool);rates[step].push_back(values[step].rate);post[step].push_back(values[step].post_spawn);}}
            for(int step=0;step<3;step++){std::sort(tools[step].begin(),tools[step].end());std::sort(rates[step].begin(),rates[step].end());std::sort(post[step].begin(),post[step].end());seeds[k][step]={tools[step][1],rates[step][1],post[step][1]};}
        }
        for(int rep=0;rep<(bench?repeats:1);rep++)for(int i=0;i<5;i++){
            int index=rep<5?(i+rep)%5:((rep-5)-i+5)%5;const auto &mode=modes[index];int k=index==0?0:1;auto keybase=(uint64_t)index*3;
            if(!rep)for(int step=0;step<3;step++){OK(agent_tool_prepare_seed(p,keybase+step+1,seeds[k][step].tool,seeds[k][step].rate));OK(agent_tool_prepare_seed_progress(p,keybase+step+1,seeds[k][step].post_spawn));}
            trial(p,f,true,512,mode,bench?"evaluation":"acceptance",rep,false,keybase);
        }
    }else if(taskcost){
        const Mode calibration[]={
            {"cal_resident",{TOOL_KV_RESIDENT,TOOL_PREPARE_LATE,5},TEST_RUN_SPAWN},
            {"cal_restore",{TOOL_KV_RESTORE,TOOL_PREPARE_LATE,5},TEST_RUN_SPAWN}};
        const Mode modes[]={
            {"resident",{TOOL_KV_RESIDENT,TOOL_PREPARE_LATE,5},TEST_RUN_SPAWN},
            {"restore_timed_noevent",{TOOL_KV_RESTORE,TOOL_PREPARE_TIMED,5},TEST_RUN_SPAWN,false},
            {"restore_timed",{TOOL_KV_RESTORE,TOOL_PREPARE_TIMED,5},TEST_RUN_SPAWN},
            {"restore_progress",{TOOL_KV_RESTORE,TOOL_PREPARE_PROGRESS,5},TEST_RUN_SPAWN},
            {"restore_progress_early",{TOOL_KV_RESTORE,TOOL_PREPARE_PROGRESS,5,true},TEST_RUN_SPAWN}};
        for(bool compile:{false,true})for(int words:{512,2304}){
            Fixture f(std::string(argv[3])+"/tool-fixture",words,compile,18000000,true);
            std::array<std::array<Seed,3>,2> seeds={};
            for(int k=0;k<2;k++){
                std::array<std::vector<double>,3> tools,rates,post;
                for(int rep=0;rep<3;rep++){auto values=trial(p,f,compile,words,calibration[k],"calibration",rep,k==0&&rep==0);for(int step=0;step<3;step++){tools[step].push_back(values[step].tool);rates[step].push_back(values[step].rate);post[step].push_back(values[step].post_spawn);}}
                for(int step=0;step<3;step++){std::sort(tools[step].begin(),tools[step].end());std::sort(rates[step].begin(),rates[step].end());std::sort(post[step].begin(),post[step].end());seeds[k][step]={tools[step][1],rates[step][1],post[step][1]};}
            }
            for(int rep=0;rep<(bench?repeats:1);rep++)for(int i=0;i<5;i++){
                int index=(i+rep)%5;const auto &mode=modes[index];int k=index==0?0:1;auto keybase=(uint64_t)index*3;
                if(!rep)for(int step=0;step<3;step++){OK(agent_tool_prepare_seed(p,keybase+step+1,seeds[k][step].tool,seeds[k][step].rate));if(compile)OK(agent_tool_prepare_seed_progress(p,keybase+step+1,seeds[k][step].post_spawn));}
                trial(p,f,compile,words,mode,bench?"evaluation":"acceptance",rep,false,keybase);
            }
        }
    }else if(balanced){
        const Mode calibration[]={
            {"cal_fork_resident",{TOOL_KV_RESIDENT,TOOL_PREPARE_LATE,5},TEST_RUN_FORK},
            {"cal_fork_restore",{TOOL_KV_RESTORE,TOOL_PREPARE_LATE,5},TEST_RUN_FORK},
            {"cal_spawn_resident",{TOOL_KV_RESIDENT,TOOL_PREPARE_LATE,5},TEST_RUN_SPAWN},
            {"cal_spawn_restore",{TOOL_KV_RESTORE,TOOL_PREPARE_LATE,5},TEST_RUN_SPAWN}};
        const Mode modes[]={
            {"resident",{TOOL_KV_RESIDENT,TOOL_PREPARE_LATE,5},TEST_RUN_SPAWN},
            {"restore_fork_immediate",{TOOL_KV_RESTORE,TOOL_PREPARE_IMMEDIATE,5},TEST_RUN_FORK},
            {"restore_fork_timed",{TOOL_KV_RESTORE,TOOL_PREPARE_TIMED,5},TEST_RUN_FORK},
            {"restore_fork_progress",{TOOL_KV_RESTORE,TOOL_PREPARE_PROGRESS,5},TEST_RUN_FORK},
            {"restore_spawn_immediate",{TOOL_KV_RESTORE,TOOL_PREPARE_IMMEDIATE,5},TEST_RUN_SPAWN},
            {"restore_spawn_timed",{TOOL_KV_RESTORE,TOOL_PREPARE_TIMED,5},TEST_RUN_SPAWN},
            {"restore_spawn_progress",{TOOL_KV_RESTORE,TOOL_PREPARE_PROGRESS,5},TEST_RUN_SPAWN}};
        for(bool compile:{false,true})for(int words:{320,1792}){
            Fixture f(std::string(argv[3])+"/tool-fixture",words,compile,25000000);
            std::array<std::array<Seed,3>,4> seeds={};
            for(int k=0;k<4;k++){
                std::array<std::vector<double>,3> tools,rates,post;
                for(int rep=0;rep<3;rep++){auto values=trial(p,f,compile,words,calibration[k],"calibration",rep,k==0&&rep==0);for(int step=0;step<3;step++){tools[step].push_back(values[step].tool);rates[step].push_back(values[step].rate);post[step].push_back(values[step].post_spawn);}}
                for(int step=0;step<3;step++){std::sort(tools[step].begin(),tools[step].end());std::sort(rates[step].begin(),rates[step].end());std::sort(post[step].begin(),post[step].end());seeds[k][step]={tools[step][1],rates[step][1],post[step][1]};}
            }
            for(int rep=0;rep<(bench?repeats:1);rep++)for(int i=0;i<7;i++){
                int index=(i+rep)%7;const auto &mode=modes[index];int k=index==0?2:(mode.backend==TEST_RUN_FORK?1:3);auto keybase=(uint64_t)index*3;
                if(!rep)for(int step=0;step<3;step++){OK(agent_tool_prepare_seed(p,keybase+step+1,seeds[k][step].tool,seeds[k][step].rate));if(compile)OK(agent_tool_prepare_seed_progress(p,keybase+step+1,seeds[k][step].post_spawn));}
                trial(p,f,compile,words,mode,bench?"evaluation":"acceptance",rep,false,keybase);
            }
        }
    }else{
    std::vector<int> selection=timing?std::vector<int>{0,4,5,6}:std::vector<int>{0,1,2,3,4,5,6};
    for(bool compile:{false,true})for(int words:{128,2048}){
        Fixture f(std::string(argv[3])+"/tool-fixture",words,compile);
        std::array<std::array<Seed,3>,3> seeds={};
        for(int kind=0;kind<3;kind++){
            if(timing&&kind==1)continue;
            auto mode=MODES[kind==0?0:kind==1?1:4];
            std::array<std::vector<double>,3> tools,rates;
            for(int i=0;i<3;i++){auto seed=trial(p,f,compile,words,mode,"calibration",i,kind==0&&i==0);for(int step=0;step<3;step++){tools[step].push_back(seed[step].tool);rates[step].push_back(seed[step].rate);}}
            for(int step=0;step<3;step++){std::sort(tools[step].begin(),tools[step].end());std::sort(rates[step].begin(),rates[step].end());seeds[kind][step]={tools[step][1],rates[step][1]};}
        }
        for(int rep=0;rep<(bench?repeats:1);rep++)for(size_t i=0;i<selection.size();i++){
            int index=selection[(i+rep)%selection.size()];const auto &mode=MODES[index];int kind=(int)mode.options.kv_mode;
            auto keybase=(uint64_t)index*3;
            if(!rep)for(int step=0;step<3;step++)OK(agent_tool_prepare_seed(p,keybase+step+1,seeds[kind][step].tool,seeds[kind][step].rate));
            trial(p,f,compile,words,mode,bench?"evaluation":"acceptance",rep,false,keybase);
        }
    }
    }
    agent_tool_prepare_close(p);
    size_t files=0;for(const auto &entry:std::filesystem::recursive_directory_iterator(argv[3]))if(entry.path().extension()==".kv")files++;
    assert(!files);emit({{"kind","closed"},{"passed",true},{"private_kv_files",0}});
}
