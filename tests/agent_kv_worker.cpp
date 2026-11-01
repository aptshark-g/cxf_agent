#include "agent_kv_worker.h"
#include "../src/llama_internal.h"
#include "llama.h"
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

static uint64_t now_ns() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
static bool same_file(const struct stat &a,const struct stat &b) {
#ifdef __APPLE__
    auto am=a.st_mtimespec, bm=b.st_mtimespec, ac=a.st_ctimespec, bc=b.st_ctimespec;
#else
    auto am=a.st_mtim, bm=b.st_mtim, ac=a.st_ctim, bc=b.st_ctim;
#endif
    return S_ISREG(b.st_mode) && a.st_dev==b.st_dev && a.st_ino==b.st_ino && a.st_size==b.st_size &&
           am.tv_sec==bm.tv_sec && am.tv_nsec==bm.tv_nsec && ac.tv_sec==bc.tv_sec && ac.tv_nsec==bc.tv_nsec;
}
struct AgentKvWorker {
    struct Snapshot { uint64_t id=0; cxf_llama_kv_image image={};std::string path;struct stat identity={};int held_fd=-1; };
    struct Ticket {
        uint64_t id=0, snapshot=0;bool cancelled=false, done=false, finishing=false, committing=false;
        cxf_result rc=CXF_ERR_EMPTY;AgentKvReport report={};
    };
    struct Call { std::function<cxf_result()> fn;std::condition_variable cv;bool done=false;cxf_result rc=CXF_ERR_IO; };
    struct Request { Call *call=nullptr;uint64_t ticket=0; };
    cxf_llama_params params;std::string model,backend,directory;
    cxf_llama *engine=nullptr;
    uint64_t budget=0,used=0,next_id=1;
    std::array<Snapshot,8> snapshots;
    std::array<Ticket,16> tickets;
    std::array<Request,16> queue;
    size_t head=0,count=0;
    std::mutex mutex;std::condition_variable cv;
    std::thread thread;bool startup=false,stopping=false;cxf_result open_rc=CXF_ERR_IO;

