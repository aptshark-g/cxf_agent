#include "agent_stream.h"
extern "C" {
#include "agent_basic.h"
#include "sha256.h"
}
#include "agent_native_messages.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <spawn.h>
#include <poll.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <crt_externs.h>
#define ENVIRON (*_NSGetEnviron())
#else
extern char **environ;
#define ENVIRON environ
#endif
#define OK(x) do{auto rc_=(x);if(rc_!=CXF_OK){std::fprintf(stderr,"FAIL %d %s rc=%d\n",__LINE__,#x,rc_);std::abort();}}while(0)
static uint64_t tick(){return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
static double ms(uint64_t n){return n/1e6;}
static double cpu_ms(const timeval &t){return t.tv_sec*1000.0+t.tv_usec/1000.0;}
static std::string hash(const void *p,size_t n){unsigned char h[32];sha256_hash(h,(const unsigned char *)p,n);const char *hex="0123456789abcdef";std::string s;for(auto x:h){s+=hex[x>>4];s+=hex[x&15];}return s;}
static void emit(common_json j){puts(j.dump().c_str());fflush(stdout);}
using Fn=std::function<cxf_result(cxf_llama *)>;
static void owner(AgentKvWorker *w,Fn f){OK(agent_kv_worker_run(w,[](cxf_llama *e,void *c){return (*(Fn *)c)(e);},&f));}
static cxf_llama_view view(cxf_llama *e,cxf_llama_session h){cxf_llama_view v;OK(cxf_llama_session_view(e,h,&v));return v;}
static void no_files(const std::string &root){for(const auto &p:std::filesystem::recursive_directory_iterator(root))assert(p.path().extension()!=".kv");}
struct Reference{std::vector<int32_t> ids;int32_t token=0;};
struct Output{Reference *reference;bool make_reference=false;uint64_t first_ns=0,oracle_ns=0;int32_t token=0;std::string canonical;};
static cxf_result final(cxf_llama *e,cxf_llama_session h,const char *prompt,uint32_t bytes,void *ctx){
 auto &out=*(Output *)ctx;auto v=view(e,h);assert(v.committed==v.evaluated);OK(cxf_llama_greedy(e,h,v.revision,&out.token));out.first_ns=tick();auto t=tick();
 std::vector<int32_t> ids(v.committed);OK(cxf_llama_read(e,h,0,ids.data(),v.committed));uint32_t n=0;auto rc=cxf_llama_tokenize(e,prompt,bytes,false,nullptr,0,&n);assert(rc==CXF_OK||rc==CXF_ERR_FULL);std::vector<int32_t> full(n);OK(cxf_llama_tokenize(e,prompt,bytes,false,full.data(),n,&n));assert(full==ids);
 if(out.make_reference){out.reference->ids=full;out.reference->token=out.token;}
 else assert(ids==out.reference->ids&&out.token==out.reference->token);
 out.canonical=hash(ids.data(),ids.size()*sizeof(int32_t));out.oracle_ns=tick()-t;return CXF_OK;
}
static void feed(AgentStream *s,const std::string &wire){size_t offset=0;bool retry=false;do{uint32_t used=0;auto rc=agent_stream_feed(s,offset<wire.size()?wire.data()+offset:nullptr,(uint32_t)(wire.size()-offset),&used);offset+=used;if(rc==CXF_ERR_FULL){AgentStreamReport report;agent_stream_report(s,&report);assert(!report.failed);do{rc=agent_stream_wait_input(s,100);}while(rc==CXF_ERR_EMPTY);OK(rc);retry=true;}else{OK(rc);retry=false;}}while(offset<wire.size()||retry);}
struct Producer{uint64_t start=0,spawned=0,reaped=0;double cpu=0;common_json arrivals=common_json::array();std::string content;};
static Producer produce(AgentStream *stream,const std::string &helper,const std::string &path,unsigned long iterations){
 Producer p;int fds[2];assert(!pipe(fds));for(int fd:fds)assert(!fcntl(fd,F_SETFD,FD_CLOEXEC));
 posix_spawn_file_actions_t actions;assert(!posix_spawn_file_actions_init(&actions));assert(!posix_spawn_file_actions_addclose(&actions,fds[0]));assert(!posix_spawn_file_actions_addopen(&actions,STDIN_FILENO,"/dev/null",O_RDONLY,0));assert(!posix_spawn_file_actions_adddup2(&actions,fds[1],STDOUT_FILENO));assert(!posix_spawn_file_actions_addclose(&actions,fds[1]));
 posix_spawnattr_t attr;assert(!posix_spawnattr_init(&attr));assert(!posix_spawnattr_setflags(&attr,POSIX_SPAWN_SETPGROUP));assert(!posix_spawnattr_setpgroup(&attr,0));
 auto count=std::to_string(iterations);char *argv[]={const_cast<char *>(helper.c_str()),const_cast<char *>("stream"),const_cast<char *>(path.c_str()),count.data(),nullptr};pid_t pid;p.start=tick();int rc=posix_spawn(&pid,helper.c_str(),&actions,&attr,argv,ENVIRON);p.spawned=tick();posix_spawn_file_actions_destroy(&actions);posix_spawnattr_destroy(&attr);close(fds[1]);assert(!rc);
 std::string pending;bool eof=false,failed=false;while(!eof){if(tick()-p.start>30000000000ull){failed=true;break;}pollfd fd={fds[0],POLLIN|POLLHUP,0};int ready=poll(&fd,1,100);if(ready<0){if(errno==EINTR)continue;failed=true;break;}if(!ready)continue;
  char data[4096];auto n=read(fds[0],data,sizeof data);if(n<0){if(errno==EINTR)continue;failed=true;break;}if(!n){eof=true;break;}pending.append(data,(size_t)n);
  for(;;){auto pos=pending.find('\n');if(pos==std::string::npos)break;std::string line=pending.substr(0,pos+1);pending.erase(0,pos+1);auto received=tick();auto j=common_json::parse(line);p.arrivals.push_back({{"seq",j.at("seq")},{"kind",j.at("kind")},{"produced_ns",j.at("produced_ns")},{"received_ns",received}});if(j.at("kind")=="text")p.content+=j.at("text").get<std::string>()+'\n';feed(stream,line);}
  assert(pending.size()<16384);
 }
 if(failed){kill(-pid,SIGKILL);agent_stream_cancel(stream);}close(fds[0]);int status;struct rusage usage={};pid_t got;do{got=wait4(pid,&status,0,&usage);}while(got<0&&errno==EINTR);p.reaped=tick();p.cpu=cpu_ms(usage.ru_utime)+cpu_ms(usage.ru_stime);
 assert(!failed&&got==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==0&&pending.empty()&&p.arrivals.size()==13);return p;
}
static void trial(AgentKvWorker *w,const std::string &root,int words,unsigned long iterations,int mode,int rep,Reference &ref,bool fresh,double cold,double warm){
 auto path=root+"/input.txt",helper=root+"/resident-tool";{std::ofstream f(path);for(int i=0;i<12;i++){for(int j=0;j<words;j++)f<<"reading ";f<<'\n';}}
 std::string prefix="Prior completed task context:\n";for(int i=0;i<1024;i++)prefix+="history ";Output out{&ref,fresh};AgentStreamParams p={};p.system="Summarize the actual immutable measurement units.";p.initial_context=prefix.data();p.initial_bytes=(uint32_t)prefix.size();p.prefill_budget=512;p.defer_until_end=mode==0;p.flush_each_update=mode==2;p.finalize=final;p.final_ctx=&out;AgentStream *s;
 auto initial=tick();OK(agent_stream_open(w,&p,&s));double initial_ms=ms(tick()-initial);auto producer=produce(s,helper,path,iterations);AgentStreamReport report;OK(agent_stream_finish(s,&report));auto returned=tick();assert(report.done&&!report.failed&&!report.cancelled&&out.first_ns);assert(!report.tool_calls&&report.input_evaluated_tokens==report.committed_tokens-report.initial_tokens);
 const char *names[]={"wait_all","stream_coalesced","stream_eager"};auto row=common_json{{"kind","timing"},{"scenario","resident_stream"},{"words",words},{"producer_iterations",iterations},{"mode",names[mode]},{"rep",rep},{"correct",true},{"source_start_ns",producer.start},{"spawn_return_ns",producer.spawned},{"producer_reaped_ns",producer.reaped},{"input_end_ns",report.input_end_ns},{"model_ready_ns",report.model_ready_ns},{"first_token_ns",out.first_ns},{"returned_ns",returned},{"task_first_ms",ms(out.first_ns-producer.start)},{"task_ready_ms",ms(report.model_ready_ns-producer.start)},{"post_input_first_ms",ms(out.first_ns-report.input_end_ns)},{"post_input_ready_ms",ms(report.model_ready_ns-report.input_end_ns)},{"input_ms",ms(report.input_end_ns-producer.start)},{"producer_ms",ms(producer.reaped-producer.start)},{"producer_cpu_ms",producer.cpu},{"prefill_ms",ms(report.prefill_ns)},{"render_ms",ms(report.render_ns)},{"tokenize_ms",ms(report.tokenize_ns)},{"input_evaluated_tokens",report.input_evaluated_tokens},{"input_decode_calls",report.input_decode_calls},{"tokenized_tokens",report.tokenized_tokens},{"tokenizer_calls",report.tokenizer_calls},{"initial_tokens",report.initial_tokens},{"committed_tokens",report.committed_tokens},{"evaluated_frontier",report.evaluated_frontier},{"initial_history_ms",initial_ms},{"oracle_ms",ms(out.oracle_ns)},{"fixture_first_exec_ms",cold},{"fixture_warm_exec_ms",warm},{"arrivals",producer.arrivals},{"content_sha256",hash(producer.content.data(),producer.content.size())},{"canonical_sha256",out.canonical},{"greedy_token",out.token},{"pending_peak",report.pending_peak},{"backpressure",report.backpressure_returns},{"kv_files",0},{"snapshot_operations",0}};
 agent_stream_close(s);auto closed=tick();no_files(root);owner(w,[](cxf_llama *e){cxf_llama_stats s;cxf_llama_get_stats(e,&s);assert(!s.sessions&&!s.logical_tokens&&!s.stored_tokens);return CXF_OK;});
 // Independent oracle after the receiving session's normal close: diagnostic
 // KV must not require a second full simultaneous context in the 8192 pool.
 auto fresh_start=tick();if(fresh)owner(w,[&](cxf_llama *e){cxf_llama_session h;OK(cxf_llama_session_new(e,&h));OK(cxf_llama_append(e,h,1,ref.ids.data(),(uint32_t)ref.ids.size()));uint32_t done;OK(cxf_llama_prefill(e,h,view(e,h).revision,UINT32_MAX,true,&done));int32_t expected;OK(cxf_llama_greedy(e,h,view(e,h).revision,&expected));assert(expected==out.token);ref.token=expected;OK(cxf_llama_session_close(e,h));return CXF_OK;});
 row["closed_ns"]=closed;row["task_complete_wall_ms"]=ms(closed-producer.start);row["task_complete_ms"]=ms(closed-producer.start-out.oracle_ns);row["fresh_reference_ms"]=fresh?ms(tick()-fresh_start):0;emit(row);
}
int main(int argc,char **argv){
 assert(argc==6);bool bench=!strcmp(argv[4],"bench");int repeats=bench?atoi(argv[5]):1;assert(repeats>=1&&repeats<=30);std::string root=std::string(argv[3])+"/resident-source";std::filesystem::create_directory(root);std::filesystem::copy_file(std::string(argv[3])+"/resident-tool",root+"/resident-tool",std::filesystem::copy_options::overwrite_existing);std::ofstream(root+"/settings.ini")<<"limit=0\n";
 TestRunCfg cfg={root.c_str(),"./resident-tool verify",10000};char result[BASIC_OUT_HINT];uint32_t count;auto t=tick();OK(test_run_exec(&cfg,"",0,result,sizeof result,&count));double cold=ms(tick()-t);t=tick();OK(test_run_exec(&cfg,"",0,result,sizeof result,&count));double warm=ms(tick()-t);
 auto params=cxf_llama_default_params();params.model_path=argv[1];params.backend_path=argv[2];params.context_tokens=8192;params.max_sessions=4;params.threads=4;AgentKvWorker *w;OK(agent_kv_worker_open(&params,argv[3],1024ull*1024*1024,&w));emit({{"kind","metadata"},{"context_tokens",8192},{"batch_tokens",512},{"prefill_min_tokens",128},{"threads",4},{"repeats",repeats},{"semantic_units",12},{"source","actual prebuilt local process; CPU work, no sleep"},{"build_during_task",false},{"snapshot_operations",0}});
 for(int words:{80,240})for(unsigned long iterations:{0ul,40000000ul}){Reference ref;for(int rep=0;rep<repeats;rep++)for(int i=0;i<3;i++){int mode=(rep+i)%3;trial(w,root,words,iterations,mode,rep,ref,rep==0&&mode==0,cold,warm);}}
 emit({{"kind","correctness"},{"passed",true},{"fresh_reference_prompts",4},{"canonical_and_greedy",true},{"no_snapshot_or_evict",true},{"producer_completed",true}});agent_kv_worker_close(w);no_files(argv[3]);std::filesystem::remove_all(root);emit({{"kind","closed"},{"passed",true},{"private_kv_files",0}});
}
