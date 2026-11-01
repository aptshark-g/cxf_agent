#include "agent_tool_prepare.h"
#include "../src/llama_internal.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <new>
#include <string_view>
#include <thread>

static uint64_t clock_ns(){return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
static double millis(uint64_t n){return n/1e6;}
static constexpr double ALPHA=.09512492197250393;
static bool options_valid(AgentToolPrepareOptions o){return o.kv_mode>=TOOL_KV_RESIDENT&&o.kv_mode<=TOOL_KV_RESTORE&&o.policy>=TOOL_PREPARE_LATE&&o.policy<=TOOL_PREPARE_PROGRESS&&std::isfinite(o.margin_ms)&&o.margin_ms>=0&&o.margin_ms<=600000&&(!o.hold_snapshot_file||(o.release_snapshot_early&&o.kv_mode==TOOL_KV_RESTORE));}
struct AgentToolPrepare {
    struct Estimate {uint64_t key=0;double tool=0,service=0,post_spawn=0;uint32_t nt=0,np=0,nprogress=0;};
    int output_policy=0;AgentKvWorker *worker=nullptr;AgentNative *native=nullptr;
    AgentToolPrepareOptions options={};std::array<Estimate,32> estimates;
    std::atomic<bool> active{false},cancelled{false};std::atomic<uint64_t> ticket{0};
    std::mutex mutex;std::condition_variable cv;bool tool_finished=false,accepting_cancel=false;
    uint64_t next_epoch=1,epoch=0,progress_ns=0,tool_start_ns=0;
    Estimate *estimate(uint64_t key){for(auto &x:estimates)if(x.key==key)return &x;for(auto &x:estimates)if(!x.key){x.key=key;return &x;}return nullptr;}
};
using OwnerFn=std::function<cxf_result(cxf_llama *)>;
static cxf_result owner(AgentToolPrepare *p,OwnerFn fn){return agent_kv_worker_run(p->worker,[](cxf_llama *e,void *ctx){return (*(OwnerFn *)ctx)(e);},&fn);}
extern "C" cxf_result agent_tool_prepare_open(const cxf_llama_params *params,uint32_t maximum,const char *parent,uint64_t budget,AgentToolPrepareOptions options,AgentToolPrepare **out){
    if(!out)return CXF_ERR_RANGE;*out=nullptr;
    if(!params||!maximum||!options_valid(options))return CXF_ERR_RANGE;
    AgentToolPrepare *p=nullptr;
    try {
        p=new AgentToolPrepare;p->options=options;
        auto rc=agent_kv_worker_open(params,parent,budget,&p->worker);
        if(rc==CXF_OK)rc=owner(p,[&](cxf_llama *e){return agent_native_bind(e,params->context_tokens,maximum,&p->native);});
        if(rc!=CXF_OK){agent_tool_prepare_close(p);return rc;}
        *out=p;return CXF_OK;
    }catch(const std::bad_alloc &){agent_tool_prepare_close(p);return CXF_ERR_NOMEM;}catch(...){agent_tool_prepare_close(p);return CXF_ERR_IO;}
}
extern "C" void agent_tool_prepare_close(AgentToolPrepare *p){
    if(!p)return;
    if(p->native)(void)owner(p,[&](cxf_llama *){agent_native_close(p->native);p->native=nullptr;return CXF_OK;});
    agent_kv_worker_close(p->worker);delete p;
}
extern "C" cxf_result agent_tool_prepare_options(AgentToolPrepare *p,AgentToolPrepareOptions o){
    if(!p||!options_valid(o))return CXF_ERR_RANGE;if(p->active)return CXF_ERR_FULL;p->options=o;return CXF_OK;
}
extern "C" cxf_result agent_tool_prepare_seed(AgentToolPrepare *p,uint64_t key,double tool,double service){
    if(!p||!key||!std::isfinite(tool)||tool<0||tool>600000||!std::isfinite(service)||service<0||service>600000)return CXF_ERR_RANGE;
    if(p->active)return CXF_ERR_FULL;auto *v=p->estimate(key);if(!v)return CXF_ERR_FULL;
    v->tool=tool;v->service=service;v->nt=v->np=3;v->post_spawn=0;v->nprogress=0;return CXF_OK;
}
extern "C" cxf_result agent_tool_prepare_seed_progress(AgentToolPrepare *p,uint64_t key,double remaining){
    if(!p||!key||!std::isfinite(remaining)||remaining<0||remaining>600000)return CXF_ERR_RANGE;
    if(p->active)return CXF_ERR_FULL;auto *v=p->estimate(key);if(!v)return CXF_ERR_FULL;
    v->post_spawn=remaining;v->nprogress=3;return CXF_OK;
}
extern "C" cxf_result agent_tool_prepare_progress(AgentToolPrepare *p,uint64_t epoch,uint64_t event){
    if(!p||!epoch)return CXF_ERR_RANGE;
    {std::lock_guard<std::mutex> lock(p->mutex);
        if(!p->active||!p->accepting_cancel||p->cancelled||epoch!=p->epoch)return CXF_ERR_BAD_HANDLE;
        if(p->progress_ns||event<p->tool_start_ns||event>clock_ns())return CXF_ERR_RANGE;
        p->progress_ns=event;
    }
    p->cv.notify_all();return CXF_OK;
}
extern "C" cxf_result agent_tool_prepare_run(AgentToolPrepare *p,AgentToolNativeFn fn,void *ctx){
    if(!p||!fn)return CXF_ERR_RANGE;if(p->active)return CXF_ERR_FULL;
    return owner(p,[&](cxf_llama *){return fn(p->native,ctx);});
}
static void update(double &value,uint32_t &count,double sample){value=count?value+ALPHA*(sample-value):sample;if(count!=UINT32_MAX)count++;}
extern "C" cxf_result agent_tool_prepare_execute(AgentToolPrepare *p,uint64_t key,AgentToolExec fn,const void *ctx,const char *args,uint32_t n,char *out,uint32_t cap,uint32_t *written,AgentToolPrepareReport *report){
    if(!p||!key||!fn||!out||!written||!report||(n&&!args))return CXF_ERR_RANGE;
    {std::lock_guard<std::mutex> lock(p->mutex);if(p->active.exchange(true))return CXF_ERR_FULL;p->tool_finished=false;p->cancelled=false;p->accepting_cancel=true;p->progress_ns=0;p->tool_start_ns=0;p->epoch=p->next_epoch==UINT64_MAX?0:p->next_epoch++;}
    struct Active {AgentToolPrepare *p;~Active(){p->active=false;}} active{p};
    *report={};auto &r=*report;r.entered_ns=clock_ns();r.prepare_rc=CXF_ERR_EMPTY;r.early_drop_rc=r.drop_rc=CXF_ERR_EMPTY;
    r.epoch=p->epoch;
    auto *estimate=p->estimate(key);
    cxf_llama_session session={};uint64_t revision=0;AgentKvSnapshot snapshot=0;
    auto options=p->options;
    uint64_t start=clock_ns();
    try {
    r.prepare_rc=owner(p,[&](cxf_llama *){return agent_native_checkpoint(p->native,&session,&revision,&r.prefix_tokens);});
    r.checkpoint_ns=clock_ns()-start;
    if(r.prepare_rc==CXF_OK&&options.kv_mode==TOOL_KV_RESTORE){start=clock_ns();r.prepare_rc=agent_kv_worker_save(p->worker,session,revision,&snapshot);r.save_ns=clock_ns()-start;}
    if(r.prepare_rc==CXF_OK&&options.kv_mode!=TOOL_KV_RESIDENT){start=clock_ns();r.prepare_rc=agent_kv_worker_evict(p->worker,session,revision);r.evict_ns=clock_ns()-start;}
    if(estimate){r.predicted_tool_ms=estimate->tool;r.predicted_prepare_ms=estimate->service*r.prefix_tokens;r.tool_observations_before=estimate->nt;r.prepare_observations_before=estimate->np;}
    if(estimate){r.predicted_post_spawn_ms=estimate->post_spawn;r.progress_observations_before=estimate->nprogress;}
    if(options.policy==TOOL_PREPARE_TIMED&&r.tool_observations_before&&r.prepare_observations_before)
        r.planned_delay_ms=std::fmax(0,r.predicted_tool_ms-r.predicted_prepare_ms-options.margin_ms);
    r.initial_tool_estimate_ms=r.predicted_tool_ms;r.initial_delay_ms=r.planned_delay_ms;
    }catch(...){r.prepare_rc=CXF_ERR_NOMEM;}
    auto release_snapshot=[&](bool early){
        if(!snapshot)return;
        auto &begin=early?r.early_drop_start_ns:r.drop_start_ns;
        auto &end=early?r.early_drop_end_ns:r.drop_end_ns;
        auto &rc=early?r.early_drop_rc:r.drop_rc;
        if(options.trace_resources)(early?r.early_drop_before:r.drop_before)=agent_resource_sample();
        begin=clock_ns();rc=early&&options.hold_snapshot_file?agent_kv_worker_unlink_hold(p->worker,snapshot):agent_kv_worker_drop(p->worker,snapshot);end=clock_ns();
        if(options.trace_resources)(early?r.early_drop_after:r.drop_after)=agent_resource_sample();
        if(rc==CXF_OK&&!(early&&options.hold_snapshot_file))snapshot=0;
    };
    auto prepare=[&]{
        struct Release {decltype(release_snapshot) &fn;bool enabled;~Release(){if(enabled)fn(true);}} cleanup{release_snapshot,options.release_snapshot_early};
        r.thread_enter_ns=clock_ns();
        try {
            if(options.policy==TOOL_PREPARE_PROGRESS){
                std::unique_lock<std::mutex> lock(p->mutex);
                p->cv.wait(lock,[&]{return p->tool_finished||p->cancelled.load()||p->progress_ns;});
                if(p->progress_ns){
                    r.progress_ns=p->progress_ns;
                    double remaining=r.progress_observations_before?r.predicted_post_spawn_ms:0;
                    double lead=r.prepare_observations_before?r.predicted_prepare_ms:0;
                    auto delay=std::fmax(0,remaining-lead-options.margin_ms);
                    if(!r.progress_observations_before)delay=0;
                    r.timer_deadline_ns=r.progress_ns+(uint64_t)(std::fmin(delay,600000.0)*1e6);
                    r.predicted_tool_ms=millis(r.progress_ns-p->tool_start_ns)+remaining;
                    r.planned_delay_ms=millis(r.timer_deadline_ns-p->tool_start_ns);
                    auto due=std::chrono::steady_clock::time_point(std::chrono::nanoseconds(r.timer_deadline_ns));
                    p->cv.wait_until(lock,due,[&]{return p->tool_finished||p->cancelled.load();});
                }
            }
            if(options.policy==TOOL_PREPARE_TIMED){
                std::unique_lock<std::mutex> lock(p->mutex);
                auto due=std::chrono::steady_clock::now()+std::chrono::nanoseconds((uint64_t)(std::fmin(r.planned_delay_ms,600000.0)*1e6));
                r.timer_deadline_ns=(uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(due.time_since_epoch()).count();
                p->cv.wait_until(lock,due,[&]{return p->tool_finished||p->cancelled.load();});
            }
            r.timer_wake_ns=clock_ns();
            if(p->cancelled){r.prepare_rc=CXF_ERR_BAD_HANDLE;return;}
            r.prepare_start_ns=clock_ns();
            if(options.kv_mode==TOOL_KV_RESTORE){
                AgentKvTicket ticket=0;r.prepare_rc=agent_kv_worker_prepare(p->worker,snapshot,&ticket);p->ticket=ticket;
                r.prepare_api_return_ns=clock_ns();
                if(ticket){if(p->cancelled)(void)agent_kv_worker_cancel(p->worker,ticket);r.prepare_rc=agent_kv_worker_finish(p->worker,ticket,nullptr,nullptr,&r.kv);p->ticket=0;}
            }else r.prepare_rc=owner(p,[&](cxf_llama *e){uint32_t done=0;return cxf_llama_prefill(e,session,revision,UINT32_MAX,true,&done);});
            r.prefix_ready_ns=clock_ns();
            if(p->cancelled){
                if(r.prepare_rc==CXF_OK)r.wasted_tokens=r.prefix_tokens;
                (void)agent_kv_worker_evict(p->worker,session,revision);r.prepare_rc=CXF_ERR_BAD_HANDLE;
            }
        }catch(...){r.prepare_rc=CXF_ERR_IO;p->ticket=0;}
    };
    std::thread preparation;
    bool needs_prepare=r.prepare_rc==CXF_OK&&options.kv_mode!=TOOL_KV_RESIDENT;
    if(options.policy==TOOL_PREPARE_PROGRESS){std::lock_guard<std::mutex> lock(p->mutex);r.tool_start_ns=clock_ns();p->tool_start_ns=r.tool_start_ns;}
    if(needs_prepare&&options.policy!=TOOL_PREPARE_LATE){r.thread_launch_start_ns=clock_ns();try{preparation=std::thread(prepare);}catch(...){r.prepare_rc=CXF_ERR_IO;needs_prepare=false;}r.thread_launch_end_ns=clock_ns();}
    if(!r.tool_start_ns){std::lock_guard<std::mutex> lock(p->mutex);r.tool_start_ns=clock_ns();p->tool_start_ns=r.tool_start_ns;}
    r.tool_fn_start_ns=clock_ns();
    try{r.tool_rc=fn(ctx,args,n,out,cap,written);}catch(...){r.tool_rc=CXF_ERR_IO;*written=0;}
    r.tool_end_ns=clock_ns();
    {std::lock_guard<std::mutex> lock(p->mutex);p->tool_finished=true;}p->cv.notify_all();
    if(needs_prepare&&options.policy==TOOL_PREPARE_LATE)prepare();
    if(preparation.joinable())preparation.join();
    r.join_return_ns=clock_ns();
    /* The cancellation cutoff and state check share a lock. A cancel accepted
     * after native completion still rolls the speculative prefix back here. */
    {std::lock_guard<std::mutex> lock(p->mutex);p->accepting_cancel=false;r.cancelled=p->cancelled;if(!r.progress_ns)r.progress_ns=p->progress_ns;}
    if(r.cancelled&&needs_prepare&&r.prepare_rc==CXF_OK){
        r.wasted_tokens=r.prefix_tokens;(void)agent_kv_worker_evict(p->worker,session,revision);r.prepare_rc=CXF_ERR_BAD_HANDLE;
    }
    release_snapshot(false);
    if(estimate){
        std::string_view output(out,*written<=cap?*written:0);
        bool completed=r.tool_rc==CXF_OK&&output.find("timeout=1")==std::string_view::npos&&output.find("io_error=1")==std::string_view::npos;
        if(completed)update(estimate->tool,estimate->nt,millis(r.tool_end_ns-r.tool_start_ns));
        if(completed&&r.progress_ns&&r.progress_ns<=r.tool_end_ns)
            update(estimate->post_spawn,estimate->nprogress,millis(r.tool_end_ns-r.progress_ns));
        if(!r.cancelled&&r.prepare_rc==CXF_OK&&r.prefix_tokens&&r.prepare_start_ns&&r.prefix_ready_ns)
            update(estimate->service,estimate->np,millis(r.prefix_ready_ns-r.prepare_start_ns)/r.prefix_tokens);
    }
    r.finished_ns=clock_ns();return r.tool_rc;
}
extern "C" cxf_result agent_tool_prepare_cancel(AgentToolPrepare *p){
    if(!p)return CXF_ERR_RANGE;
    {std::lock_guard<std::mutex> lock(p->mutex);if(!p->active||!p->accepting_cancel)return CXF_ERR_EMPTY;p->cancelled=true;}
    p->cv.notify_all();auto id=p->ticket.load();if(id)(void)agent_kv_worker_cancel(p->worker,id);return CXF_OK;
}
extern "C" cxf_result agent_tool_prepare_reset(AgentToolPrepare *p){return agent_tool_prepare_run(p,[](AgentNative *n,void *){return agent_native_reset(n);},nullptr);}
extern "C" cxf_result agent_tool_prepare_action(AgentToolPrepare *p,AgentLlmCfg *cfg,const char *log,uint32_t len,char *raw,uint32_t rawcap,char *out,uint32_t cap,uint32_t *written){
    if(!p)return CXF_ERR_RANGE;if(p->active)return CXF_ERR_FULL;
    return owner(p,[&](cxf_llama *){return agent_native_chat_action(p->native,cfg,log,len,raw,rawcap,out,cap,written);});
}
extern "C" void agent_tool_prepare_stats(AgentToolPrepare *p,AgentNativeStats *out){if(p&&out&&!p->active)(void)owner(p,[&](cxf_llama *){agent_native_stats(p->native,out);return CXF_OK;});}

static int output_base_policy(int mode){return (mode==6||mode==8||mode>=10)?4:mode>=5?0:mode;}
extern "C" cxf_result agent_tool_output_policy(AgentToolPrepare *p,int mode){if(!p||mode<0||mode>22)return CXF_ERR_RANGE;if(p->active)return CXF_ERR_FULL;auto rc=owner(p,[&](cxf_llama *e){cxf_llama_defer_sync(e,mode==14||mode==15);return cxf_llama_chunk_policy(e,mode>=16?512:0,mode>=18);});if(rc==CXF_OK)p->output_policy=mode;return rc;}
extern "C" cxf_result agent_tool_prepare_output(AgentToolPrepare *p,const TestRunCfg *cfg,bool enabled,const char *args,uint32_t n,char *out,uint32_t cap,uint32_t *written,AgentToolOutputReport *report){
    if(written)*written=0;if(report){*report={};report->tool_rc=report->stream_rc=CXF_ERR_RANGE;}
    if(!p||!cfg||!cfg->root||!cfg->command||!*cfg->command||!out||!written||!report||n||cap<BASIC_OUT_HINT||p->options.kv_mode!=TOOL_KV_RESIDENT)return CXF_ERR_RANGE;
    {std::lock_guard<std::mutex> lock(p->mutex);if(p->active.exchange(true))return CXF_ERR_FULL;p->cancelled=false;p->accepting_cancel=true;}
    struct Active{AgentToolPrepare *p;~Active(){p->active=false;}} active{p};
    *report={};auto &r=*report;r.entered_ns=clock_ns();r.enabled=enabled;r.stream_rc=CXF_ERR_EMPTY;
    AgentNativeStats before={},after={};cxf_llama_decode_trace tx={},ty={};bool begun=false;
    try{r.stream_rc=owner(p,[&](cxf_llama *){cxf_llama_trace_get(agent_native_engine(p->native,nullptr),&tx);agent_native_stats(p->native,&before);auto rc=agent_native_tool_output_begin(p->native);begun=rc==CXF_OK;return rc;});}catch(...){r.stream_rc=CXF_ERR_NOMEM;}
    struct Queue {AgentToolPrepare *p;AgentToolOutputReport *report;char latest[4096];uint32_t size=0,published=0;bool pending=false,finished=false,failed=false,started=false;void (*launch)(void *)=nullptr;void *launch_ctx=nullptr;} q{p,&r,{}};
    std::thread consumer;
    auto launch=[&]{auto launched=clock_ns();try{consumer=std::thread([&]{
        try{for(;;){char text[4096];uint32_t size;
            {std::unique_lock<std::mutex> lock(p->mutex);p->cv.wait(lock,[&]{return q.pending||q.finished||p->cancelled.load();});if(p->cancelled||(q.finished&&(output_base_policy(p->output_policy)==0||output_base_policy(p->output_policy)==4)))break;if(!q.pending){if(q.finished)break;continue;}if(!q.started&&output_base_policy(p->output_policy)==0){p->cv.wait_for(lock,std::chrono::milliseconds(10),[&]{return q.finished||p->cancelled.load();});if(q.finished||p->cancelled)break;q.started=true;}size=q.size;std::memcpy(text,q.latest,size);q.pending=false;}
            auto queued=clock_ns();auto rc=owner(p,[&](cxf_llama *){auto start=clock_ns();r.owner_queue_ns+=start-queued;if(!r.first_native_ns)r.first_native_ns=start;struct Finish{AgentToolOutputReport &r;uint64_t t;~Finish(){r.last_native_ns=clock_ns();r.owner_work_ns+=r.last_native_ns-t;}} finish{r,start};if(output_base_policy(p->output_policy)==2||p->output_policy==11||p->output_policy==20)return CXF_OK;if(output_base_policy(p->output_policy)==3||p->output_policy==12||p->output_policy==21)return agent_native_tool_output_tokenize(p->native,text,size);return agent_native_tool_output_update(p->native,text,size);});
            if(rc!=CXF_OK){std::lock_guard<std::mutex> lock(p->mutex);r.stream_rc=rc;q.failed=true;break;}
        }}catch(...){std::lock_guard<std::mutex> lock(p->mutex);r.stream_rc=CXF_ERR_IO;q.failed=true;}
    });r.consumer_threads++;}catch(...){r.stream_rc=CXF_ERR_IO;q.failed=true;}r.consumer_launch_ns+=clock_ns()-launched;};
    q.launch=+[](void *ctx){(*static_cast<decltype(launch)*>(ctx))();};q.launch_ctx=&launch;if(enabled&&begun&&p->output_policy!=13)launch();
    auto observe=[](void *ctx,const char *text,uint32_t size,bool final){
        auto &q=*(Queue *)ctx;auto &r=*q.report;auto *p=q.p;if(p->output_policy==10)return;
        std::lock_guard<std::mutex> lock(p->mutex);r.observed++;r.body_bytes=size;if(!r.first_output_ns&&size)r.first_output_ns=clock_ns();
        if(output_base_policy(p->output_policy)==4&&(final||!size||clock_ns()-r.first_output_ns<10000000))return;
        if(q.failed||p->cancelled||(!final&&size<q.published+(p->output_policy==22?1536u:512u))||size==q.published)return;
        if(q.pending)r.overwritten++;std::memcpy(q.latest,text,size);q.size=size;q.published=size;q.pending=true;r.submitted++;if(p->output_policy==13&&r.consumer_threads==0)q.launch(q.launch_ctx);p->cv.notify_all();
    };
    TestRunTrace trace={};r.tool_start_ns=clock_ns();r.tool_rc=(p->output_policy>=8?test_run_exec_output_race:p->output_policy>=5?test_run_exec_output_event:test_run_exec_output)(cfg,args,n,out,cap,written,&trace,enabled&&begun?+observe:nullptr,&q);r.tool_end_ns=clock_ns();r.process=trace;r.body_bytes=trace.retained_output_bytes;if(!r.first_output_ns)r.first_output_ns=trace.first_output_ns;
    {std::lock_guard<std::mutex> lock(p->mutex);q.finished=true;}p->cv.notify_all();if(consumer.joinable())consumer.join();r.joined_ns=clock_ns();
    {std::lock_guard<std::mutex> lock(p->mutex);p->accepting_cancel=false;r.cancelled=p->cancelled;}
    after=before;try{(void)owner(p,[&](cxf_llama *){if(begun&&r.cancelled)(void)agent_native_tool_output_abort(p->native);agent_native_stats(p->native,&after);cxf_llama_trace_get(agent_native_engine(p->native,nullptr),&ty);return CXF_OK;});}catch(...){r.stream_rc=CXF_ERR_IO;}
    r.decode_ns=ty.decode_ns-tx.decode_ns;r.sync_ns=ty.sync_ns-tx.sync_ns;r.decode_cpu_ns=ty.decode_cpu_ns-tx.decode_cpu_ns;r.sync_cpu_ns=ty.sync_cpu_ns-tx.sync_cpu_ns;r.decode_calls=ty.decode_calls-tx.decode_calls;r.sync_calls=ty.sync_calls-tx.sync_calls;r.evaluated=after.output_evaluated-before.output_evaluated;r.tokenized=after.output_tokenized-before.output_tokenized;r.updates=after.output_updates-before.output_updates;r.replayed=after.output_replayed-before.output_replayed;r.front_ns=after.output_front_ns-before.output_front_ns;r.prefill_ns=after.output_prefill_ns-before.output_prefill_ns;r.returned_ns=clock_ns();return r.tool_rc;
}
