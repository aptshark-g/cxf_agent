#include "agent_native.h"
#include "native_context_audit.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <string>
#include <vector>
#include <mach/mach.h>
#include <malloc/malloc.h>

static void ok(cxf_result rc) { if (rc != CXF_OK) { std::fprintf(stderr, "memory audit rc=%d\n", rc); std::abort(); } }
static uint64_t heap_live() { malloc_statistics_t s = {}; malloc_zone_statistics(nullptr, &s); return s.size_in_use; }
static uint64_t rss() { mach_task_basic_info_data_t s = {}; mach_msg_type_number_t n = MACH_TASK_BASIC_INFO_COUNT;
    return task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&s, &n) == KERN_SUCCESS ? s.resident_size : 0; }
struct Probe {
    struct Point { NativeContextAuditEvent event; uint64_t heap, resident; } points[16];
    uint32_t count = 0;
    std::atomic<bool> active{false}, stop{false}; std::atomic<uint64_t> peak{0};
    std::atomic<const char *> phase{"entry"}, peak_phase{"entry"};
    std::thread sampler;
    void note(uint64_t n) { auto old=peak.load(); while(old<n) if(peak.compare_exchange_weak(old,n)) { peak_phase=phase.load(); break; } }
    Probe() : sampler([this] { while (!stop.load()) { if (active.load()) note(heap_live());
        std::this_thread::sleep_for(std::chrono::microseconds(200)); } }) {}
    void close() { stop = true; if (sampler.joinable()) sampler.join(); }
    ~Probe() { close(); }
    static void sample(void *ctx, const NativeContextAuditEvent *event) {
        auto *p = (Probe *)ctx; p->phase=event->stage; auto h = heap_live(); p->note(h);
        assert(p->count < 16); p->points[p->count++] = {*event, h, rss()};
    }
};
static void *allocate(void *, size_t n, size_t align) { assert(align <= 16); return std::malloc(n); }
static void deallocate(void *, void *p, size_t) { std::free(p); }
static cxf_llama_view view(cxf_llama *e, cxf_llama_session h) { cxf_llama_view v; ok(cxf_llama_session_view(e, h, &v)); return v; }
static cxf_llama_stats stats(cxf_llama *e) { cxf_llama_stats s; cxf_llama_get_stats(e, &s); return s; }
static void finish(cxf_llama *e, cxf_llama_session h) { uint32_t n; ok(cxf_llama_prefill(e, h, view(e,h).revision, UINT32_MAX, true, &n)); }
static std::string fixture(int shape, int size) {
    std::string log="context\n";
    if (!shape) { log.reserve((size_t)size*6+32); for(int i=0;i<size;i++) log+="alpha "; }
    else for(int i=0;i<size;i++) { std::string body(64,'x'); log+="#call fs.read 3\na.c\n#result fs.read 64B\n"+body+"\n"; }
    return log;
}

