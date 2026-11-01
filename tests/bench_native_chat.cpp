#include "agent_native.h"
#include "../src/llama_internal.h"
#include "chat.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>

using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }
static void ok(cxf_result rc) { if (rc != CXF_OK) { std::fprintf(stderr, "benchmark rc=%d\n", rc); std::abort(); } }
static cxf_llama_view view(cxf_llama *e, cxf_llama_session h) {
    cxf_llama_view v; ok(cxf_llama_session_view(e, h, &v)); return v;
}
static std::vector<int32_t> tokenize(cxf_llama *e, const std::string &s) {
    std::vector<int32_t> t(8192); uint32_t n;
    ok(cxf_llama_tokenize(e, s.data(), (uint32_t)s.size(), false, t.data(), (uint32_t)t.size(), &n));
    t.resize(n); return t;
}
static std::string render(const common_chat_templates *tmpls, AgentLlmCfg &cfg, const std::string &log) {
    uint32_t n = agent_llm_build_request(&cfg, log.data(), (uint32_t)log.size(), nullptr, 0);
    std::string request(n, '\0'); agent_llm_build_request(&cfg, log.data(), (uint32_t)log.size(), &request[0], n);
    auto json = common_json::parse(request);
    common_chat_templates_inputs input;
    input.messages = common_chat_msgs_parse_oaicompat(json.at("messages"));
    input.tools = common_chat_tools_parse_oaicompat(json.at("tools"));
    input.enable_thinking = false; input.reasoning_format = COMMON_REASONING_FORMAT_DEEPSEEK;
    auto rendered = common_chat_templates_apply(tmpls, input);
    /* A real cached provider also needs the output parser. Include its load
     * on both paths rather than charging only the incremental provider. */
    common_chat_parser_params parser(rendered);
    if (!rendered.parser.empty()) parser.parser.load(rendered.parser);
    return rendered.prompt;
}
static void prefill(cxf_llama *e, cxf_llama_session h) {
    uint32_t n; ok(cxf_llama_prefill(e, h, view(e, h).revision, UINT32_MAX, true, &n));
}
struct Result { double wall = 0, front = 0, render = 0, token = 0, guard = 0, commit = 0, prefill = 0; uint64_t tokenized = 0, evaluated = 0, cache_hits = 0, cache_misses = 0; };
static Result baseline_run(cxf_llama *e, cxf_llama_session h, const common_chat_templates *tmpls,
                       AgentLlmCfg &cfg, const std::string &log) {
    Result r; auto start = Clock::now();
    auto text = render(tmpls, cfg, log); r.render = ms(start);
    auto stage = Clock::now(); auto tokens = tokenize(e, text); r.token = ms(stage); r.tokenized = tokens.size();
    stage = Clock::now();
    auto v = view(e, h); std::vector<int32_t> previous(v.committed);
    ok(cxf_llama_read(e, h, 0, previous.data(), v.committed));
    uint32_t keep = 0;
    while (keep < tokens.size() && keep < previous.size() && tokens[keep] == previous[keep]) keep++;
    r.guard = ms(stage); stage = Clock::now();
    ok(cxf_llama_replace(e, h, v.revision, keep, tokens.data() + keep, (uint32_t)tokens.size() - keep));
    r.commit = ms(stage); r.front = ms(start); stage = Clock::now();
    prefill(e, h); r.prefill = ms(stage); r.evaluated = tokens.size() - keep; r.wall = ms(start); return r;
}
static Result baseline(cxf_llama *e, cxf_llama_session h, const common_chat_templates *tmpls,
                       AgentLlmCfg &cfg, const std::string &log) {
    auto start = Clock::now(); auto r = baseline_run(e, h, tmpls, cfg, log);
    r.wall = ms(start); r.front = r.wall - r.prefill; return r;
}
static Result native(AgentNative *p, AgentLlmCfg &cfg, const std::string &log) {
    AgentNativeStats a, b; agent_native_stats(p, &a);
    cxf_llama_stats x, y; auto *e = agent_native_engine(p, nullptr); cxf_llama_get_stats(e, &x);
    auto start = Clock::now(); ok(agent_native_prepare(p, &cfg, log.data(), (uint32_t)log.size()));
    Result r; r.wall = ms(start); agent_native_stats(p, &b); cxf_llama_get_stats(e, &y);
    r.render = (b.render_ns - a.render_ns) / 1e6; r.token = (b.tokenize_ns - a.tokenize_ns) / 1e6;
    r.guard = (b.guard_ns - a.guard_ns) / 1e6; r.commit = (b.commit_ns - a.commit_ns) / 1e6;
    r.prefill = (b.prefill_ns - a.prefill_ns) / 1e6;
    r.front = r.wall - r.prefill; /* includes transient object destruction */
    r.tokenized = b.tokenized_tokens - a.tokenized_tokens; r.evaluated = y.evaluated_tokens - x.evaluated_tokens;
    r.cache_hits = b.template_cache_hits - a.template_cache_hits; r.cache_misses = b.template_cache_misses - a.template_cache_misses;
    return r;
}
static void add(Result &a, const Result &b) {
    a.wall += b.wall; a.front += b.front; a.render += b.render; a.token += b.token;
    a.guard += b.guard; a.commit += b.commit; a.prefill += b.prefill;
    a.tokenized += b.tokenized; a.evaluated += b.evaluated;
    a.cache_hits += b.cache_hits; a.cache_misses += b.cache_misses;
}
static void emit(const char *mode, int rep, int lines, uint32_t prefix, Result r) {
    std::printf("{\"mode\":\"%s\",\"rep\":%d,\"prefix_lines\":%d,\"prefix_tokens\":%u,"
        "\"wall_ms\":%.6f,\"front_ms\":%.6f,\"render_ms\":%.6f,\"tokenize_ms\":%.6f,"
        "\"guard_ms\":%.6f,\"commit_ms\":%.6f,\"prefill_ms\":%.6f,"
        "\"tokenized\":%llu,\"evaluated\":%llu,\"cache_hits\":%llu,\"cache_misses\":%llu,\"correct\":true}\n",
        mode, rep, lines, prefix, r.wall, r.front, r.render, r.token, r.guard, r.commit, r.prefill,
        (unsigned long long)r.tokenized, (unsigned long long)r.evaluated,
        (unsigned long long)r.cache_hits, (unsigned long long)r.cache_misses); std::fflush(stdout);
}
int main(int argc, char **argv) {
    assert(argc == 4); int repeats = std::atoi(argv[3]); assert(repeats > 0 && repeats <= 30);
    cxf_llama_params params = cxf_llama_default_params(); params.model_path = argv[1]; params.backend_path = argv[2];
    params.context_tokens = 8192;
    AgentNative *p; ok(agent_native_open(&params, 16, &p));
    auto *e = agent_native_engine(p, nullptr);
    auto tmpls = common_chat_templates_init(cxf_llama_native_model(e), "");
    AgentLlmCfg cfg = {}; std::strcpy(cfg.model, "native-qwen3"); std::strcpy(cfg.system, "Use tools to read files.");
    AgentToolSpec spec = { "fs.read", "Read a file", "relative path", "file content" }; char tools[4096];
    cfg.tools = tools; cfg.tools_len = agent_llm_render_tools(&spec, 1, tools, sizeof tools);
    auto generated = tokenize(e, "<tool_call>\n{\"name\":\"fs.read\",\"arguments\":{\"args\":\"part.txt\"}}\n</tool_call>");
    for (int lines : {16, 96}) for (int rep = -1; rep < repeats; rep++) for (int variant = 0; variant < 2; variant++) {
        bool cache = (rep + variant + 2) % 2 != 0;
        agent_native_template_cache(p, cache);
        ok(agent_native_reset(p)); cxf_llama_session b, n; ok(cxf_llama_session_new(e, &b)); agent_native_engine(p, &n);
        std::string log = "Read the supplied context and file results.\n";
        for (int i = 0; i < lines; i++) log += "Context item " + std::to_string(i) + ": stable local inference prefix.\n";
        baseline(e, b, tmpls.get(), cfg, log); native(p, cfg, log);
        uint32_t prefix = view(e, n).committed;
        Result br, nr;
        for (int turn = 0; turn < 6; turn++) {
            /* Teacher-forced completion: real KV decode, outside the measured
             * synchronization window. Both paths start from identical tokens. */
            for (auto h : {b, n}) {
                ok(cxf_llama_append(e, h, view(e, h).revision, generated.data(), (uint32_t)generated.size()));
                prefill(e, h);
            }
            std::string body = "#file part.txt\n";
            for (int i = 0; i < 8; i++) body += "result " + std::to_string(turn) + " row " + std::to_string(i) + "\n";
            log += "#call fs.read 8\npart.txt\n#result fs.read " + std::to_string(body.size()) + "B\n" + body + "\n";
            if ((rep + turn + 2) % 2) { add(br, baseline(e, b, tmpls.get(), cfg, log)); add(nr, native(p, cfg, log)); }
            else { add(nr, native(p, cfg, log)); add(br, baseline(e, b, tmpls.get(), cfg, log)); }
            auto bv = view(e, b), nv = view(e, n); assert(bv.committed == nv.committed);
            std::vector<int32_t> bt(bv.committed), nt(nv.committed);
            ok(cxf_llama_read(e, b, 0, bt.data(), bv.committed)); ok(cxf_llama_read(e, n, 0, nt.data(), nv.committed)); assert(bt == nt);
            int32_t x, y; ok(cxf_llama_greedy(e, b, bv.revision, &x)); ok(cxf_llama_greedy(e, n, nv.revision, &y)); assert(x == y);
        }
        if (rep >= 0) {
            emit(cache ? "full_cached_control_1" : "full_cached_control_0", rep, lines, prefix, br);
            emit(cache ? "native_prepare_cache" : "native_checkpoint", rep, lines, prefix, nr);
        }
        ok(cxf_llama_session_close(e, b));
    }
    agent_native_close(p); return 0;
}