    Snapshot *snapshot(uint64_t id) { for(auto &x:snapshots)if(x.id==id)return &x;return nullptr; }
    Ticket *ticket(uint64_t id) { for(auto &x:tickets)if(x.id==id)return &x;return nullptr; }
    static cxf_result guarded(const std::function<cxf_result()> &fn) {
        try{return fn();}catch(const std::bad_alloc &){return CXF_ERR_NOMEM;}catch(...){return CXF_ERR_IO;}
    }
    cxf_result sync(std::function<cxf_result()> fn) {
        Call call;call.fn=std::move(fn);
        std::unique_lock<std::mutex> lock(mutex);
        if(stopping)return CXF_ERR_BAD_HANDLE;
        if(count==queue.size())return CXF_ERR_FULL;
        queue[(head+count)%queue.size()]={&call,0};count++;cv.notify_one();
        call.cv.wait(lock,[&]{return call.done;});return call.rc;
    }
    bool valid(const Snapshot &s,cxf_llama_view *out=nullptr) {
        cxf_llama_view v={};auto rc=cxf_llama_session_view(engine,s.image.session,&v);
        if(out)*out=v;
        return s.held_fd<0 && rc==CXF_OK && v.revision==s.image.revision && v.committed==s.image.evaluated;
    }
    bool cancelled(uint64_t id) {
        std::lock_guard<std::mutex> lock(mutex);auto *t=ticket(id);return !t || t->cancelled;
    }
    void undo_cancelled(const Snapshot &s,uint64_t id,AgentKvReport &report) {
        if(!report.restored_bytes || !valid(s))return;
        // A second live hint for the same version can still use this work.
        {
            std::lock_guard<std::mutex> lock(mutex);
            for(const auto &t:tickets)if(t.id && t.id!=id && !t.cancelled) {
                auto *other=snapshot(t.snapshot);
                if(other && other->image.session.owner==s.image.session.owner &&
                   cxf_handle_eq(other->image.session.slot,s.image.session.slot) && other->image.revision==s.image.revision)return;
            }
        }
        if(cxf_llama_kv_evict(engine,s.image.session,s.image.revision)==CXF_OK)
            report.wasted_bytes=report.restored_bytes;
    }
    void prepare(uint64_t id) {
        AgentKvReport report={};uint64_t snap=0;
        {
            std::lock_guard<std::mutex> lock(mutex);auto *t=ticket(id);if(!t)return;
            report=t->report;snap=t->snapshot;
        }
        report.started_ns=now_ns();
        {std::lock_guard<std::mutex> lock(mutex);if(auto *t=ticket(id))t->report.started_ns=report.started_ns;}
        cxf_result rc=CXF_ERR_BAD_HANDLE;auto *s=snapshot(snap);
        if(!cancelled(id) && s && valid(*s)) {
            cxf_llama_view v={};valid(*s,&v);
            if(v.evaluated==s->image.evaluated)rc=CXF_OK;
            else if(v.evaluated)rc=CXF_ERR_RANGE;
            else {
                uint64_t start=now_ns();struct stat current={};
                bool ok=::lstat(s->path.c_str(),&current)==0 && same_file(s->identity,current);
                uint32_t header[3]={};
                if(ok) {
                    FILE *file=std::fopen(s->path.c_str(),"rb");
                    ok=file && std::fread(header,sizeof(header),1,file)==1;
                    if(file)std::fclose(file);
                    ok=ok && header[0]==LLAMA_STATE_SEQ_MAGIC && header[1]==LLAMA_STATE_SEQ_VERSION && !header[2];
                }
                report.verify_ns=now_ns()-start;rc=CXF_ERR_FORMAT;
                if(ok && !cancelled(id)) {
                    start=now_ns();report.restore_start_ns=start;
                    {std::lock_guard<std::mutex> lock(mutex);if(auto *t=ticket(id))t->report.restore_start_ns=start;}
                    rc=cxf_llama_kv_restore_file(engine,&s->image,s->path.c_str());
                    report.restore_end_ns=now_ns();report.restore_ns=report.restore_end_ns-start;
                    if(rc==CXF_OK) {
                        report.restored_bytes=s->image.bytes;report.restored_tokens=s->image.evaluated;
                        struct stat after={};
                        if(::lstat(s->path.c_str(),&after) || !same_file(s->identity,after)) {
                            (void)cxf_llama_kv_evict(engine,s->image.session,s->image.revision);rc=CXF_ERR_FORMAT;
                        }
                    }
                }
            }
        }
        report.cancelled=cancelled(id);
        if(report.cancelled) {if(s)undo_cancelled(*s,id,report);rc=CXF_ERR_BAD_HANDLE;}
        report.ready_ns=now_ns();
        std::lock_guard<std::mutex> lock(mutex);
        if(auto *t=ticket(id)){t->report=report;t->rc=rc;t->done=true;}
    }
    void loop() {
        open_rc=cxf_llama_open(&params,&engine);
        {std::lock_guard<std::mutex> lock(mutex);startup=true;cv.notify_all();}
        for(;;) {
            Request request;
            {
                std::unique_lock<std::mutex> lock(mutex);cv.wait(lock,[&]{return stopping || count;});
                if(!count && stopping)break;
                request=queue[head];head=(head+1)%queue.size();count--;
            }
            if(request.call) {
                auto rc=guarded(request.call->fn);
                std::lock_guard<std::mutex> lock(mutex);request.call->rc=rc;request.call->done=true;request.call->cv.notify_one();
            } else {
                try {prepare(request.ticket);}catch(...) {
                    std::lock_guard<std::mutex> lock(mutex);
                    if(auto *t=ticket(request.ticket)){t->rc=CXF_ERR_IO;t->done=true;t->report.ready_ns=now_ns();}
                }
            }
        }
        cxf_llama_close(engine);engine=nullptr;
        for(auto &s:snapshots)if(s.id){if(s.held_fd>=0)(void)::close(s.held_fd);else (void)std::remove(s.path.c_str());}
        if(!directory.empty())(void)::rmdir(directory.c_str());
    }
    ~AgentKvWorker() {
        if(thread.joinable()) {
            {std::lock_guard<std::mutex> lock(mutex);stopping=true;for(auto &t:tickets)if(t.id)t.cancelled=true;}
            cv.notify_one();thread.join();
        } else if(!directory.empty())(void)::rmdir(directory.c_str());
    }
};

