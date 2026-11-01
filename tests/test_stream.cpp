#include "agent_stream.h"
extern "C" {
#include "agent_basic.h"
}
#include "agent_native_messages.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#define OK(x) do{auto rc_=(x);if(rc_!=CXF_OK){std::fprintf(stderr,"FAIL %d %s rc=%d\n",__LINE__,#x,rc_);std::abort();}}while(0)
static uint64_t now(){return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
static double ms(uint64_t n){return (double)n/1e6;}
static void until(uint64_t t){std::this_thread::sleep_until(std::chrono::steady_clock::time_point(std::chrono::nanoseconds(t)));}
using Owner=std::function<cxf_result(cxf_llama *)>;
static cxf_result owner(AgentKvWorker *w,Owner fn){return agent_kv_worker_run(w,[](cxf_llama *e,void *v){return (*(Owner *)v)(e);},&fn);}
static std::string line(uint32_t seq,const char *kind,const std::string &text="",const char *id=nullptr,uint32_t version=1){
    common_json j;j["seq"]=seq;j["kind"]=kind;if(std::strcmp(kind,"text")==0 || std::strcmp(kind,"file_chunk")==0)j["text"]=text;
    if(id){j["id"]=id;j["version"]=version;}return j.dump()+"\n";
}
static cxf_result transfer(AgentStream *s,const std::string &wire,size_t split=7){
    size_t offset=0;bool retry=false;
    do {
        uint32_t used=0;auto size=(uint32_t)std::min(split,wire.size()-offset);
        auto rc=agent_stream_feed(s,size?wire.data()+offset:nullptr,size,&used);offset+=used;
        if(rc==CXF_ERR_FULL){AgentStreamReport report;agent_stream_report(s,&report);if(report.failed)return rc;
            do {rc=agent_stream_wait_input(s,100);}while(rc==CXF_ERR_EMPTY);if(rc!=CXF_OK)return rc;retry=true;
        }else{if(rc!=CXF_OK)return rc;retry=false;}
    }while(offset<wire.size() || retry);
    return CXF_OK;
}
static void feed(AgentStream *s,const std::string &wire,size_t split=7){OK(transfer(s,wire,split));}
struct Output {
    bool called=false,full_reference=false;int32_t first=0;uint64_t first_ns=0;std::vector<int32_t> canonical;
    std::string prompt,preview;bool ended=false;
};
static cxf_result summarize(cxf_llama *e,cxf_llama_session h,const char *prompt,uint32_t bytes,void *ctx){
    auto &out=*(Output *)ctx;out.called=true;out.prompt.assign(prompt,bytes);
    cxf_llama_view v;OK(cxf_llama_session_view(e,h,&v));assert(v.committed==v.evaluated);
    out.canonical.resize(v.committed);OK(cxf_llama_read(e,h,0,out.canonical.data(),v.committed));
    // Independent one-shot tokenization of the final complete rendered prompt.
    std::vector<int32_t> full(bytes+16);uint32_t n=0;
    OK(cxf_llama_tokenize(e,prompt,bytes,false,full.data(),(uint32_t)full.size(),&n));full.resize(n);assert(full==out.canonical);
    int32_t expected=0;
    if(out.full_reference){
        cxf_llama_session ref;OK(cxf_llama_session_new(e,&ref));OK(cxf_llama_append(e,ref,1,full.data(),n));
        cxf_llama_view r;OK(cxf_llama_session_view(e,ref,&r));uint32_t processed;
        OK(cxf_llama_prefill(e,ref,r.revision,UINT32_MAX,true,&processed));OK(cxf_llama_greedy(e,ref,r.revision,&expected));OK(cxf_llama_session_close(e,ref));
    }
    cxf_llama_session branch;OK(cxf_llama_fork(e,h,v.revision,v.committed,&branch));
    // Timing records real greedy sampling; the full-token oracle above is test-only
    // work and excluded by model_ready_ns, but included in callback duration.
    for(int i=0;i<16;i++){
        cxf_llama_view b;OK(cxf_llama_session_view(e,branch,&b));int32_t t;
        OK(cxf_llama_greedy(e,branch,b.revision,&t));if(i==0){out.first=t;out.first_ns=now();}
        if(i==0 && out.full_reference)assert(t==expected);
        if(cxf_llama_is_eog(e,t)){out.ended=true;break;}
        char piece[256];uint32_t count=0;OK(cxf_llama_piece(e,t,piece,sizeof piece,&count));out.preview.append(piece,count);
        OK(cxf_llama_append(e,branch,b.revision,&t,1));OK(cxf_llama_session_view(e,branch,&b));uint32_t processed;
        OK(cxf_llama_prefill(e,branch,b.revision,1,true,&processed));
    }
    OK(cxf_llama_session_close(e,branch));return CXF_OK;
}
struct Tools {
    std::string root;std::atomic<unsigned> calls{0};
    std::mutex mutex;std::condition_variable cv;bool block_one=false,entered=false,release=false;
};
static cxf_result compile_unit(const AgentStreamUnit *unit,void *ctx,char *out,uint32_t cap,uint32_t *written){
    auto &tools=*(Tools *)ctx;tools.calls++;
    if(tools.block_one && unit->version==1){std::unique_lock<std::mutex> lock(tools.mutex);tools.entered=true;tools.cv.notify_all();tools.cv.wait(lock,[&]{return tools.release;});}
    auto dir=std::filesystem::path(tools.root)/(std::string(unit->id)+"-"+std::to_string(unit->version)+"-"+std::to_string(unit->end_seq));
    std::filesystem::create_directory(dir);
    {std::ofstream file(dir/"unit.c",std::ios::binary);file.write(unit->text,unit->bytes);if(!file)return CXF_ERR_IO;}
    auto path=dir.string();TestRunCfg cfg={path.c_str(),"cc unit.c -o unit && ./unit",10000};char receipt[BASIC_OUT_HINT];uint32_t n=0;
    auto rc=test_run_exec(&cfg,"",0,receipt,sizeof receipt,&n);
    if(n>cap)return CXF_ERR_RANGE;std::memcpy(out,receipt,n);*written=n;return rc;
}
static cxf_result observe_unit(const AgentStreamUnit *unit,void *,char *out,uint32_t cap,uint32_t *written){
    int n=std::snprintf(out,cap,"read_only unit_bytes=%u\n",unit->bytes);if(n<0 || (uint32_t)n>=cap)return CXF_ERR_RANGE;*written=(uint32_t)n;return CXF_OK;
}
struct Run {
    AgentStreamReport report={};Output output;std::vector<AgentStreamReceipt> receipts;std::vector<std::string> receipt_text;
    uint64_t start=0,returned=0;unsigned tools=0;
};
static std::string initial(){std::string s="Prior task context:\n";for(int i=0;i<512;i++)s+="alpha ";return s;}
static AgentStreamParams parameters(Output &out,Tools &tools,int mode,const std::string &prefix){
    AgentStreamParams p={};p.initial_context=prefix.data();p.initial_bytes=(uint32_t)prefix.size();p.prefill_budget=512;
    p.defer_until_end=mode==0;p.flush_each_update=mode==2;p.process_unit=compile_unit;p.unit_ctx=&tools;p.finalize=summarize;p.final_ctx=&out;return p;
}
static void collect(AgentStream *s,Run &r){
    for(uint32_t i=0;;i++){AgentStreamReceipt receipt;char body[4096];uint32_t n=0;auto rc=agent_stream_receipt(s,i,&receipt,body,sizeof body,&n);
        if(rc==CXF_ERR_RANGE)break;OK(rc);r.receipts.push_back(receipt);r.receipt_text.emplace_back(body,n);}
}
static Run execute(AgentKvWorker *w,const char *root,const std::vector<std::string> &events,int gap,int mode,size_t split=7,bool full_reference=false){
    Run r;r.output.full_reference=full_reference;Tools tools;tools.root=root;auto prefix=initial();auto p=parameters(r.output,tools,mode,prefix);AgentStream *s;OK(agent_stream_open(w,&p,&s));
    r.start=now();for(size_t i=0;i<events.size();i++){
        // end is delivered immediately after the last payload, with no free idle interval.
        auto arrival=std::min(i+1,events.size()-1);until(r.start+(uint64_t)gap*arrival*1000000);feed(s,events[i],split);
    }
    OK(agent_stream_finish(s,&r.report));r.returned=now();collect(s,r);r.tools=tools.calls;agent_stream_close(s);
    assert(r.output.called && r.report.done && !r.report.failed && !r.report.cancelled);return r;
}
static void no_sessions(AgentKvWorker *w){OK(owner(w,[](cxf_llama *e){cxf_llama_stats s;cxf_llama_get_stats(e,&s);assert(!s.sessions && !s.logical_tokens && !s.stored_tokens && !s.token_payload_capacity);return CXF_OK;}));}
static void invalid(AgentKvWorker *w,const char *root,const std::string &wire,cxf_result expected,bool eof=false){
    Output out;Tools tools;tools.root=root;auto prefix=initial();auto p=parameters(out,tools,1,prefix);AgentStream *s;OK(agent_stream_open(w,&p,&s));
    uint32_t used=0;auto rc=agent_stream_feed(s,wire.data(),(uint32_t)wire.size(),&used);
    if(eof)assert(rc==CXF_OK);else assert(rc==expected);
    AgentStreamReport report;assert(agent_stream_finish(s,&report)==expected);assert(!out.called && !tools.calls);
    agent_stream_close(s);no_sessions(w);
}
static void feedback(AgentKvWorker *w,const char *root){
    // Real two-round feedback: receive a failed test receipt before ending
    // the source, then send the corrected version and observe a passing test.
    Output out;Tools tools;tools.root=root;auto prefix=initial();auto p=parameters(out,tools,1,prefix);AgentStream *s;OK(agent_stream_open(w,&p,&s));
    feed(s,line(1,"file_chunk","int main(void){return 1;}\n","feedback.c",1));feed(s,line(2,"file_end","","feedback.c",1));
    AgentStreamReceipt first_receipt;char first_body[4096];uint32_t first_size=0;auto deadline=now()+10000000000ull;cxf_result receipt_rc;
    do {receipt_rc=agent_stream_receipt(s,0,&first_receipt,first_body,sizeof first_body,&first_size);if(receipt_rc==CXF_ERR_EMPTY)std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    while(receipt_rc==CXF_ERR_EMPTY && now()<deadline);
    OK(receipt_rc);assert(!first_receipt.stale && std::string(first_body,first_size).find("exit=1")!=std::string::npos && !out.called);
    feed(s,line(3,"file_chunk","int main(void){return 0;}\n","feedback.c",2));feed(s,line(4,"file_end","","feedback.c",2));feed(s,line(5,"end"));
    AgentStreamReport report;OK(agent_stream_finish(s,&report));assert(report.tool_completed==2 && report.stale_receipts==1);
    AgentStreamReceipt last_receipt;OK(agent_stream_receipt(s,1,&last_receipt,first_body,sizeof first_body,&first_size));assert(!last_receipt.stale && std::string(first_body,first_size).find("exit=0")!=std::string::npos);
    agent_stream_close(s);no_sessions(w);
}
static void correctness(AgentKvWorker *w,const char *root){
    std::vector<std::string> source={line(1,"text","中文🙂\nquotes \" and backslash \\ and NUL "+std::string(1,'\0')),
        line(2,"file_chunk","int main(void) {\n","main.c"),line(3,"file_chunk","return 0; }\n","main.c"),line(4,"file_end","","main.c"),line(5,"end")};
    auto batch=execute(w,root,source,0,0,1,true);auto stream=execute(w,root,source,0,1,1,true);auto eager=execute(w,root,source,0,2,4096,true);
    assert(batch.output.canonical==stream.output.canonical && batch.output.canonical==eager.output.canonical);
    assert(batch.output.first==stream.output.first && batch.output.first==eager.output.first);
    assert(stream.tools==1 && stream.receipt_text.size()==1 && stream.receipt_text[0].find("exit=0 signal=0 timeout=0 io_error=0")!=std::string::npos);
    no_sessions(w);
    feedback(w,root);
    // Partial JSON/UTF-8 never becomes a tool event. End without file_end fails.
    invalid(w,root,line(2,"text","out of order"),CXF_ERR_FORMAT);
    invalid(w,root,"{\"seq\":1,\"kind\":\"text\",\"text\":\"\xc0\xaf\"}\n",CXF_ERR_FORMAT);
    invalid(w,root,"{\"seq\":1,\"kind\":\"text\",\"text\":\"incomplete",CXF_ERR_FORMAT,true);
    invalid(w,root,line(1,"file_end","","main.c"),CXF_ERR_FORMAT);
    invalid(w,root,line(1,"file_chunk","int main(void){","main.c")+line(2,"end"),CXF_ERR_FORMAT);
    invalid(w,root,line(1,"file_chunk","x","../bad.c"),CXF_ERR_FORMAT);
    invalid(w,root,line(1,"text","ok"),CXF_ERR_FORMAT,true);
    invalid(w,root,std::string(16385,'x'),CXF_ERR_FULL);
    invalid(w,root,line(1,"end")+line(2,"text","late"),CXF_ERR_FORMAT);
    invalid(w,root,line(1,"text","one")+line(1,"text","duplicate"),CXF_ERR_FORMAT);
    invalid(w,root,"{\"seq\":1,\"kind\":\"file_chunk\",\"id\":\"x.c\",\"version\":0,\"text\":\"x\"}\n",CXF_ERR_FORMAT);
    // Terminal capacity limits are distinct from transient backlog pressure.
    for(int limit=0;limit<3;limit++){
        Output o;Tools t;t.root=root;auto pre=initial();auto conf=parameters(o,t,0,pre);AgentStream *x;OK(agent_stream_open(w,&conf,&x));
        cxf_result rc=CXF_OK;
        for(uint32_t seq=1;seq<=129 && rc==CXF_OK;seq++){
            auto wire=limit==0?line(seq,"text",std::string(8192,'q')):limit==1?line(seq,"text",""):line(seq,"file_chunk","x",("file"+std::to_string(seq)+".c").c_str());
            uint32_t used=0;rc=agent_stream_feed(x,wire.data(),(uint32_t)wire.size(),&used);
        }
        assert(rc==CXF_ERR_FULL);AgentStreamReport report;agent_stream_report(x,&report);assert(report.failed && report.source_bytes<=65536);
        assert(agent_stream_finish(x,&report)==CXF_ERR_FULL && !o.called && !t.calls);agent_stream_close(x);no_sessions(w);
    }
    // A live old-version test is allowed to finish, but its result is stale.
    Output out;Tools tools;tools.root=root;tools.block_one=true;auto prefix=initial();auto p=parameters(out,tools,1,prefix);AgentStream *s;OK(agent_stream_open(w,&p,&s));
    feed(s,line(1,"file_chunk","int main(void){return 1;}\n","edit.c",1));
    assert(!tools.calls);feed(s,line(2,"file_end","","edit.c",1));
    {std::unique_lock<std::mutex> lock(tools.mutex);assert(tools.cv.wait_for(lock,std::chrono::seconds(10),[&]{return tools.entered;}));}
    feed(s,line(3,"file_chunk","int main(void){return 0;}\n","edit.c",2));feed(s,line(4,"file_end","","edit.c",2));
    {std::lock_guard<std::mutex> lock(tools.mutex);tools.release=true;tools.cv.notify_all();}
    feed(s,line(5,"end"));Run r;OK(agent_stream_finish(s,&r.report));collect(s,r);
    assert(tools.calls==2 && r.report.stale_receipts==1 && r.receipts.size()==2 && r.receipts[0].stale && !r.receipts[1].stale);
    assert(r.receipt_text[0].find("exit=1")!=std::string::npos && r.receipt_text[1].find("exit=0")!=std::string::npos);
    assert(out.prompt.find("[local_result edit.c v1")==std::string::npos && out.prompt.find("[local_result edit.c v2")!=std::string::npos);
    agent_stream_close(s);no_sessions(w);
    // Cancellation while the processor is active does not publish its result.
    out={};tools.calls=0;tools.block_one=true;tools.entered=false;tools.release=false;p=parameters(out,tools,1,prefix);OK(agent_stream_open(w,&p,&s));
    feed(s,line(1,"file_chunk","int main(void){return 0;}\n","running.c"));feed(s,line(2,"file_end","","running.c"));
    {std::unique_lock<std::mutex> lock(tools.mutex);assert(tools.cv.wait_for(lock,std::chrono::seconds(10),[&]{return tools.entered;}));}
    OK(agent_stream_cancel(s));{std::lock_guard<std::mutex> lock(tools.mutex);tools.release=true;tools.cv.notify_all();}
    assert(agent_stream_finish(s,&r.report)==CXF_ERR_BAD_HANDLE && !out.called);agent_stream_close(s);assert(tools.calls==1);no_sessions(w);
    // Cancellation of a partial file never executes it or publishes a summary.
    out={};tools.block_one=false;tools.calls=0;p=parameters(out,tools,1,prefix);OK(agent_stream_open(w,&p,&s));
    feed(s,line(1,"file_chunk","int main(void){","cancel.c"));OK(agent_stream_cancel(s));
    assert(agent_stream_finish(s,&r.report)==CXF_ERR_BAD_HANDLE && !out.called && !tools.calls);agent_stream_close(s);no_sessions(w);
    // Force backlog pressure while owner is occupied; accepted frames are retried once.
    out={};p=parameters(out,tools,1,prefix);OK(agent_stream_open(w,&p,&s));
    std::mutex mutex;std::condition_variable cv;bool entered=false,release=false;
    std::thread blocker([&]{OK(owner(w,[&](cxf_llama *){std::unique_lock<std::mutex> lock(mutex);entered=true;cv.notify_all();cv.wait(lock,[&]{return release;});return CXF_OK;}));});
    {std::unique_lock<std::mutex> lock(mutex);cv.wait(lock,[&]{return entered;});}
    bool full=false;uint32_t sequence=1;
    while(!full && sequence<32){auto wire=line(sequence,"text","bounded backlog");uint32_t used=0;auto rc=agent_stream_feed(s,wire.data(),(uint32_t)wire.size(),&used);assert(used==wire.size());
        if(rc==CXF_ERR_FULL){full=true;}else{OK(rc);sequence++;}}
    assert(full);{std::lock_guard<std::mutex> lock(mutex);release=true;cv.notify_all();}blocker.join();
    feed(s,"",1);sequence++;feed(s,line(sequence,"end"));OK(agent_stream_finish(s,&r.report));assert(r.report.backpressure_returns && r.report.pending_peak<=16);
    agent_stream_close(s);no_sessions(w);
    std::puts("{\"kind\":\"correctness\",\"passed\":true,\"cases\":[\"arbitrary_byte_splits\",\"unicode_escape_nul\",\"full_token_oracle\",\"independent_full_prefill_greedy\",\"greedy_batch_equivalence\",\"real_C_test\",\"receipt_feedback_before_end\",\"partial_file_not_executed\",\"invalid_UTF8_JSON\",\"sequence_and_end_validation\",\"capacity_limits\",\"versioned_stale_receipts\",\"active_processor_cancel\",\"cancellation\",\"bounded_backpressure\",\"session_cleanup\"]}");
}
static std::vector<std::string> fixture(bool file){
    std::vector<std::string> out;uint32_t seq=1;
    if(file){out.push_back(line(seq++,"file_chunk","int main(void) {\n","demo.c"));out.push_back(line(seq++,"file_chunk","    return 0;\n","demo.c"));
        out.push_back(line(seq++,"file_chunk","}\n","demo.c"));out.push_back(line(seq++,"file_end","","demo.c"));}
    for(int i=0;i<(file?12:16);i++){std::string body="Observation "+std::to_string(i)+": ";for(int n=0;n<32;n++)body+="delta ";out.push_back(line(seq++,"text",body));}
    out.push_back(line(seq,"end"));return out;
}
static void benchmark(AgentKvWorker *w,const char *root,int repeats){
    const char *names[]={"wait_all","stream_coalesced","stream_eager"};
    for(bool file:{false,true})for(int gap:{0,20,100}){
        auto events=fixture(file);std::vector<int32_t> oracle;int32_t first=0;
        for(int rep=-1;rep<repeats;rep++)for(int variant=0;variant<3;variant++){
            int mode=(rep+variant+3)%3;auto r=execute(w,root,events,gap,mode,7);
            if(oracle.empty()){oracle=r.output.canonical;first=r.output.first;}else assert(oracle==r.output.canonical && first==r.output.first);
            if(rep<0)continue;
            uint64_t early_tools=0;for(auto &x:r.receipts)if(x.finished_ns && x.finished_ns<r.report.input_end_ns)early_tools++;
            std::printf("{\"kind\":\"timing\",\"scenario\":\"%s\",\"gap_ms\":%d,\"planned_input_ms\":%zu,\"rep\":%d,\"mode\":\"%s\",\"correct\":true,\"frames\":%llu,\"tokens\":%u,\"initial_tokens\":%u,\"tokenized_tokens\":%llu,\"tokenizer_calls\":%llu,\"evaluated_tokens\":%llu,\"decode_calls\":%llu,\"input_evaluated_tokens\":%llu,\"input_decode_calls\":%llu,\"tool_calls\":%llu,\"tools_finished_before_end\":%llu,\"tool_scratch_peak\":%llu,\"source_storage_bytes\":%llu,\"pending_peak\":%llu,\"backpressure\":%llu,\"source_start_ns\":%llu,\"input_end_ns\":%llu,\"model_ready_ns\":%llu,\"first_token_ns\":%llu,\"done_ns\":%llu,\"input_ms\":%.6f,\"post_input_ready_ms\":%.6f,\"total_ready_ms\":%.6f,\"total_first_token_ms\":%.6f,\"total_output_ms\":%.6f,\"render_ms\":%.6f,\"tokenize_ms\":%.6f,\"prefill_ms\":%.6f,\"tool_ms\":%.6f}\n",
                file?"complete_file_test":"text_prefill",gap,(events.size()-1)*(size_t)gap,rep,names[mode],(unsigned long long)r.report.frames,r.report.committed_tokens,r.report.initial_tokens,
                (unsigned long long)r.report.tokenized_tokens,(unsigned long long)r.report.tokenizer_calls,(unsigned long long)r.report.evaluated_tokens,(unsigned long long)r.report.decode_calls,
                (unsigned long long)r.report.input_evaluated_tokens,(unsigned long long)r.report.input_decode_calls,
                (unsigned long long)r.report.tool_calls,(unsigned long long)early_tools,(unsigned long long)r.report.tool_scratch_peak,(unsigned long long)r.report.source_storage_bytes,
                (unsigned long long)r.report.pending_peak,(unsigned long long)r.report.backpressure_returns,(unsigned long long)r.start,(unsigned long long)r.report.input_end_ns,
                (unsigned long long)r.report.model_ready_ns,(unsigned long long)r.output.first_ns,(unsigned long long)r.report.done_ns,
                ms(r.report.input_end_ns-r.start),ms(r.report.model_ready_ns-r.report.input_end_ns),ms(r.report.model_ready_ns-r.start),ms(r.output.first_ns-r.start),ms(r.report.done_ns-r.start),
                ms(r.report.render_ns),ms(r.report.tokenize_ns),ms(r.report.prefill_ns),ms(r.report.tool_ns));
            for(auto &x:r.receipts)std::printf("{\"kind\":\"unit\",\"scenario\":\"complete_file_test\",\"gap_ms\":%d,\"rep\":%d,\"mode\":\"%s\",\"version\":%u,\"end_seq\":%llu,\"started_ns\":%llu,\"finished_ns\":%llu,\"stale\":%s}\n",gap,rep,names[mode],x.version,(unsigned long long)x.end_seq,(unsigned long long)x.started_ns,(unsigned long long)x.finished_ns,x.stale?"true":"false");
            std::fflush(stdout);
        }
    }
}
int main(int argc,char **argv){
    if(argc!=6 || (std::strcmp(argv[4],"test") && std::strcmp(argv[4],"bench") && std::strcmp(argv[4],"replay"))){std::fprintf(stderr,"usage: agent-stream-replay MODEL RUNTIME SCRATCH test|bench|replay REPEATS|NDJSON|-\n");return 2;}
    auto params=cxf_llama_default_params();params.model_path=argv[1];params.backend_path=argv[2];params.context_tokens=8192;params.max_sessions=4;
    AgentKvWorker *w;auto open_rc=agent_kv_worker_open(&params,argv[3],1024ull*1024*1024,&w);if(open_rc!=CXF_OK){std::fprintf(stderr,"stream worker open rc=%d\n",open_rc);return 2;}
    if(std::strcmp(argv[4],"replay")==0){
        Output out;auto prefix=initial();Tools tools;tools.root=argv[3];auto p=parameters(out,tools,1,prefix);p.process_unit=observe_unit;p.unit_ctx=nullptr;AgentStream *s;OK(agent_stream_open(w,&p,&s));
        FILE *input=std::strcmp(argv[5],"-")==0?stdin:std::fopen(argv[5],"rb");char buffer[4096];cxf_result rc=input?CXF_OK:CXF_ERR_IO;
        while(rc==CXF_OK){auto n=::read(fileno(input),buffer,sizeof buffer);if(n<0){if(errno==EINTR)continue;rc=CXF_ERR_IO;break;}if(!n)break;rc=transfer(s,std::string(buffer,(size_t)n),4096);}if(input && input!=stdin)std::fclose(input);
        AgentStreamReport report={};if(rc==CXF_OK)rc=agent_stream_finish(s,&report);
        common_json result;result["result"]=(int)rc;result["preview"]=out.preview;result["ended"]=out.ended;result["tokens"]=report.committed_tokens;
        result["scope"]="16-token bounded local preview; file processing is read-only in replay CLI";std::cout<<result.dump()<<'\n';agent_stream_close(s);
        if(rc!=CXF_OK){agent_kv_worker_close(w);return 1;}
    }else{correctness(w,argv[3]);if(std::strcmp(argv[4],"bench")==0){int n=std::atoi(argv[5]);assert(n>0&&n<=30);benchmark(w,argv[3],n);}}
    no_sessions(w);agent_kv_worker_close(w);std::puts("{\"kind\":\"closed\",\"sessions\":0,\"passed\":true}");
}
