/* Process-local diagnostic interception of public Metal command-buffer methods.
 * Optional timestamp-only completion markers; submission/wait order unchanged. No other process touched. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <mach/mach_time.h>
#include <mutex>
#include <atomic>
#include <memory>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cstdio>
#include <cassert>
#include "agent_metal_probe.h"
struct Notification {std::atomic<double> begin{0},end{0};};
struct Rec {id<MTLCommandBuffer> cb;uint64_t id,queue;double commit,commit_end;uint64_t batch;std::shared_ptr<Notification> note;};
struct Wait {uint64_t cb;int api;unsigned long entry_status;double begin,end,gpu_start,gpu_end;};
static std::mutex mu;
static std::vector<Rec*> recs;
static std::vector<Wait> waits;
static std::unordered_map<void*,Rec*> lookup;
static std::unordered_map<void*,uint64_t> queues;static uint64_t scope_id=0,cb_id=0,queue_id=0;static int scope_kind=0;static std::atomic<bool> enabled{false};
static std::atomic<uint64_t> active_batch{0};static uint64_t batch_id=0;
static std::atomic<int> api_phase{0};static std::atomic<bool> completion_marker{false};
static FILE *dest=nullptr;static mach_timebase_info_data_t tb;
using Call=void(*)(id,SEL);static Call orig_commit=nullptr,orig_wait=nullptr;
static double stamp(){return mach_absolute_time()*(double)tb.numer/tb.denom/1e9;}
static void commit_hook(id self,SEL sel){
 if(!enabled.load(std::memory_order_relaxed)){orig_commit(self,sel);return;}
 Rec *r=nullptr;{std::lock_guard<std::mutex> l(mu);if(enabled&&scope_kind){auto cb=(id<MTLCommandBuffer>)self;void *q=(void*)cb.commandQueue;if(!queues.count(q))queues[q]=++queue_id;r=new Rec{(id<MTLCommandBuffer>)[self retain],++cb_id,queues[q],stamp(),0,active_batch.load(),{}};if(completion_marker.load(std::memory_order_relaxed))r->note=std::make_shared<Notification>();recs.push_back(r);lookup[(void*)self]=r;}}
 if(r&&r->note){auto note=r->note;[(id<MTLCommandBuffer>)self addCompletedHandler:^(id<MTLCommandBuffer> cb){(void)cb;note->begin.store(stamp(),std::memory_order_release);note->end.store(stamp(),std::memory_order_release);}];}
 orig_commit(self,sel);
 if(r){std::lock_guard<std::mutex> l(mu);r->commit_end=stamp();}
}
static void wait_hook(id self,SEL sel){
 if(!enabled.load(std::memory_order_relaxed)){orig_wait(self,sel);return;}
 int api=api_phase.load(std::memory_order_relaxed);double begin=stamp();unsigned long entry_status=((id<MTLCommandBuffer>)self).status;orig_wait(self,sel);double end=stamp();
 std::lock_guard<std::mutex> l(mu);if(enabled&&scope_kind){auto it=lookup.find((void*)self);auto cb=(id<MTLCommandBuffer>)self;waits.push_back({it==lookup.end()?0:it->second->id,api,entry_status,begin,end,cb.GPUStartTime,cb.GPUEndTime});}
}
extern "C" void agent_metal_probe_init(const char *path){if(!path)return;@autoreleasepool{
 mach_timebase_info(&tb);dest=fopen(path,"w");assert(dest);id<MTLDevice>d=MTLCreateSystemDefaultDevice();assert(d);id<MTLCommandQueue>q=[d newCommandQueue];id<MTLCommandBuffer>b=[q commandBufferWithUnretainedReferences];Class c=object_getClass(b);
 Call cm=(Call)class_getMethodImplementation(c,@selector(commit));Call wm=(Call)class_getMethodImplementation(c,@selector(waitUntilCompleted));assert(cm&&wm);orig_commit=cm;orig_wait=wm;
 class_addMethod(c,@selector(commit),(IMP)commit_hook,"v@:");class_replaceMethod(c,@selector(commit),(IMP)commit_hook,"v@:");class_addMethod(c,@selector(waitUntilCompleted),(IMP)wait_hook,"v@:");class_replaceMethod(c,@selector(waitUntilCompleted),(IMP)wait_hook,"v@:");
 fprintf(dest,"{\"kind\":\"metal_metadata\",\"class\":\"%s\",\"timebase_numer\":%u,\"timebase_denom\":%u,\"completion_marker_supported\":true}\n",class_getName(c),tb.numer,tb.denom);fflush(dest);[q release];[d release];enabled=true;
}}
extern "C" void agent_metal_probe_batch(unsigned pos,unsigned tokens){std::lock_guard<std::mutex>l(mu);if(!enabled||!scope_kind)return;active_batch=++batch_id;fprintf(dest,"{\"kind\":\"metal_batch\",\"scope_id\":%llu,\"scope_kind\":%d,\"batch\":%llu,\"position\":%u,\"tokens\":%u,\"begin\":%.9f}\n",scope_id,scope_kind,batch_id,pos,tokens,stamp());}
extern "C" void agent_metal_probe_api(int phase){if(enabled.load(std::memory_order_relaxed))api_phase.store(phase,std::memory_order_relaxed);}
extern "C" void agent_metal_probe_completion(int on){completion_marker.store(on,std::memory_order_relaxed);}
extern "C" void agent_metal_probe_enable(int on){std::lock_guard<std::mutex>l(mu);enabled=dest&&on;}
extern "C" void agent_metal_probe_begin(int kind){std::lock_guard<std::mutex>l(mu);if(!enabled)return;assert(!scope_kind);scope_id++;scope_kind=kind;active_batch=0;}
extern "C" void agent_metal_probe_end(){std::unique_lock<std::mutex>l(mu);if(!enabled||!scope_kind)return;
 unsigned valid=0;double gpu_sum=0,queue_sum=0,driver_sum=0,wake_sum=0,max_wake=0;unsigned blocking=0,late=0;
 for(auto*r:recs){double gs=r->cb.GPUStartTime,ge=r->cb.GPUEndTime,ks=r->cb.kernelStartTime,ke=r->cb.kernelEndTime;bool ok=gs>0&&ge>=gs&&gs>=r->commit-.001&&ge<=stamp()+.001;if(ok){valid++;gpu_sum+=ge-gs;queue_sum+=gs-r->commit;driver_sum+=std::max(0.0,ke-ks);}
 fprintf(dest,"{\"kind\":\"metal_command\",\"scope_id\":%llu,\"scope_kind\":%d,\"cb\":%llu,\"queue\":%llu,\"batch\":%llu,\"commit\":%.9f,\"commit_end\":%.9f,\"kernel_start\":%.9f,\"kernel_end\":%.9f,\"gpu_start\":%.9f,\"gpu_end\":%.9f,\"notify_registered\":%s,\"notify_begin\":%.9f,\"notify_end\":%.9f,\"valid\":%s,\"status\":%lu}\n",scope_id,scope_kind,r->id,r->queue,r->batch,r->commit,r->commit_end,ks,ke,gs,ge,r->note?"true":"false",r->note?r->note->begin.load(std::memory_order_acquire):0,r->note?r->note->end.load(std::memory_order_acquire):0,ok?"true":"false",(unsigned long)r->cb.status);
 }
 for(auto&w:waits){bool blocked=w.gpu_end>0&&w.begin<w.gpu_end&&w.end>=w.gpu_end;if(blocked){blocking++;double lag=w.end-w.gpu_end;wake_sum+=lag;max_wake=std::max(max_wake,lag);}else late++;
 fprintf(dest,"{\"kind\":\"metal_wait\",\"scope_id\":%llu,\"scope_kind\":%d,\"cb\":%llu,\"entry_status\":%lu,\"api_phase\":%d,\"begin\":%.9f,\"end\":%.9f,\"gpu_start\":%.9f,\"gpu_end\":%.9f,\"blocking\":%s}\n",scope_id,scope_kind,w.cb,w.entry_status,w.api,w.begin,w.end,w.gpu_start,w.gpu_end,blocked?"true":"false");}
 fflush(dest);fprintf(stderr,"[metal-probe] {\"kind\":\"metal\",\"scope_id\":%llu,\"scope_kind\":%d,\"buffers\":%zu,\"valid_buffers\":%u,\"gpu_sum_ms\":%.6f,\"queue_sum_ms\":%.6f,\"driver_sum_ms\":%.6f,\"blocking_waits\":%u,\"late_waits\":%u,\"wake_sum_ms\":%.6f,\"wake_max_ms\":%.6f}\n",scope_id,scope_kind,recs.size(),valid,gpu_sum*1000,queue_sum*1000,driver_sum*1000,blocking,late,wake_sum*1000,max_wake*1000);fflush(stderr);
 auto done=std::move(recs);recs.clear();waits.clear();lookup.clear();scope_kind=0;l.unlock();for(auto*r:done){[r->cb release];delete r;}
}
