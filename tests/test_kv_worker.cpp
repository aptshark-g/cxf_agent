#include "agent_kv_worker.h"
#include "agent_native_messages.h"
#include "../src/llama_internal.h"
#include "chat.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifdef __APPLE__
#include <mach/mach.h>
#endif

#define OK(x) do {cxf_result r_=(x);if(r_!=CXF_OK){std::fprintf(stderr,"FAIL %d %s rc=%d\n",__LINE__,#x,r_);std::abort();}} while(0)
static uint64_t ns(){return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
static double ms(uint64_t n){return (double)n/1e6;}
static uint64_t rss(){
#ifdef __APPLE__
    mach_task_basic_info_data_t info={};mach_msg_type_number_t count=MACH_TASK_BASIC_INFO_COUNT;
    if(task_info(mach_task_self(),MACH_TASK_BASIC_INFO,(task_info_t)&info,&count)==KERN_SUCCESS)return info.resident_size;
#endif
    return 0;
}
using Fn=std::function<cxf_result(cxf_llama *)>;
static cxf_result owner(AgentKvWorker *p,Fn fn) {
    return agent_kv_worker_run(p,[](cxf_llama *e,void *ctx){return (*(Fn *)ctx)(e);},&fn);
}
static cxf_llama_view view(cxf_llama *e,cxf_llama_session h){cxf_llama_view v;OK(cxf_llama_session_view(e,h,&v));return v;}
static std::vector<int32_t> tokens(cxf_llama *e,const std::string &s){
    uint32_t n=0;auto rc=cxf_llama_tokenize(e,s.data(),(uint32_t)s.size(),false,nullptr,0,&n);assert(rc==CXF_OK || rc==CXF_ERR_FULL);
    std::vector<int32_t> t(n);OK(cxf_llama_tokenize(e,s.data(),(uint32_t)s.size(),false,t.data(),n,&n));return t;
}
static void fill(cxf_llama *e,cxf_llama_session h){uint32_t n;OK(cxf_llama_prefill(e,h,view(e,h).revision,UINT32_MAX,true,&n));}
struct Fixture {
    AgentLlmCfg cfg={};char tools[4096];std::unique_ptr<common_chat_template> tmpl;
    common_json tool_values;std::string log,final_log,prefix,expected_prompt;
    std::vector<int32_t> prefix_tokens,full_tokens;
    cxf_llama_session parent={};uint64_t revision=0;AgentKvSnapshot snapshot=0;
    int32_t expected_token=0;double save_ms=0;int words=0,records=0,history=0;
};
static std::string assemble(Fixture &f,const std::string &log) {
    jinja::value_array values;OK(agent_native_collect_values(&f.cfg,log.data(),(uint32_t)log.size(),&values));
    return agent_native_render_values(*f.tmpl,std::move(values),f.tool_values);
}
static std::unique_ptr<Fixture> fixture(AgentKvWorker *p,int words,int records,int history=0) {
    auto f=std::make_unique<Fixture>();f->words=words;f->records=records;f->history=history;
    std::strcpy(f->cfg.model,"native-qwen3");std::strcpy(f->cfg.system,"Use tools.");
    AgentToolSpec spec={"fs.read","Read file","path","contents"};f->cfg.tools=f->tools;
    f->cfg.tools_len=agent_llm_render_tools(&spec,1,f->tools,sizeof f->tools);
    auto tools=common_chat_tools_parse_oaicompat(common_json::parse(std::string(f->tools,f->cfg.tools_len)));
    f->tool_values=common_chat_tools_to_json_oaicompat(tools);
    std::string source;
    OK(owner(p,[&](cxf_llama *e){source=llama_model_chat_template(cxf_llama_native_model(e),nullptr);return CXF_OK;}));
    f->tmpl.reset(new common_chat_template(source,"",""));
    f->log="context\n";for(int i=0;i<words;i++)f->log+="alpha ";f->log+='\n';
    for(int i=0;i<history;i++)f->log+="#call fs.read 3\na.c\n#result fs.read 64B\n"+std::string(64,'x')+"\n";
    auto initial=assemble(*f,f->log);auto boundary=initial.rfind("<|im_start|>assistant\n");assert(boundary!=std::string::npos);
    f->prefix=initial.substr(0,boundary);
    f->final_log=f->log;
    for(int i=0;i<records;i++)f->final_log+="#call fs.read 3\na.c\n#result fs.read 64B\n"+std::string(64,'x')+"\n";
    f->expected_prompt=assemble(*f,f->final_log);
    assert(f->expected_prompt.compare(0,f->prefix.size(),f->prefix)==0);
    OK(owner(p,[&](cxf_llama *e){
        f->prefix_tokens=tokens(e,f->prefix);f->full_tokens=tokens(e,f->expected_prompt);
        assert(f->full_tokens.size()>f->prefix_tokens.size());
        assert(std::equal(f->prefix_tokens.begin(),f->prefix_tokens.end(),f->full_tokens.begin()));
        cxf_llama_session reference;OK(cxf_llama_session_new(e,&reference));
        OK(cxf_llama_append(e,reference,1,f->full_tokens.data(),(uint32_t)f->full_tokens.size()));fill(e,reference);
        OK(cxf_llama_greedy(e,reference,view(e,reference).revision,&f->expected_token));OK(cxf_llama_session_close(e,reference));
        OK(cxf_llama_session_new(e,&f->parent));
        OK(cxf_llama_append(e,f->parent,1,f->prefix_tokens.data(),(uint32_t)f->prefix_tokens.size()));fill(e,f->parent);
        f->revision=view(e,f->parent).revision;return CXF_OK;
    }));
    auto begin=ns();OK(agent_kv_worker_save(p,f->parent,f->revision,&f->snapshot));f->save_ms=ms(ns()-begin);
    return f;
}
struct Commit {Fixture *f;const std::string *prompt;bool called=false;int32_t token=0;uint64_t decode_tokens=0,first_token_ns=0;};
static cxf_result commit(cxf_llama *e,void *ctx) {
    auto &c=*(Commit *)ctx;c.called=true;auto &f=*c.f;
    assert(c.prompt->compare(0,f.prefix.size(),f.prefix)==0);
    auto suffix=tokens(e,c.prompt->substr(f.prefix.size()));
    assert(suffix.size()+f.prefix_tokens.size()==f.full_tokens.size());
    assert(std::equal(suffix.begin(),suffix.end(),f.full_tokens.begin()+f.prefix_tokens.size()));
    cxf_llama_stats before,after;cxf_llama_get_stats(e,&before);
    cxf_llama_session child;OK(cxf_llama_fork(e,f.parent,f.revision,(uint32_t)f.prefix_tokens.size(),&child));
    OK(cxf_llama_append(e,child,view(e,child).revision,suffix.data(),(uint32_t)suffix.size()));fill(e,child);
    OK(cxf_llama_greedy(e,child,view(e,child).revision,&c.token));c.first_token_ns=ns();assert(c.token==f.expected_token);
    cxf_llama_get_stats(e,&after);c.decode_tokens=after.evaluated_tokens-before.evaluated_tokens;
    assert(c.decode_tokens==suffix.size());
    OK(cxf_llama_session_close(e,child));return CXF_OK;
}
static cxf_result finish_retry(AgentKvWorker *p,AgentKvTicket ticket,AgentKvReport *report=nullptr) {
    cxf_result rc;
    do {rc=agent_kv_worker_finish(p,ticket,nullptr,nullptr,report);if(rc==CXF_ERR_FULL)std::this_thread::yield();}while(rc==CXF_ERR_FULL);
    return rc;
}
static void check_parent(AgentKvWorker *p,Fixture &f,uint32_t evaluated) {
    OK(owner(p,[&](cxf_llama *e){auto v=view(e,f.parent);assert(v.revision==f.revision && v.evaluated==evaluated);
        std::vector<int32_t> actual(v.committed);OK(cxf_llama_read(e,f.parent,0,actual.data(),v.committed));assert(actual==f.prefix_tokens);
        cxf_llama_stats s;cxf_llama_get_stats(e,&s);assert(s.logical_tokens==f.prefix_tokens.size());
        assert(s.stored_tokens>=f.prefix_tokens.size() && s.stored_tokens<f.prefix_tokens.size()+256);return CXF_OK;}));
}
struct Block {
    std::mutex mutex;std::condition_variable cv;bool entered=false,release=false;
    void wait(){std::unique_lock<std::mutex> lock(mutex);cv.wait(lock,[&]{return entered;});}
    void unblock(){std::lock_guard<std::mutex> lock(mutex);release=true;cv.notify_one();}
    cxf_result run(){std::unique_lock<std::mutex> lock(mutex);entered=true;cv.notify_one();cv.wait(lock,[&]{return release;});return CXF_OK;}
};
static std::vector<std::filesystem::path> kv_files(const char *root){
    std::vector<std::filesystem::path> out;
    for(const auto &x:std::filesystem::recursive_directory_iterator(root))if(x.path().extension()==".kv")out.push_back(x.path());
    return out;
}
static void correctness(AgentKvWorker *p,const char *root) {
    auto f=fixture(p,256,1);AgentKvTicket ticket;AgentKvReport report;
    Commit c{f.get(),&f->expected_prompt};
    OK(agent_kv_worker_prepare(p,f->snapshot,&ticket));OK(agent_kv_worker_finish(p,ticket,commit,&c,&report));assert(c.called && !report.restored_bytes);
    OK(agent_kv_worker_evict(p,f->parent,f->revision));check_parent(p,*f,0);
    OK(agent_kv_worker_prepare(p,f->snapshot,&ticket));c.called=false;
    OK(agent_kv_worker_finish(p,ticket,commit,&c,&report));assert(c.called && report.restored_tokens==f->prefix_tokens.size() && report.restored_bytes);
    assert(report.queued_ns<=report.started_ns && report.started_ns<=report.ready_ns && report.ready_ns<=report.finished_ns);
    check_parent(p,*f,(uint32_t)f->prefix_tokens.size());
    auto files=kv_files(root);assert(files.size()==1);
    {std::ifstream in(files[0],std::ios::binary);uint32_t header[3];in.read((char *)header,sizeof header);assert(in && header[2]==0);}
    AgentKvSnapshot extra[7];for(auto &id:extra)OK(agent_kv_worker_save(p,f->parent,f->revision,&id));
    AgentKvSnapshot excess=99;assert(agent_kv_worker_save(p,f->parent,f->revision,&excess)==CXF_ERR_FULL && !excess);
    assert(kv_files(root).size()==8);
    for(auto id:extra)OK(agent_kv_worker_unlink_hold(p,id));
    assert(kv_files(root).size()==1);
    assert(agent_kv_worker_save(p,f->parent,f->revision,&excess)==CXF_ERR_FULL&&!excess);
    for(auto id:extra){
        assert(agent_kv_worker_unlink_hold(p,id)==CXF_ERR_BAD_HANDLE);
        OK(agent_kv_worker_prepare(p,id,&ticket));assert(finish_retry(p,ticket)==CXF_ERR_BAD_HANDLE);
        OK(agent_kv_worker_drop(p,id));assert(agent_kv_worker_drop(p,id)==CXF_ERR_BAD_HANDLE);
    }
    // Deterministically fill the bounded queue while the owner is occupied.
    OK(agent_kv_worker_evict(p,f->parent,f->revision));Block block;
    std::thread blocker([&]{OK(owner(p,[&](cxf_llama *){return block.run();}));});block.wait();
    AgentKvTicket pending[16];for(auto &id:pending){OK(agent_kv_worker_prepare(p,f->snapshot,&id));OK(agent_kv_worker_cancel(p,id));}
    AgentKvTicket overflow=99;assert(agent_kv_worker_prepare(p,f->snapshot,&overflow)==CXF_ERR_FULL && !overflow);
    block.unblock();blocker.join();
    for(auto id:pending){assert(finish_retry(p,id,&report)==CXF_ERR_BAD_HANDLE);assert(report.cancelled && !report.restored_bytes);}
    check_parent(p,*f,0);
    // Cancel completed preparation before final commit; it must not execute.
    OK(agent_kv_worker_prepare(p,f->snapshot,&ticket));OK(owner(p,[](cxf_llama *){return CXF_OK;}));
    assert(agent_kv_worker_drop(p,f->snapshot)==CXF_ERR_FULL);OK(agent_kv_worker_cancel(p,ticket));c.called=false;
    assert(agent_kv_worker_unlink_hold(p,f->snapshot)==CXF_ERR_FULL);
    assert(agent_kv_worker_finish(p,ticket,commit,&c,&report)==CXF_ERR_BAD_HANDLE && !c.called);
    assert(report.cancelled && report.restored_bytes==report.wasted_bytes && report.wasted_bytes);check_parent(p,*f,0);
    assert(agent_kv_worker_finish(p,ticket,nullptr,nullptr,nullptr)==CXF_ERR_BAD_HANDLE);
    // Cancel after native restore has started. It is allowed to finish the
    // native call, but no old completion or restored KV may be published.
    OK(agent_kv_worker_prepare(p,f->snapshot,&ticket));bool ready=false;auto deadline=ns()+3000000000ull;
    do {OK(agent_kv_worker_poll(p,ticket,&report,&ready));if(!report.restore_start_ns)std::this_thread::yield();}
    while(!report.restore_start_ns && !ready && ns()<deadline);
    assert(report.restore_start_ns && !ready);OK(agent_kv_worker_cancel(p,ticket));c.called=false;
    assert(agent_kv_worker_finish(p,ticket,commit,&c,&report)==CXF_ERR_BAD_HANDLE && !c.called);
    assert(report.cancelled && report.wasted_bytes && report.restored_bytes==report.wasted_bytes);check_parent(p,*f,0);
    // An edit after preparation cannot publish a commit for an old revision.
    OK(agent_kv_worker_prepare(p,f->snapshot,&ticket));OK(owner(p,[&](cxf_llama *e){
        int32_t token=f->prefix_tokens[0];return cxf_llama_append(e,f->parent,f->revision,&token,1);}));
    c.called=false;assert(agent_kv_worker_finish(p,ticket,commit,&c,&report)==CXF_ERR_BAD_HANDLE && !c.called);
    OK(agent_kv_worker_prepare(p,f->snapshot,&ticket));assert(finish_retry(p,ticket)==CXF_ERR_BAD_HANDLE);
    OK(agent_kv_worker_drop(p,f->snapshot));
    OK(owner(p,[&](cxf_llama *e){OK(cxf_llama_replace(e,f->parent,view(e,f->parent).revision,0,f->prefix_tokens.data(),(uint32_t)f->prefix_tokens.size()));
        fill(e,f->parent);f->revision=view(e,f->parent).revision;return CXF_OK;}));
    OK(agent_kv_worker_save(p,f->parent,f->revision,&f->snapshot));
    // Private-file truncation is rejected before native restore touches KV.
    files=kv_files(root);assert(files.size()==1);std::filesystem::resize_file(files[0],12);
    OK(agent_kv_worker_evict(p,f->parent,f->revision));OK(agent_kv_worker_prepare(p,f->snapshot,&ticket));
    c.called=false;assert(agent_kv_worker_finish(p,ticket,commit,&c,&report)==CXF_ERR_FORMAT && !c.called);check_parent(p,*f,0);
    OK(agent_kv_worker_drop(p,f->snapshot));
    OK(owner(p,[&](cxf_llama *e){fill(e,f->parent);return CXF_OK;}));OK(agent_kv_worker_save(p,f->parent,f->revision,&f->snapshot));
    // Closed/reused session slots cannot receive old KV or a final callback.
    OK(agent_kv_worker_prepare(p,f->snapshot,&ticket));cxf_llama_session replacement={};
    OK(owner(p,[&](cxf_llama *e){OK(cxf_llama_session_close(e,f->parent));OK(cxf_llama_session_new(e,&replacement));assert(view(e,replacement).committed==0);return CXF_OK;}));
    c.called=false;assert(agent_kv_worker_finish(p,ticket,commit,&c,&report)==CXF_ERR_BAD_HANDLE && !c.called);
    OK(owner(p,[&](cxf_llama *e){assert(view(e,replacement).committed==0);return cxf_llama_session_close(e,replacement);}));
    OK(agent_kv_worker_drop(p,f->snapshot));
    OK(agent_kv_worker_prepare(p,UINT64_MAX,&ticket));assert(finish_retry(p,ticket)==CXF_ERR_BAD_HANDLE);
    assert(kv_files(root).empty());
    std::puts("{\"kind\":\"correctness\",\"passed\":true,\"cases\":[\"resident\",\"disk_roundtrip\",\"token_and_greedy_oracle\",\"canonical_single_owner\",\"queue_bound\",\"snapshot_bound\",\"queued_cancel\",\"inflight_cancel\",\"ready_cancel\",\"drop_in_use\",\"stale_revision\",\"slot_reuse\",\"truncated_snapshot\",\"unknown_snapshot\",\"no_token_list\"]}");
}
static void trials(AgentKvWorker *p,int words,int records,int window_ms,int repeats,int history=0) {
    auto f=fixture(p,words,records,history);
    std::printf("{\"kind\":\"snapshot\",\"words\":%d,\"records\":%d,\"history_records\":%d,\"prefix_tokens\":%zu,\"full_tokens\":%zu,\"save_ms\":%.6f}\n",words,records,history,f->prefix_tokens.size(),f->full_tokens.size(),f->save_ms);
    for(int rep=-1;rep<repeats;rep++)for(int variant=0;variant<2;variant++) {
        bool early=(rep+variant+2)%2!=0;
        OK(agent_kv_worker_evict(p,f->parent,f->revision));
        auto rss_before=rss();AgentKvTicket ticket=0;auto start=ns();
        if(early)OK(agent_kv_worker_prepare(p,f->snapshot,&ticket));
        if(window_ms)std::this_thread::sleep_for(std::chrono::milliseconds(window_ms)); // labelled controlled window
        auto assembly_start=ns();auto prompt=assemble(*f,f->final_log);auto assembled=ns();
        assert(prompt==f->expected_prompt);
        if(!early)OK(agent_kv_worker_prepare(p,f->snapshot,&ticket));
        Commit c{f.get(),&prompt};AgentKvReport report;auto submit=ns();
        OK(agent_kv_worker_finish(p,ticket,commit,&c,&report));auto returned=ns();
        assert(c.called && c.token==f->expected_token && report.restored_tokens==f->prefix_tokens.size());
        check_parent(p,*f,(uint32_t)f->prefix_tokens.size());
        if(rep>=0) {
            uint64_t overlap_begin=std::max(start,report.started_ns),overlap_end=std::min(assembled,report.ready_ns);
            auto overlap=overlap_end>overlap_begin?overlap_end-overlap_begin:0;
            auto residual=report.ready_ns>submit?report.ready_ns-submit:0;
            std::printf("{\"kind\":\"timing\",\"scenario\":\"%s\",\"words\":%d,\"records\":%d,\"history_records\":%d,\"window_ms\":%d,\"rep\":%d,\"mode\":\"%s\",\"prefix_tokens\":%zu,\"suffix_decode_tokens\":%llu,\"restored_bytes\":%llu,\"rss_before\":%llu,\"rss_after\":%llu,\"correct\":true,\"start_ns\":%llu,\"assembly_start_ns\":%llu,\"assembled_ns\":%llu,\"queued_ns\":%llu,\"prepare_start_ns\":%llu,\"restore_start_ns\":%llu,\"restore_end_ns\":%llu,\"ready_ns\":%llu,\"submit_ns\":%llu,\"first_token_ns\":%llu,\"assembly_ms\":%.6f,\"queue_ms\":%.6f,\"verify_ms\":%.6f,\"restore_ms\":%.6f,\"overlap_ms\":%.6f,\"post_submit_prepare_ms\":%.6f,\"post_submit_first_token_ms\":%.6f,\"total_first_token_ms\":%.6f,\"return_overhead_ms\":%.6f}\n",
                window_ms?"controlled_wait":"real_assembly",words,records,history,window_ms,rep,early?"early_hint":"late_hint",f->prefix_tokens.size(),
                (unsigned long long)c.decode_tokens,(unsigned long long)report.restored_bytes,(unsigned long long)rss_before,(unsigned long long)rss(),
                (unsigned long long)start,(unsigned long long)assembly_start,(unsigned long long)assembled,(unsigned long long)report.queued_ns,
                (unsigned long long)report.started_ns,(unsigned long long)report.restore_start_ns,(unsigned long long)report.restore_end_ns,
                (unsigned long long)report.ready_ns,(unsigned long long)submit,(unsigned long long)c.first_token_ns,
                ms(assembled-assembly_start),ms(report.started_ns-report.queued_ns),ms(report.verify_ns),ms(report.restore_ns),ms(overlap),ms(residual),ms(c.first_token_ns-submit),ms(c.first_token_ns-start),ms(returned-report.finished_ns));
            std::fflush(stdout);
        }
    }
    OK(agent_kv_worker_drop(p,f->snapshot));OK(owner(p,[&](cxf_llama *e){return cxf_llama_session_close(e,f->parent);}));
}
int main(int argc,char **argv) {
    assert(argc==6);const char *root=argv[3];bool benchmark=std::strcmp(argv[4],"bench")==0;int repeats=std::atoi(argv[5]);assert(repeats>=1 && repeats<=30);
    auto params=cxf_llama_default_params();params.model_path=argv[1];params.backend_path=argv[2];params.context_tokens=8192;params.max_sessions=4;
    // A tiny disk budget must fail before creating a snapshot or evicting KV.
    AgentKvWorker *p;OK(agent_kv_worker_open(&params,root,1,&p));
    cxf_llama_session h={};uint64_t revision=0;
    OK(owner(p,[&](cxf_llama *e){auto t=tokens(e,"Budget test.");OK(cxf_llama_session_new(e,&h));OK(cxf_llama_append(e,h,1,t.data(),(uint32_t)t.size()));fill(e,h);revision=view(e,h).revision;return CXF_OK;}));
    AgentKvSnapshot rejected=99;assert(agent_kv_worker_save(p,h,revision,&rejected)==CXF_ERR_FULL && !rejected);
    OK(owner(p,[&](cxf_llama *e){assert(view(e,h).evaluated);return CXF_OK;}));agent_kv_worker_close(p);assert(kv_files(root).empty());
    OK(agent_kv_worker_open(&params,root,1024ull*1024*1024,&p));correctness(p,root);
    if(benchmark) {
        trials(p,4096,1,0,repeats);trials(p,1024,128,0,repeats);
        trials(p,1024,1,0,repeats,128); // dense existing history, only one new tool result
        for(int window:{25,100,250})trials(p,4096,1,window,repeats);
    }
    // Closing also drains retained unlinked fds, in addition to live tickets.
    auto f=fixture(p,64,1);OK(agent_kv_worker_evict(p,f->parent,f->revision));AgentKvTicket pending;
    AgentKvSnapshot held;OK(owner(p,[&](cxf_llama *e){fill(e,f->parent);return CXF_OK;}));
    OK(agent_kv_worker_save(p,f->parent,f->revision,&held));OK(agent_kv_worker_unlink_hold(p,held));
    OK(agent_kv_worker_evict(p,f->parent,f->revision));
    OK(agent_kv_worker_prepare(p,f->snapshot,&pending));agent_kv_worker_close(p);
    assert(kv_files(root).empty());
    std::puts("{\"kind\":\"closed\",\"private_kv_files\":0,\"passed\":true,\"disk_budget_rejection\":true}");
}