extern "C" cxf_result agent_kv_worker_open(const cxf_llama_params *params,const char *parent,
                                            uint64_t budget,AgentKvWorker **out) {
    if(!out)return CXF_ERR_RANGE;*out=nullptr;
    if(!params || !params->model_path || !parent || !*parent || !budget)return CXF_ERR_RANGE;
    AgentKvWorker *p=nullptr;
    try {
        p=new AgentKvWorker;p->params=*params;p->model=params->model_path;
        if(params->backend_path)p->backend=params->backend_path;
        p->params.model_path=p->model.c_str();p->params.backend_path=params->backend_path?p->backend.c_str():nullptr;
        p->budget=budget;std::string pattern=std::string(parent)+"/cxf-kv-XXXXXX";
        if(!::mkdtemp(&pattern[0])){delete p;return CXF_ERR_IO;}
        p->directory=std::move(pattern);p->thread=std::thread([p]{p->loop();});
        {std::unique_lock<std::mutex> lock(p->mutex);p->cv.wait(lock,[&]{return p->startup;});}
        if(p->open_rc!=CXF_OK){auto rc=p->open_rc;delete p;return rc;}
        *out=p;return CXF_OK;
    }catch(const std::bad_alloc &){delete p;return CXF_ERR_NOMEM;}catch(...){delete p;return CXF_ERR_IO;}
}
extern "C" void agent_kv_worker_close(AgentKvWorker *p){delete p;}
extern "C" cxf_result agent_kv_worker_run(AgentKvWorker *p,AgentKvOwnerFn fn,void *ctx) {
    if(!p || !fn)return CXF_ERR_RANGE;
    try{return p->sync([=]{return fn(p->engine,ctx);});}catch(const std::bad_alloc &){return CXF_ERR_NOMEM;}catch(...){return CXF_ERR_IO;}
}
extern "C" cxf_result agent_kv_worker_save(AgentKvWorker *p,cxf_llama_session h,uint64_t revision,AgentKvSnapshot *out) {
    if(out)*out=0;if(!p || !out)return CXF_ERR_RANGE;
    try{return p->sync([=] {
        AgentKvWorker::Snapshot *s=nullptr;
        for(auto &x:p->snapshots)if(!x.id){s=&x;break;}
        if(!s)return CXF_ERR_FULL;
        uint64_t id;
        {std::lock_guard<std::mutex> lock(p->mutex);if(p->next_id==UINT64_MAX)return CXF_ERR_FULL;id=p->next_id++;}
        std::string path=p->directory+"/"+std::to_string(id)+".kv";
        cxf_llama_kv_image image={};auto rc=cxf_llama_kv_save_file(p->engine,h,revision,path.c_str(),p->budget-p->used,&image);
        if(rc!=CXF_OK)return rc;
        struct stat identity={};
        if(::lstat(path.c_str(),&identity) || !S_ISREG(identity.st_mode) || (uint64_t)identity.st_size!=image.bytes) {
            (void)std::remove(path.c_str());return CXF_ERR_IO;
        }
        s->path=std::move(path);s->image=image;s->identity=identity;s->id=id;p->used+=image.bytes;*out=id;return CXF_OK;
    });}catch(const std::bad_alloc &){return CXF_ERR_NOMEM;}catch(...){return CXF_ERR_IO;}
}
extern "C" cxf_result agent_kv_worker_drop(AgentKvWorker *p,AgentKvSnapshot id) {
    if(!p || !id)return CXF_ERR_RANGE;
    try{return p->sync([=] {
        auto *s=p->snapshot(id);if(!s)return CXF_ERR_BAD_HANDLE;
        {std::lock_guard<std::mutex> lock(p->mutex);for(const auto &t:p->tickets)if(t.id && t.snapshot==id)return CXF_ERR_FULL;}
        int rc=s->held_fd>=0?::close(s->held_fd):std::remove(s->path.c_str());
        if(rc&&s->held_fd<0)return CXF_ERR_IO;
        // Do not retry close on a possibly reused descriptor after EINTR.
        p->used-=s->image.bytes;*s={};return rc?CXF_ERR_IO:CXF_OK;
    });}catch(...){return CXF_ERR_IO;}
}
extern "C" cxf_result agent_kv_worker_unlink_hold(AgentKvWorker *p,AgentKvSnapshot id){
    if(!p||!id)return CXF_ERR_RANGE;
    try{return p->sync([=]{
        auto *s=p->snapshot(id);if(!s||s->held_fd>=0)return CXF_ERR_BAD_HANDLE;
        {std::lock_guard<std::mutex> lock(p->mutex);for(const auto &t:p->tickets)if(t.id&&t.snapshot==id)return CXF_ERR_FULL;}
        int fd=::open(s->path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return CXF_ERR_IO;
        struct stat identity={};
        if(::fstat(fd,&identity)||!same_file(s->identity,identity)||::unlink(s->path.c_str())){::close(fd);return CXF_ERR_IO;}
        s->held_fd=fd;return CXF_OK;
    });}catch(...){return CXF_ERR_IO;}
}
extern "C" cxf_result agent_kv_worker_evict(AgentKvWorker *p,cxf_llama_session h,uint64_t revision) {
    if(!p)return CXF_ERR_RANGE;
    try{return p->sync([=]{return cxf_llama_kv_evict(p->engine,h,revision);});}catch(...){return CXF_ERR_IO;}
}
extern "C" cxf_result agent_kv_worker_prepare(AgentKvWorker *p,AgentKvSnapshot snapshot,AgentKvTicket *out) {
    if(out)*out=0;if(!p || !out || !snapshot)return CXF_ERR_RANGE;
    try {
        std::lock_guard<std::mutex> lock(p->mutex);
        if(p->stopping)return CXF_ERR_BAD_HANDLE;
        if(p->count==p->queue.size() || p->next_id==UINT64_MAX)return CXF_ERR_FULL;
        AgentKvWorker::Ticket *t=nullptr;for(auto &x:p->tickets)if(!x.id){t=&x;break;}
        if(!t)return CXF_ERR_FULL;
        *t={};t->id=p->next_id++;t->snapshot=snapshot;t->report.queued_ns=now_ns();*out=t->id;
        p->queue[(p->head+p->count)%p->queue.size()]={nullptr,t->id};p->count++;p->cv.notify_one();return CXF_OK;
    }catch(...){return CXF_ERR_IO;}
}
extern "C" cxf_result agent_kv_worker_cancel(AgentKvWorker *p,AgentKvTicket id) {
    if(!p || !id)return CXF_ERR_RANGE;
    std::lock_guard<std::mutex> lock(p->mutex);auto *t=p->ticket(id);
    if(!t || t->committing)return CXF_ERR_BAD_HANDLE;t->cancelled=true;return CXF_OK;
}
extern "C" cxf_result agent_kv_worker_poll(AgentKvWorker *p,AgentKvTicket id,AgentKvReport *out,bool *ready) {
    if(!p || !id || !out || !ready)return CXF_ERR_RANGE;
    std::lock_guard<std::mutex> lock(p->mutex);auto *t=p->ticket(id);if(!t)return CXF_ERR_BAD_HANDLE;
    *out=t->report;out->cancelled=t->cancelled;*ready=t->done;return CXF_OK;
}
extern "C" cxf_result agent_kv_worker_finish(AgentKvWorker *p,AgentKvTicket id,AgentKvOwnerFn fn,
                                             void *ctx,AgentKvReport *out) {
    uint64_t entered=now_ns();
    if(out)*out={};if(!p || !id)return CXF_ERR_RANGE;
    {
        std::lock_guard<std::mutex> lock(p->mutex);auto *t=p->ticket(id);
        if(!t || t->finishing)return CXF_ERR_BAD_HANDLE;t->finishing=true;
    }
    cxf_result rc;
    try {rc=p->sync([=] {
        uint64_t owner_start=now_ns();
        AgentKvReport report={};uint64_t snap=0;cxf_result result;
        {
            std::lock_guard<std::mutex> lock(p->mutex);auto *t=p->ticket(id);
            if(!t || !t->done)return CXF_ERR_BAD_HANDLE;
            report=t->report;report.cancelled=t->cancelled;snap=t->snapshot;result=t->rc;t->committing=true;
        }
        auto *s=p->snapshot(snap);cxf_llama_view v={};
        if(report.cancelled) {if(s)p->undo_cancelled(*s,id,report);result=CXF_ERR_BAD_HANDLE;}
        else if(result==CXF_OK && (!s || !p->valid(*s,&v) || v.evaluated!=s->image.evaluated))result=CXF_ERR_BAD_HANDLE;
        if(result==CXF_OK && fn)result=AgentKvWorker::guarded([=]{return fn(p->engine,ctx);});
        report.finish_enter_ns=entered;report.finish_owner_ns=owner_start;
        report.finished_ns=now_ns();if(out)*out=report;
        {std::lock_guard<std::mutex> lock(p->mutex);if(auto *t=p->ticket(id))*t={};}
        return result;
    });}catch(const std::bad_alloc &){rc=CXF_ERR_NOMEM;}catch(...){rc=CXF_ERR_IO;}
    {std::lock_guard<std::mutex> lock(p->mutex);if(auto *t=p->ticket(id))t->finishing=false;}
    if(out)out->finish_return_ns=now_ns();
    return rc;
}
