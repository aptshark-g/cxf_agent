#include "agent_stream.h"
#include "agent_native_messages.h"
#include "../src/llama_internal.h"
extern "C" {
#include "sha256.h"
}
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <memory>
#include <new>
#include <thread>
#include <vector>

namespace {
constexpr uint32_t FRAME_CAP=16384, SOURCE_CAP=65536, EVENT_CAP=128, FILE_CAP=8, BACKLOG=16, RECEIPT_CAP=4096;
const std::string HEADER="<|im_start|>assistant\n", GENERATION=HEADER+"<think>\n\n</think>\n\n";
uint64_t tick(){return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
struct Failure{cxf_result rc;};
void check(cxf_result rc){if(rc!=CXF_OK)throw Failure{rc};}
using Owner=std::function<cxf_result(cxf_llama *)>;
cxf_result run(AgentKvWorker *w,Owner fn){return agent_kv_worker_run(w,[](cxf_llama *e,void *p){
    try{return (*(Owner *)p)(e);}catch(const Failure &f){return f.rc;}catch(const std::bad_alloc &){return CXF_ERR_NOMEM;}catch(...){return CXF_ERR_IO;}
},&fn);}
void hash(const char *s,size_t n,unsigned char out[32]){sha256_hash(out,(const unsigned char *)s,n);}
uint32_t number(const common_json &j,const char *key){
    const auto &v=j.at(key);if(!v.is_number_integer())throw Failure{CXF_ERR_FORMAT};
    auto n=v.get<int64_t>();if(n<=0 || n>UINT32_MAX)throw Failure{CXF_ERR_FORMAT};return (uint32_t)n;
}
std::string string_field(const common_json &j,const char *key){const auto &v=j.at(key);if(!v.is_string())throw Failure{CXF_ERR_FORMAT};return v.get<std::string>();}
bool valid_id(const std::string &s){
    if(s.empty() || s.size()>64 || s=="." || s=="..")return false;
    for(unsigned char c:s)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.'))return false;
    return true;
}
jinja::value_string value(const std::string &s){auto v=jinja::mk_val<jinja::value_string>();v->val_str.parts.push_back({true,s});return v;}
}
struct AgentStream {
    struct Event{std::string content;uint32_t body_offset=0;};
    struct File {
        std::string id;uint32_t version=0;uint64_t end_seq=0,started=0,finished=0;
        bool closed=false,running=false,done=false;cxf_result rc=CXF_ERR_EMPTY;
        std::vector<const Event *> chunks;std::string receipt;
    };
    AgentKvWorker *worker;AgentStreamParams params;
    std::string system,frame;bool complete_frame=false,ended=false,finishing=false,cancelled=false,done=false;
    cxf_result error=CXF_OK;uint32_t next_seq=1;size_t cursor=0;
    std::deque<Event> events;std::deque<File> files;
    cxf_llama_session session={};bool has_session=false;
    std::unique_ptr<common_chat_template> tmpl;
    size_t prefix_bytes=0;unsigned char prefix_hash[32]={};
    uint64_t initial_eval_counter=0,initial_decode_counter=0;
    AgentStreamReport report={};std::mutex mutex;std::condition_variable cv;
    std::thread scheduler,processor;
    void fail(cxf_result rc){std::lock_guard<std::mutex> lock(mutex);if(error==CXF_OK)error=rc;cv.notify_all();}
    File *latest(const std::string &id){File *out=nullptr;for(auto &f:files)if(f.id==id && (!out || f.version>out->version))out=&f;return out;}
    bool tools_done(){for(auto &f:files)if(f.closed && !f.done)return false;return true;}
    bool stopped(){return cancelled || error!=CXF_OK;}
    cxf_result accept_frame(){ // host mutex held; a FULL backlog retains this frame
        auto j=common_json::parse(frame);if(!j.is_object())return CXF_ERR_FORMAT;
        uint32_t seq=number(j,"seq");if(seq!=next_seq || ended)return CXF_ERR_FORMAT;
        auto kind=string_field(j,"kind");
        if(kind=="end"){
            for(auto &f:files)if(latest(f.id)==&f && !f.closed)return CXF_ERR_FORMAT;
            ended=true;report.input_end_ns=tick();report.frames++;next_seq++;cv.notify_all();return CXF_OK;
        }
        if(kind!="text" && kind!="file_chunk" && kind!="file_end")return CXF_ERR_FORMAT;
        if(!params.defer_until_end && events.size()-cursor>=BACKLOG){report.backpressure_returns++;return CXF_ERR_FULL;}
        std::string body=kind=="file_end"?"":string_field(j,"text");
        if(events.size()>=EVENT_CAP || body.size()>SOURCE_CAP-report.source_bytes)throw Failure{CXF_ERR_FULL};
        File *file=nullptr;std::string label;
        if(kind!="text") {
            auto id=string_field(j,"id");auto version=number(j,"version");if(!valid_id(id))return CXF_ERR_FORMAT;
            auto *old=latest(id);
            if(kind=="file_end") {
                if(!old || old->version!=version || old->closed)return CXF_ERR_FORMAT;file=old;
            } else {
                if(old && (old->version>version || (old->version==version && old->closed)))return CXF_ERR_FORMAT;
                if(!old || old->version<version) {
                    if(files.size()>=FILE_CAP)throw Failure{CXF_ERR_FULL};
                    files.emplace_back();file=&files.back();file->id=id;file->version=version;
                } else file=old;
            }
            label="["+kind+" "+id+" v"+std::to_string(version)+"]\n";
        }
        events.push_back({label+body,(uint32_t)label.size()});
        if(file) {
            if(kind=="file_end"){file->closed=true;file->end_seq=seq;}
            else file->chunks.push_back(&events.back());
        }
        report.source_bytes+=body.size();report.source_storage_bytes+=events.back().content.size();
        report.frames++;next_seq++;report.pending_peak=std::max<uint64_t>(report.pending_peak,events.size()-cursor);cv.notify_all();return CXF_OK;
    }
    std::string render(size_t count,bool receipts){
        auto messages=jinja::mk_val<jinja::value_array>();
        auto add=[&](const char *role,const std::string &text){auto m=jinja::mk_val<jinja::value_object>();m->insert("role",value(role));m->insert("content",value(text));messages->push_back(std::move(m));};
        add("system",system);
        // Event objects are immutable after acceptance; deque references survive append.
        std::vector<const Event *> input;std::vector<std::string> results;
        {std::lock_guard<std::mutex> lock(mutex);
            for(size_t i=0;i<count;i++)input.push_back(&events[i]);
            if(receipts) {
                std::vector<File *> sorted;for(auto &f:files)if(f.done && latest(f.id)==&f)sorted.push_back(&f);
                std::sort(sorted.begin(),sorted.end(),[](const File *a,const File *b){return a->end_seq<b->end_seq;});
                for(auto *f:sorted)results.push_back("[local_result "+f->id+" v"+std::to_string(f->version)+" rc="+std::to_string(f->rc)+"]\n"+f->receipt);
            }
        }
        for(auto *e:input)add("user",e->content);for(auto &s:results)add("user",s);
        return agent_native_render_values(*tmpl,std::move(messages),common_json::array());
    }
    void apply(const std::string &prompt,bool final,bool initial=false){
        auto boundary=prompt.rfind(HEADER);
        if(boundary==std::string::npos || prompt.substr(boundary)!=GENERATION)throw Failure{CXF_ERR_DEP};
        size_t end=final?prompt.size():boundary;
        unsigned char digest[32];
        if(prefix_bytes>end)throw Failure{CXF_ERR_FORMAT};
        hash(prompt.data(),prefix_bytes,digest);if(prefix_bytes && std::memcmp(digest,prefix_hash,32))throw Failure{CXF_ERR_FORMAT};
        uint64_t tokenize_ns=0,prefill_ns=0,calls=0,tokenized=0;uint32_t evaluated=0,committed=0;
        check(run(worker,[&](cxf_llama *engine){
            {std::lock_guard<std::mutex> lock(mutex);if(stopped())return CXF_ERR_BAD_HANDLE;}
            cxf_llama_view v;check(cxf_llama_session_view(engine,session,&v));
            uint32_t bytes=(uint32_t)(end-prefix_bytes),n=0;
            if(bytes){
                auto start=tick();std::vector<int32_t> t(std::min<uint32_t>(8192,std::max(32u,bytes/4+32u)));
                calls++;auto rc=cxf_llama_tokenize(engine,prompt.data()+prefix_bytes,bytes,false,t.data(),(uint32_t)t.size(),&n);
                if(rc==CXF_ERR_FULL){if(n>65536)return CXF_ERR_FULL;std::vector<int32_t>().swap(t);t.resize(n);calls++;rc=cxf_llama_tokenize(engine,prompt.data()+prefix_bytes,bytes,false,t.data(),n,&n);}
                check(rc);tokenized=n;tokenize_ns=tick()-start;check(cxf_llama_append(engine,session,v.revision,t.data(),n));
                check(cxf_llama_session_view(engine,session,&v));
            }
            auto start=tick();uint32_t processed=0;
            check(cxf_llama_prefill(engine,session,v.revision,initial?UINT32_MAX:params.prefill_budget,initial||final||params.flush_each_update,&processed));
            prefill_ns=tick()-start;check(cxf_llama_session_view(engine,session,&v));evaluated=v.evaluated;committed=v.committed;return CXF_OK;
        }));
        prefix_bytes=end;hash(prompt.data(),end,prefix_hash);
        std::lock_guard<std::mutex> lock(mutex);
        if(initial)report.initial_tokens=committed;
        else {report.tokenize_ns+=tokenize_ns;report.prefill_ns+=prefill_ns;report.tokenizer_calls+=calls;report.tokenized_tokens+=tokenized;}
        report.committed_tokens=committed;report.evaluated_frontier=evaluated;
    }
    void pump(bool flush){
        uint32_t committed=0,evaluated=0;uint64_t elapsed=0;
        check(run(worker,[&](cxf_llama *e){
            {std::lock_guard<std::mutex> lock(mutex);if(stopped())return CXF_ERR_BAD_HANDLE;}
            cxf_llama_view v;check(cxf_llama_session_view(e,session,&v));uint32_t done=0;
            auto start=tick();check(cxf_llama_prefill(e,session,v.revision,params.prefill_budget,flush||params.flush_each_update,&done));elapsed=tick()-start;
            check(cxf_llama_session_view(e,session,&v));committed=v.committed;evaluated=v.evaluated;return CXF_OK;
        }));
        std::lock_guard<std::mutex> lock(mutex);report.prefill_ns+=elapsed;report.committed_tokens=committed;report.evaluated_frontier=evaluated;
    }
    void process_loop(){
        try {for(;;){
            File *file=nullptr;
            {std::unique_lock<std::mutex> lock(mutex);cv.wait(lock,[&]{
                if(stopped() || done)return true;if(params.defer_until_end && !ended)return false;
                for(auto &f:files)if(f.closed && !f.running && !f.done)return true;return false;
            });if(stopped() || done)return;
                for(auto &f:files)if(f.closed && !f.running && !f.done){
                    if(latest(f.id)!=&f){f.done=true;f.rc=CXF_ERR_BAD_HANDLE;continue;}file=&f;break;
                }
                if(!file){cv.notify_all();continue;}file->running=true;file->started=tick();report.tool_calls++;
            }
            std::string body;for(auto *e:file->chunks)body.append(e->content.data()+e->body_offset,e->content.size()-e->body_offset);
            {std::lock_guard<std::mutex> lock(mutex);report.tool_scratch_peak=std::max<uint64_t>(report.tool_scratch_peak,body.capacity());}
            AgentStreamUnit unit={file->id.c_str(),body.data(),file->version,(uint32_t)body.size(),file->end_seq};
            char output[RECEIPT_CAP];uint32_t written=0;cxf_result rc=params.process_unit?CXF_OK:CXF_ERR_DEP;
            if(params.process_unit)try{rc=params.process_unit(&unit,params.unit_ctx,output,sizeof output,&written);}catch(...){rc=CXF_ERR_IO;}
            if(written>sizeof output)throw Failure{CXF_ERR_RANGE};
            std::string receipt(output,written);(void)common_json(receipt).dump(); // validate callback UTF-8
            {std::lock_guard<std::mutex> lock(mutex);file->receipt=std::move(receipt);file->rc=rc;file->finished=tick();file->done=true;
                report.tool_completed++;report.tool_ns+=file->finished-file->started;cv.notify_all();}
        }}catch(const Failure &f){fail(f.rc);}catch(const std::bad_alloc &){fail(CXF_ERR_NOMEM);}catch(...){fail(CXF_ERR_FORMAT);}
    }
    void schedule_loop(){
        try {
            size_t processed=cursor;bool final_applied=false;std::string final_prompt;
            for(;;){
                size_t count;bool final;
                {std::unique_lock<std::mutex> lock(mutex);cv.wait(lock,[&]{
                    return stopped() || ((!params.defer_until_end || (finishing&&tools_done())) && events.size()>processed) ||
                           (finishing && tools_done()) || report.committed_tokens>report.evaluated_frontier;
                });if(stopped())break;count=events.size();final=finishing && tools_done();}
                if(count>processed || (final && !final_applied)){
                    auto start=tick();auto prompt=render(count,final);
                    {std::lock_guard<std::mutex> lock(mutex);report.render_ns+=tick()-start;}
                    apply(prompt,final);
                    {std::lock_guard<std::mutex> lock(mutex);cursor=count;processed=count;cv.notify_all();}
                    if(final){final_applied=true;final_prompt=std::move(prompt);}
                } else {
                    uint32_t before;
                    {std::lock_guard<std::mutex> lock(mutex);before=report.evaluated_frontier;}
                    pump(final);
                    {std::unique_lock<std::mutex> lock(mutex);
                        // Below minimum batch: wait for more complete units or end.
                        if(report.evaluated_frontier==before && !final)cv.wait(lock,[&]{return stopped() || events.size()>processed || (finishing&&tools_done());});
                    }
                }
                bool ready;
                {std::lock_guard<std::mutex> lock(mutex);ready=final_applied && report.committed_tokens==report.evaluated_frontier;}
                if(ready){
                    {std::lock_guard<std::mutex> lock(mutex);if(stopped())break;report.model_ready_ns=tick();}
                    check(run(worker,[&](cxf_llama *e){
                        cxf_llama_stats stats;cxf_llama_get_stats(e,&stats);
                        {std::lock_guard<std::mutex> lock(mutex);if(stopped())return CXF_ERR_BAD_HANDLE;report.final_started_ns=tick();
                            report.input_evaluated_tokens=stats.evaluated_tokens-initial_eval_counter;report.input_decode_calls=stats.decode_calls-initial_decode_counter;}
                        return params.finalize?params.finalize(e,session,final_prompt.data(),(uint32_t)final_prompt.size(),params.final_ctx):CXF_OK;
                    }));break;
                }
            }
        }catch(const Failure &f){fail(f.rc);}catch(const std::bad_alloc &){fail(CXF_ERR_NOMEM);}catch(...){fail(CXF_ERR_FORMAT);}
        // Collect real decode counters; this session is the worker's only active user.
        uint64_t eval=0,decode=0;
        (void)run(worker,[&](cxf_llama *e){cxf_llama_stats s;cxf_llama_get_stats(e,&s);eval=s.evaluated_tokens;decode=s.decode_calls;return CXF_OK;});
        {std::lock_guard<std::mutex> lock(mutex);report.evaluated_tokens=eval-initial_eval_counter;report.decode_calls=decode-initial_decode_counter;
            done=true;report.done_ns=tick();cv.notify_all();}
    }
    ~AgentStream(){
        {std::lock_guard<std::mutex> lock(mutex);cancelled=true;cv.notify_all();}
        if(scheduler.joinable())scheduler.join();if(processor.joinable())processor.join();
        if(has_session)(void)run(worker,[&](cxf_llama *e){return cxf_llama_session_close(e,session);});
    }
};

extern "C" cxf_result agent_stream_open(AgentKvWorker *w,const AgentStreamParams *params,AgentStream **out){
    if(!out)return CXF_ERR_RANGE;*out=nullptr;
    if(!w || !params || !params->prefill_budget || params->prefill_budget>512 ||
       params->initial_bytes>SOURCE_CAP || (params->initial_bytes&&!params->initial_context))return CXF_ERR_RANGE;
    AgentStream *p=nullptr;
    try {
        p=new AgentStream;p->worker=w;p->params=*params;p->system=params->system?params->system:"Summarize the supplied text and local results briefly.";
        if(p->system.size()>512)throw Failure{CXF_ERR_FULL};(void)common_json(p->system).dump();
        if(params->initial_bytes){std::string initial(params->initial_context,params->initial_bytes);(void)common_json(initial).dump();
            p->events.push_back({std::move(initial),0});p->report.source_bytes=params->initial_bytes;p->report.source_storage_bytes=params->initial_bytes;}
        std::string source;
        check(run(w,[&](cxf_llama *e){const auto *m=cxf_llama_native_model(e);const char *s=llama_model_chat_template(m,nullptr),*t=llama_model_chat_template(m,"tool_use");
            if(!s || !agent_native_verified_template(s) || (t&&std::strcmp(s,t)))return CXF_ERR_DEP;
            char arch[64]={};llama_model_meta_val_str(m,"general.architecture",arch,sizeof arch);
            int32_t marker[4];uint32_t n=0;auto rc=cxf_llama_tokenize(e,"<|im_start|>",12,false,marker,4,&n);
            if(std::strcmp(arch,"qwen3") || rc!=CXF_OK || n!=1 || !(llama_vocab_get_attr(llama_model_get_vocab(m),marker[0])&(LLAMA_TOKEN_ATTR_CONTROL|LLAMA_TOKEN_ATTR_USER_DEFINED)))return CXF_ERR_DEP;
            source=s;rc=cxf_llama_session_new(e,&p->session);p->has_session=rc==CXF_OK;return rc;}));
        p->tmpl.reset(new common_chat_template(source,"",""));
        auto prompt=p->render(p->events.size(),false);p->apply(prompt,false,true);p->cursor=p->events.size();
        check(run(w,[&](cxf_llama *e){cxf_llama_stats s;cxf_llama_get_stats(e,&s);p->initial_eval_counter=s.evaluated_tokens;p->initial_decode_counter=s.decode_calls;return CXF_OK;}));
        p->processor=std::thread([p]{p->process_loop();});p->scheduler=std::thread([p]{p->schedule_loop();});*out=p;return CXF_OK;
    }catch(const Failure &f){delete p;return f.rc;}catch(const std::bad_alloc &){delete p;return CXF_ERR_NOMEM;}catch(...){delete p;return CXF_ERR_FORMAT;}
}
extern "C" cxf_result agent_stream_feed(AgentStream *p,const char *data,uint32_t bytes,uint32_t *consumed){
    if(consumed)*consumed=0;if(!p || !consumed || (bytes&&!data))return CXF_ERR_RANGE;
    std::lock_guard<std::mutex> lock(p->mutex);
    if(p->stopped())return p->cancelled?CXF_ERR_BAD_HANDLE:p->error;
    if(p->finishing || p->done)return CXF_ERR_BAD_HANDLE;
    if(!p->report.started_ns)p->report.started_ns=tick();
    try {for(;;){
        if(p->complete_frame){
            auto rc=p->accept_frame();if(rc==CXF_ERR_FULL)return rc;
            if(rc!=CXF_OK)throw Failure{rc};p->frame.clear();p->complete_frame=false;
        }
        if(*consumed==bytes)return CXF_OK;
        char c=data[(*consumed)++];
        if(c=='\n'){if(p->frame.empty())continue;p->complete_frame=true;}
        else {if(p->frame.size()>=FRAME_CAP)throw Failure{CXF_ERR_FULL};p->frame.push_back(c);}
    }}catch(const Failure &f){p->error=f.rc;}catch(const std::bad_alloc &){p->error=CXF_ERR_NOMEM;}catch(...){p->error=CXF_ERR_FORMAT;}
    p->cv.notify_all();return p->error;
}
extern "C" cxf_result agent_stream_wait_input(AgentStream *p,uint32_t timeout){
    if(!p || timeout>10000)return CXF_ERR_RANGE;
    std::unique_lock<std::mutex> lock(p->mutex);
    if(!p->cv.wait_for(lock,std::chrono::milliseconds(timeout),[&]{return p->stopped() || p->params.defer_until_end || p->events.size()-p->cursor<BACKLOG;}))return CXF_ERR_EMPTY;
    return p->cancelled?CXF_ERR_BAD_HANDLE:p->error;
}
extern "C" void agent_stream_report(AgentStream *p,AgentStreamReport *out){
    if(p && out){std::lock_guard<std::mutex> lock(p->mutex);*out=p->report;out->cancelled=p->cancelled;out->failed=p->error!=CXF_OK;out->done=p->done;
        out->result=p->cancelled?CXF_ERR_BAD_HANDLE:p->error;
        out->stale_receipts=0;for(auto &f:p->files)if(f.done && (p->stopped() || p->latest(f.id)!=&f))out->stale_receipts++;
    }
}
extern "C" cxf_result agent_stream_finish(AgentStream *p,AgentStreamReport *out){
    if(!p)return CXF_ERR_RANGE;
    {std::unique_lock<std::mutex> lock(p->mutex);
        if(!p->cancelled && (!p->ended || !p->frame.empty() || p->complete_frame)){if(p->error==CXF_OK)p->error=CXF_ERR_FORMAT;}
        p->finishing=true;p->cv.notify_all();p->cv.wait(lock,[&]{return p->done;});}
    AgentStreamReport r;agent_stream_report(p,&r);if(out)*out=r;return r.result;
}
extern "C" cxf_result agent_stream_cancel(AgentStream *p){
    if(!p)return CXF_ERR_RANGE;std::lock_guard<std::mutex> lock(p->mutex);p->cancelled=true;p->cv.notify_all();return CXF_OK;
}
extern "C" cxf_result agent_stream_receipt(AgentStream *p,uint32_t index,AgentStreamReceipt *out,char *body,uint32_t cap,uint32_t *written){
    if(!p || !out || !written || (cap&&!body))return CXF_ERR_RANGE;
    std::lock_guard<std::mutex> lock(p->mutex);if(index>=p->files.size())return CXF_ERR_RANGE;auto &f=p->files[index];
    *out={};std::memcpy(out->id,f.id.data(),f.id.size());out->version=f.version;out->end_seq=f.end_seq;out->started_ns=f.started;out->finished_ns=f.finished;
    out->stale=p->stopped() || p->latest(f.id)!=&f;out->result=f.rc;*written=(uint32_t)f.receipt.size();
    if(!f.done)return CXF_ERR_EMPTY;if(cap<*written)return CXF_ERR_FULL;if(*written)std::memcpy(body,f.receipt.data(),*written);return CXF_OK;
}
extern "C" void agent_stream_close(AgentStream *p){delete p;}
