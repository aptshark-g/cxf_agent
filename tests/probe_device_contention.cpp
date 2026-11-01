#define _DARWIN_C_SOURCE
#include "cxf/llama.h"
#include "../src/llama_internal.h"
#include "agent_metal_probe.h"
#include "llama.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <time.h>
#define OK(x) do{auto rc=(x);if(rc!=CXF_OK){fprintf(stderr,"isolation error line%d rc%d\n",__LINE__,rc);abort();}}while(0)
static uint64_t ns(clockid_t c=CLOCK_MONOTONIC_RAW){timespec t;assert(!clock_gettime(c,&t));return uint64_t(t.tv_sec)*1000000000+t.tv_nsec;}
static cxf_llama_view view(cxf_llama *e,cxf_llama_session h){cxf_llama_view v;OK(cxf_llama_session_view(e,h,&v));return v;}
static void append(cxf_llama *e,cxf_llama_session h,const int32_t *p,unsigned n){OK(cxf_llama_append(e,h,view(e,h).revision,p,n));}
static void fill(cxf_llama *e,cxf_llama_session h){uint32_t done;OK(cxf_llama_prefill(e,h,view(e,h).revision,UINT32_MAX,true,&done));}
struct Pressure {
 std::atomic<bool> stop{false};std::mutex mu;std::condition_variable cv;bool go=false,ready=false;uint64_t operations=0,checksum=0,wall=0,cpu=0;std::thread thread;
 Pressure(int mode,std::vector<uint64_t>&memory){thread=std::thread([&,mode]{std::unique_lock<std::mutex> l(mu);cv.wait(l,[&]{return go;});l.unlock();auto w=ns(),c=ns(CLOCK_THREAD_CPUTIME_ID);uint64_t x=17,index=0,count=0;
  if(mode==0){l.lock();ready=true;cv.notify_all();cv.wait(l,[&]{return stop.load();});l.unlock();}
  else{bool first=true;while(!stop.load(std::memory_order_relaxed)){for(unsigned j=0;j<4096;j++){x^=j+count;x=(x<<7)|(x>>57);x+=UINT64_C(0x9e3779b97f4a7c15);if(mode==2){memory[index]+=x;x^=memory[index];index=(index+8)&(memory.size()-1);}count++;}if(first){l.lock();ready=true;cv.notify_all();l.unlock();first=false;}}}
  wall=ns()-w;cpu=ns(CLOCK_THREAD_CPUTIME_ID)-c;operations=count;checksum=x;
 });}
 void start(){std::unique_lock<std::mutex>l(mu);go=true;cv.notify_all();cv.wait(l,[&]{return ready;});}
 void finish(){stop=true;cv.notify_all();thread.join();}
};
int main(int argc,char **argv){assert(argc==5);int repeats=atoi(argv[4]);assert(repeats==1||repeats==7);agent_metal_probe_init(argv[3]);agent_metal_probe_completion(1);
 auto p=cxf_llama_default_params();p.model_path=argv[1];p.backend_path=argv[2];p.context_tokens=8192;p.batch_tokens=640;p.max_sessions=4;p.threads=4;cxf_llama *e;OK(cxf_llama_open(&p,&e));cxf_llama_trace_observer(e,agent_metal_probe_api);cxf_llama_trace_batch_observer(e,agent_metal_probe_batch);cxf_llama_trace_enable(e,true);
 auto vocab=llama_model_get_vocab(cxf_llama_native_model(e));std::string text;for(int i=0;i<5000;i++)text+="reading ";std::vector<int32_t> tokens(8192);int count=llama_tokenize(vocab,text.data(),text.size(),tokens.data(),tokens.size(),true,true);assert(count>3000);tokens.resize(count);
 std::vector<uint64_t> memory(64*1024*1024/sizeof(uint64_t));for(size_t i=0;i<memory.size();i++)memory[i]=i+17;
 cxf_llama_session root;OK(cxf_llama_session_new(e,&root));append(e,root,tokens.data(),2048);fill(e,root);unsigned samples=0;printf("{\"kind\":\"metadata\",\"batch_tokens\":640,\"context_tokens\":8192,\"memory_bytes\":67108864,\"repeats\":%d}\n",repeats);
 for(unsigned prefix:{768u,2048u})for(unsigned amount:{128u,224u,512u,582u}){
  cxf_llama_session reference;OK(cxf_llama_session_new(e,&reference));append(e,reference,tokens.data(),prefix+amount);OK(cxf_llama_chunk_policy(e,512,false));fill(e,reference);int32_t expected;OK(cxf_llama_greedy(e,reference,view(e,reference).revision,&expected));OK(cxf_llama_session_close(e,reference));
  for(int rep=-1;rep<repeats;rep++)for(int k=0;k<6;k++){int condition=(rep+1+k)%6,pressure=condition/2;bool coalesce=condition%2;OK(cxf_llama_chunk_policy(e,512,coalesce));cxf_llama_session h;OK(cxf_llama_fork(e,root,view(e,root).revision,prefix,&h));append(e,h,tokens.data()+prefix,amount);Pressure worker(pressure,memory);worker.start();cxf_llama_decode_trace a,b;cxf_llama_trace_get(e,&a);agent_metal_probe_begin(3);auto start=ns(),cpu=ns(CLOCK_THREAD_CPUTIME_ID);uint32_t done;OK(cxf_llama_prefill(e,h,view(e,h).revision,UINT32_MAX,true,&done));cpu=ns(CLOCK_THREAD_CPUTIME_ID)-cpu;auto elapsed=ns()-start;worker.finish();cxf_llama_trace_get(e,&b);agent_metal_probe_end();assert(done==amount);int32_t actual;OK(cxf_llama_greedy(e,h,view(e,h).revision,&actual));assert(actual==expected);std::vector<int32_t> canonical(prefix+amount);OK(cxf_llama_read(e,h,0,canonical.data(),canonical.size()));assert(!memcmp(canonical.data(),tokens.data(),canonical.size()*sizeof(int32_t)));OK(cxf_llama_session_close(e,h));samples++;
   printf("{\"kind\":\"isolation\",\"phase\":\"%s\",\"rep\":%d,\"scope_id\":%u,\"prefix_tokens\":%u,\"new_tokens\":%u,\"pressure\":\"%s\",\"policy\":\"%s\",\"wall_ms\":%.6f,\"owner_cpu_ms\":%.6f,\"decode_ms\":%.6f,\"sync_ms\":%.6f,\"decode_calls\":%llu,\"worker_wall_ms\":%.6f,\"worker_cpu_ms\":%.6f,\"worker_operations\":%llu,\"logical_memory_bytes\":%llu,\"worker_checksum\":%llu,\"greedy_token\":%d,\"correct\":true}\n",rep<0?"warmup":"evaluation",rep,samples,prefix,amount,pressure==0?"idle":pressure==1?"arithmetic":"memory",coalesce?"coalesce":"split",elapsed/1e6,cpu/1e6,(b.decode_ns-a.decode_ns)/1e6,(b.sync_ns-a.sync_ns)/1e6,(unsigned long long)(b.decode_calls-a.decode_calls),worker.wall/1e6,worker.cpu/1e6,(unsigned long long)worker.operations,(unsigned long long)(pressure==2?worker.operations*16:0),(unsigned long long)worker.checksum,actual);fflush(stdout);
  }
 }
 OK(cxf_llama_session_close(e,root));cxf_llama_close(e);printf("{\"kind\":\"correctness\",\"passed\":true,\"samples\":%u,\"fresh_shapes\":8}\n{\"kind\":\"closed\",\"passed\":true}\n",samples);
}