static void branch_test(AgentNative *p, cxf_alloc &a) {
    ok(agent_native_reset(p)); auto *e = agent_native_engine(p, nullptr);
    cxf_llama_session sessions[8]; ok(cxf_llama_session_new(e, &sessions[0]));
    int32_t seed[16]; uint32_t count; ok(cxf_llama_tokenize(e, "alpha", 5, false, seed, 16, &count)); assert(count);
    { std::vector<int32_t> input(6000, seed[0]); ok(cxf_llama_append(e, sessions[0], 1, input.data(), 6000)); }
    finish(e, sessions[0]); auto before = stats(e); uint64_t heap_before = a.stat.bytes_live;
    // Native provider's empty session also exists: allow eight branches plus it.
    for (int i = 1; i < 8; i++) ok(cxf_llama_fork(e, sessions[0], view(e,sessions[0]).revision, 6000, &sessions[i]));
    auto forked = stats(e);
    assert(forked.stored_tokens == 6000 && forked.logical_tokens == 48000);
    assert(forked.token_payload_capacity == before.token_payload_capacity);
    std::printf("{\"kind\":\"branch_shared\",\"branches\":8,\"stored\":%llu,\"logical\":%llu,\"payload_capacity\":%llu,\"cxf_bytes_before\":%llu,\"cxf_bytes_after\":%llu,\"shared_kv_tokens\":%llu}\n",
        forked.stored_tokens, forked.logical_tokens, forked.token_payload_capacity, heap_before, a.stat.bytes_live, forked.shared_kv_tokens);
    std::vector<int32_t> suffix(32, seed[0]);
    for (auto h : sessions) { ok(cxf_llama_append(e,h,view(e,h).revision,suffix.data(),32)); finish(e,h); }
    auto after = stats(e); assert(after.stored_tokens == 6256 && after.logical_tokens == 48256);
    assert(after.evaluated_tokens - forked.evaluated_tokens == 256);
    std::printf("{\"kind\":\"branch_suffix\",\"stored\":%llu,\"logical\":%llu,\"payload_capacity\":%llu,\"new_decode_tokens\":%llu,\"cxf_bytes\":%llu}\n",
        after.stored_tokens, after.logical_tokens, after.token_payload_capacity,
        after.evaluated_tokens - forked.evaluated_tokens, a.stat.bytes_live);
    for (auto h : sessions) ok(cxf_llama_session_close(e,h));
    after = stats(e); assert(!after.stored_tokens && !after.token_payload_capacity);
}
int main(int argc, char **argv) {
    assert(argc == 4); cxf_alloc allocator = { allocate, deallocate, nullptr, {} };
    cxf_llama_params params = cxf_llama_default_params(); params.model_path = argv[1]; params.backend_path = argv[2];
    params.context_tokens = (uint32_t)std::atoi(argv[3]); assert(params.context_tokens>=8192 && params.context_tokens<=16384);
    params.max_sessions = 9; params.alloc = &allocator;
    AgentNative *p; ok(agent_native_open(&params, 16, &p));
    auto *engine = agent_native_engine(p, nullptr);
    AgentLlmCfg cfg = {}; std::strcpy(cfg.model,"native-qwen3"); std::strcpy(cfg.system,"Use tools.");
    AgentToolSpec spec = {"fs.read","Read file","path","contents"}; char tools[4096];
    cfg.tools = tools; cfg.tools_len = agent_llm_render_tools(&spec,1,tools,sizeof tools);
    {
        Probe probe;
        for (int shape = 0; shape < 2; shape++) for (int size : (shape == 0 ? std::vector<int>{1024,4096,7168,14336,16384,65536} : std::vector<int>{16,128,512})) {
            std::string log=fixture(shape,size);
            agent_native_context_audit(p,nullptr,nullptr,NATIVE_INPUT_JINJA_VALUES);
            ok(agent_native_reset(p));
            // Warm caches and populate canonical tokens when this fits the KV limit.
            cxf_result expected = agent_native_prepare(p,&cfg,log.data(),(uint32_t)log.size());
            assert(expected == CXF_OK || expected == CXF_ERR_FULL);
            for (int rep=0;rep<3;rep++) for(int variant=0;variant<2;variant++) {
                bool legacy = (rep+variant)%2 != 0;
                agent_native_context_audit(p,Probe::sample,&probe,legacy?NATIVE_INPUT_DIRECT:NATIVE_INPUT_JINJA_VALUES);
                probe.count=0; uint64_t before=heap_live(); probe.peak=before; probe.phase="entry"; probe.peak_phase="entry"; probe.active=true;
                AgentNativeStats a,b;agent_native_stats(p,&a);
                cxf_result rc=agent_native_prepare(p,&cfg,log.data(),(uint32_t)log.size());agent_native_stats(p,&b);
                probe.active=false; auto heap_after=heap_live(); assert(rc==expected);
                auto s=stats(engine);
                std::printf("{\"kind\":\"memory\",\"shape\":\"%s\",\"size\":%d,\"rep\":%d,\"mode\":\"%s\",\"input_bytes\":%zu,\"rc\":%d,\"heap_before\":%llu,\"heap_after\":%llu,\"sampled_heap_peak\":%llu,\"peak_since_probe\":\"%s\",\"rss_after\":%llu,\"stored\":%llu,\"logical\":%llu,\"payload_capacity\":%llu,\"cxf_bytes\":%llu,\"tokenizer_calls\":%llu,\"tokenizer_retries\":%llu,\"points\":[",
                    shape?"tool_turns":"single_text",size,rep,legacy?"normalized_json":"jinja_values",log.size(),rc,
                    before,heap_after,probe.peak.load(),probe.peak_phase.load(),rss(),s.stored_tokens,s.logical_tokens,s.token_payload_capacity,allocator.stat.bytes_live,
                    b.tokenize_calls-a.tokenize_calls,b.tokenize_retries-a.tokenize_retries);
                for(uint32_t j=0;j<probe.count;j++){auto &q=probe.points[j]; auto &x=q.event;
                    std::printf("%s{\"stage\":\"%s\",\"heap\":%llu,\"request\":%llu,\"json_strings\":%llu,\"messages\":%llu,\"normalized\":%llu,\"rendered\":%llu,\"token_scratch\":%llu}",j?",":"",x.stage,q.heap,x.request,x.json_strings,x.messages,x.normalized,x.rendered,x.token_scratch);}
                std::printf("]}\n"); std::fflush(stdout);
            }
        }
        agent_native_context_audit(p,nullptr,nullptr,NATIVE_INPUT_JINJA_VALUES); probe.close();
    }
    // No sampling thread or payload-counting hook in these timing samples.
    for (int shape=0;shape<2;shape++) for(int size : (shape==0 ? std::vector<int>{1024,4096,7168,14336,65536} : std::vector<int>{128,512})) {
        std::string log=fixture(shape,size);
        agent_native_context_audit(p,nullptr,nullptr,NATIVE_INPUT_JINJA_VALUES);
        ok(agent_native_reset(p)); auto expected=agent_native_prepare(p,&cfg,log.data(),(uint32_t)log.size());
        for(int rep=0;rep<3;rep++)for(int variant=0;variant<2;variant++) {
            bool legacy=(rep+variant)%2!=0; agent_native_context_audit(p,nullptr,nullptr,legacy?NATIVE_INPUT_DIRECT:NATIVE_INPUT_JINJA_VALUES);
            AgentNativeStats a,b;agent_native_stats(p,&a);
            auto start=std::chrono::steady_clock::now(); auto rc=agent_native_prepare(p,&cfg,log.data(),(uint32_t)log.size());
            double elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count(); assert(rc==expected);
            agent_native_stats(p,&b);
            std::printf("{\"kind\":\"timing\",\"shape\":\"%s\",\"size\":%d,\"rep\":%d,\"mode\":\"%s\",\"rc\":%d,\"wall_ms\":%.6f,\"render_ms\":%.6f,\"tokenize_ms\":%.6f,\"tokenizer_calls\":%llu,\"tokenizer_retries\":%llu}\n",shape?"tool_turns":"single_text",size,rep,legacy?"normalized_json":"jinja_values",rc,elapsed,
                (b.render_ns-a.render_ns)/1e6,(b.tokenize_ns-a.tokenize_ns)/1e6,b.tokenize_calls-a.tokenize_calls,b.tokenize_retries-a.tokenize_retries);
        }
    }
    agent_native_context_audit(p,nullptr,nullptr,NATIVE_INPUT_JINJA_VALUES); branch_test(p,allocator);
    agent_native_close(p); assert(!allocator.stat.bytes_live);
    std::puts("{\"kind\":\"closed\",\"cxf_live_bytes\":0}");
}
