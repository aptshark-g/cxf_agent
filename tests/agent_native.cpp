#include "agent_native.h"
#include "agent_action.h"
#include "native_context_audit.h"
#include "agent_native_messages.h"
#include "../src/llama_internal.h"
#include "chat.h"
#include "chat-auto-parser.h"
extern "C" {
#include "sha256.h"
}
#include <algorithm>
#include <chrono>
#include <cstring>
#include <new>
#include <stdexcept>

struct AgentNative {
    cxf_llama *engine = nullptr;
    bool owns_engine = true;
    const AgentPolicy *policy = nullptr;
    cxf_llama_session session = {};
    common_chat_templates_ptr templates;
    common_chat_parser_params parser;
    std::unique_ptr<common_chat_template> fast_template;
    unsigned char prefix_hash[32] = {};
    unsigned char tools_hash[32] = {};
    AgentNativeStats stats = {};
    uint32_t context = 0, max_generate = 0;
    bool cache_thinking = false;
    bool prepared = false;
    bool cache_enabled = true, cache_ready = false;
    bool output_pending=false;
    std::string output_prefix;
#ifdef CXF_CONTEXT_AUDIT
    NativeContextAuditFn audit = nullptr;
    void *audit_ctx = nullptr;
    NativeContextInputMode input_mode = NATIVE_INPUT_JINJA_VALUES;
#endif
};

static bool optimized_pipeline(AgentNative *p) {
#ifdef CXF_CONTEXT_AUDIT
    return p->input_mode == NATIVE_INPUT_JINJA_VALUES;
#else
    (void)p; return true;
#endif
}

static bool compact_temporaries(AgentNative *p) {
#ifdef CXF_CONTEXT_AUDIT
    return p->input_mode != NATIVE_INPUT_RETAINED_JSON;
#else
    (void)p; return true;
#endif
}
#ifdef CXF_CONTEXT_AUDIT
static uint64_t message_bytes(const common_chat_templates_inputs &input) {
    uint64_t n = 0;
    for (const auto &m : input.messages) {
        n += m.content.size() + m.reasoning_content.size();
    }
    return n;
}
#define CONTEXT_AUDIT(stage_, norm_, render_, scratch_) do { if (p->audit) { \
    NativeContextAuditEvent event = { stage_, request.size(), json.is_null() ? 0 : audit_payload, message_bytes(input), norm_, render_, scratch_ }; \
    p->audit(p->audit_ctx, &event); } } while (0)
extern "C" void agent_native_context_audit(AgentNative *p, NativeContextAuditFn fn, void *ctx, NativeContextInputMode mode) {
    if (p) { p->audit = fn; p->audit_ctx = ctx; p->input_mode = mode; }
}
#else
#define CONTEXT_AUDIT(stage_, norm_, render_, scratch_) ((void)0)
#endif

static constexpr size_t MAX_RENDER = 1024 * 1024;
static const std::string HEADER = "<|im_start|>assistant\n";
static uint64_t tick() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
static void digest(const char *p, size_t n, unsigned char out[32]) {
    sha256_hash(out, reinterpret_cast<const unsigned char *>(p), n);
}
bool agent_native_verified_template(const char *source) {
    /* Exact model-template fixture from the accepted GGUF. Unknown revisions
     * retain upstream preprocessing/analysis rather than guessing equivalence. */
    static const char expected[] = "57f1fd00f0013a2be96aa79b857391f27e23df5b5f847072b524c897e24d0361";
    unsigned char hash[32]; digest(source, std::strlen(source), hash);
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++)
        if (hex[hash[i] >> 4] != expected[2*i] || hex[hash[i] & 15] != expected[2*i+1]) return false;
    return true;
}
static cxf_llama_view view(AgentNative *p) {
    cxf_llama_view v = {};
    if (cxf_llama_session_view(p->engine, p->session, &v) != CXF_OK)
        throw std::runtime_error("invalid native session");
    return v;
}
static std::vector<int32_t> tokenize(AgentNative *p, const char *s, uint32_t n) {
    struct Timer { AgentNative *p; uint64_t start; ~Timer(){p->stats.tokenize_ns+=tick()-start;} } timer{p,tick()};
    uint32_t count = 0;
    std::vector<int32_t> tokens;
    auto run=[&]() {
        p->stats.tokenize_calls++;
        return cxf_llama_tokenize(p->engine,s,n,false,tokens.data(),(uint32_t)tokens.size(),&count);
    };
    auto reserve=[&](uint32_t slots) {
        std::vector<int32_t>().swap(tokens); // FULL output is invalid; release before replacement allocation
        tokens.resize(slots);
        p->stats.tokenize_scratch_peak_bytes=std::max(p->stats.tokenize_scratch_peak_bytes,
                                                     (uint64_t)tokens.capacity()*sizeof(int32_t));
    };
    cxf_result rc;
    if(optimized_pipeline(p)) {
        reserve(n?std::min(p->context,std::max(32u,n/4+32u)):0u);
        rc=run();
        if(count>p->context)throw std::length_error("context");
        if(rc==CXF_ERR_FULL) { p->stats.tokenize_retries++;reserve(count);rc=run(); }
    } else {
        rc=run();
        if(rc!=CXF_OK && rc!=CXF_ERR_FULL)throw std::runtime_error("tokenizer");
        if(count>p->context)throw std::length_error("context");
        reserve(count);rc=run();
    }
    if(rc!=CXF_OK || count>tokens.size())throw std::runtime_error("tokenizer");
    tokens.resize(count);
    p->stats.tokenized_tokens += count;
    return tokens;
}

static size_t output_length(const jinja::value &v,size_t limit) {
    size_t n=0;
    auto add=[&](size_t amount){if(amount>limit-n)throw std::length_error("render limit");n+=amount;};
    if(jinja::is_val<jinja::value_string>(v))for(const auto &p:v->val_str.parts)add(p.val.size());
    else if(jinja::is_val<jinja::value_int>(v)||jinja::is_val<jinja::value_float>(v)||jinja::is_val<jinja::value_bool>(v))add(v->as_string().str().size());
    else if(jinja::is_val<jinja::value_array>(v))for(const auto &x:v->as_array())add(output_length(x,limit-n));
    return n; // match upstream gather: none/object/undefined produce no text
}
static void append_output(const jinja::value &v,std::string &out) {
    if(jinja::is_val<jinja::value_string>(v))for(const auto &p:v->val_str.parts)out.append(p.val);
    else if(jinja::is_val<jinja::value_int>(v)||jinja::is_val<jinja::value_float>(v)||jinja::is_val<jinja::value_bool>(v))out+=v->as_string().str();
    else if(jinja::is_val<jinja::value_array>(v))for(const auto &x:v->as_array())append_output(x,out);
}
std::string agent_native_flatten_values(const jinja::value &v,size_t limit) {
    std::string out;out.reserve(output_length(v,limit));append_output(v,out);return out;
}
std::string agent_native_render_values(const common_chat_template &tmpl,jinja::value_array messages,common_json tools,bool thinking) {
    jinja::context ctx(tmpl.source());
    common_json meta;
    meta["bos_token"]=tmpl.bos_token();meta["eos_token"]=tmpl.eos_token();
    meta["enable_thinking"]=thinking;meta["add_generation_prompt"]=true;
    if(!tools.empty())meta["tools"]=std::move(tools);
    jinja::global_from_json(ctx,meta,true); // preserve upstream input provenance, including tool metadata
    meta=common_json();
    ctx.set_val("messages",messages);messages.reset();
    jinja::runtime runtime(ctx);
    auto result=runtime.execute(tmpl.prog);
    return agent_native_flatten_values(result,MAX_RENDER);
}

static cxf_result native_open(const cxf_llama_params *params, cxf_llama *borrowed,
                             uint32_t context, uint32_t maximum, AgentNative **out) {
    if (!out || (!params && !borrowed) || !maximum || !context) return CXF_ERR_RANGE;
    *out = nullptr;
    AgentNative *p = nullptr;
    try {
        p = new AgentNative;
        p->context = context; p->max_generate = maximum;
        p->owns_engine = !borrowed; p->engine = borrowed;
        cxf_result rc = borrowed ? CXF_OK : cxf_llama_open(params, &p->engine);
        if (rc != CXF_OK) { delete p; return rc; }
        const llama_model *model = cxf_llama_native_model(p->engine);
        char arch[64] = {};
        llama_model_meta_val_str(model, "general.architecture", arch, sizeof arch);
        const char *tmpl = llama_model_chat_template(model, nullptr);
        /* This checkpoint scheme is accepted only for Qwen3 ChatML. */
        if (std::strcmp(arch, "qwen3") || !tmpl || !std::strstr(tmpl, "<|im_start|>") ||
            !std::strstr(tmpl, "<tool_call>") || !std::strstr(tmpl, "enable_thinking")) {
            agent_native_close(p); return CXF_ERR_DEP;
        }
        auto marker = tokenize(p, "<|im_start|>", 12);
        if (marker.size() != 1 || !(llama_vocab_get_attr(llama_model_get_vocab(model), marker[0]) &
            (LLAMA_TOKEN_ATTR_CONTROL | LLAMA_TOKEN_ATTR_USER_DEFINED))) {
            agent_native_close(p); return CXF_ERR_DEP;
        }
        p->templates = common_chat_templates_init(model, "");
        const char *tool_template = llama_model_chat_template(model, "tool_use");
        if (agent_native_verified_template(tmpl) && (!tool_template || !std::strcmp(tmpl, tool_template)))
            p->fast_template.reset(new common_chat_template(tmpl, "", ""));
        rc = cxf_llama_session_new(p->engine, &p->session);
        if (rc != CXF_OK) { agent_native_close(p); return rc; }
        *out = p; return CXF_OK;
    } catch (const std::bad_alloc &) { agent_native_close(p); return CXF_ERR_NOMEM;
    } catch (...) { agent_native_close(p); return CXF_ERR_FORMAT; }
}
extern "C" cxf_result agent_native_open(const cxf_llama_params *params,uint32_t maximum,AgentNative **out) {
    return native_open(params,nullptr,params?params->context_tokens:0,maximum,out);
}
extern "C" cxf_result agent_native_bind(cxf_llama *engine,uint32_t context,uint32_t maximum,AgentNative **out) {
    return native_open(nullptr,engine,context,maximum,out);
}
extern "C" void agent_native_close(AgentNative *p) {
    if (!p) return;
    p->templates.reset();
    if(p->owns_engine)cxf_llama_close(p->engine);
    else if(p->session.owner)(void)cxf_llama_session_close(p->engine,p->session);
    delete p;
}
extern "C" cxf_result agent_native_checkpoint(AgentNative *p,cxf_llama_session *h,uint64_t *revision,uint32_t *tokens) {
    if(!p||!h||!revision||!tokens)return CXF_ERR_RANGE;
    if(!p->stats.checkpoint_tokens)return CXF_ERR_EMPTY;
    try {
        cxf_result rc=cxf_llama_replace(p->engine,p->session,view(p).revision,p->stats.checkpoint_tokens,nullptr,0);
        if(rc!=CXF_OK)return rc;
        p->prepared=false;*h=p->session;*revision=view(p).revision;*tokens=p->stats.checkpoint_tokens;
        return CXF_OK;
    }catch(...){return CXF_ERR_BAD_HANDLE;}
}
extern "C" cxf_result agent_native_reset(AgentNative *p) {
    if (!p) return CXF_ERR_RANGE;
    cxf_result rc = cxf_llama_session_close(p->engine, p->session);
    if (rc != CXF_OK) return rc;
    p->prepared = false;
    p->stats.checkpoint_tokens = p->stats.checkpoint_bytes = 0;
    p->output_pending=false;p->output_prefix.clear();
    return cxf_llama_session_new(p->engine, &p->session);
}
extern "C" cxf_result agent_native_tool_output_begin(AgentNative *p){
    if(!p)return CXF_ERR_RANGE;if(p->output_pending)return CXF_ERR_FULL;
    if(!p->stats.checkpoint_tokens)return CXF_ERR_EMPTY;
    if(!p->fast_template||!p->cache_ready||!agent_native_verified_template(p->fast_template->source().c_str()))return CXF_ERR_DEP;
    try{
        const std::string marker="CXF_TOOL_BODY_MARKER_743bf1";
        std::string local="#call test.run 0\n\n#result test.run "+std::to_string(marker.size())+"B\n"+marker+"\n";
        AgentLlmCfg cfg={};std::strcpy(cfg.model,"native-tool");jinja::value_array values;
        auto rc=agent_native_collect_values(&cfg,local.data(),(uint32_t)local.size(),&values);if(rc!=CXF_OK)return rc;
        auto rendered=agent_native_render_values(*p->fast_template,std::move(values),common_json::array());
        auto at=rendered.find(marker);if(at==std::string::npos||rendered.compare(0,HEADER.size(),HEADER))return CXF_ERR_DEP;
        std::string prefix=rendered.substr(0,at);cxf_llama_session h;uint64_t revision;uint32_t tokens;
        rc=agent_native_checkpoint(p,&h,&revision,&tokens);if(rc!=CXF_OK)return rc;
        p->output_prefix=std::move(prefix);p->output_pending=true;return CXF_OK;
    }catch(const std::bad_alloc &){return CXF_ERR_NOMEM;}catch(...){return CXF_ERR_FORMAT;}
}
extern "C" cxf_result agent_native_tool_output_tokenize(AgentNative *p,const char *text,uint32_t n){
    if(!p||!p->output_pending||n>4096||(n&&!text))return CXF_ERR_RANGE;
    if(!agent_policy_text(p->policy,text,n))return CXF_ERR_RANGE;
    try{auto t=tick();std::string suffix=p->output_prefix;suffix.append(text?text:"",n);auto tokens=tokenize(p,suffix.data(),suffix.size());p->stats.output_tokenized+=tokens.size();p->stats.output_front_ns+=tick()-t;p->stats.output_updates++;return CXF_OK;}catch(...){return CXF_ERR_FORMAT;}
}
extern "C" cxf_result agent_native_tool_output_update(AgentNative *p,const char *text,uint32_t n){
    if(!p||(n&&!text)||n>4096)return CXF_ERR_RANGE;if(!p->output_pending)return CXF_ERR_BAD_HANDLE;
    if(!agent_policy_text(p->policy,text,n))return CXF_ERR_RANGE;
    try{
        // Validate complete UTF8 without assuming stdout read boundaries are BPE boundaries.
        if(!agent_native_input_utf8(text?text:"",n))return CXF_ERR_FORMAT;
        auto start=tick();auto old=view(p);auto base=p->stats.checkpoint_tokens;
        std::string suffix=p->output_prefix;suffix.append(text?text:"",n);auto tokens=tokenize(p,suffix.data(),(uint32_t)suffix.size());
        p->stats.output_tokenized+=tokens.size();if(tokens.size()>p->context-base)return CXF_ERR_FULL;
        if(old.committed<base)return CXF_ERR_BAD_HANDLE;std::vector<int32_t> previous(old.committed-base);
        if(!previous.empty()){auto rc=cxf_llama_read(p->engine,p->session,base,previous.data(),(uint32_t)previous.size());if(rc!=CXF_OK)return rc;}
        uint32_t same=0;while(same<previous.size()&&same<tokens.size()&&previous[same]==tokens[same])same++;
        if(old.evaluated>base+same)p->stats.output_replayed+=old.evaluated-base-same;
        auto rc=cxf_llama_replace(p->engine,p->session,old.revision,base+same,tokens.data()+same,(uint32_t)tokens.size()-same);if(rc!=CXF_OK)return rc;
        p->stats.output_front_ns+=tick()-start;start=tick();uint32_t done=0;
        rc=cxf_llama_prefill(p->engine,p->session,view(p).revision,512,false,&done);
        auto elapsed=tick()-start;p->stats.output_prefill_ns+=elapsed;p->stats.prefill_ns+=elapsed;p->stats.output_evaluated+=done;p->stats.output_updates++;return rc;
    }catch(const std::bad_alloc &){return CXF_ERR_NOMEM;}catch(...){return CXF_ERR_FORMAT;}
}
extern "C" cxf_result agent_native_tool_output_abort(AgentNative *p){
    if(!p)return CXF_ERR_RANGE;if(!p->output_pending)return CXF_OK;
    auto old=view(p);if(old.evaluated>p->stats.checkpoint_tokens)p->stats.output_discarded+=old.evaluated-p->stats.checkpoint_tokens;
    auto rc=cxf_llama_replace(p->engine,p->session,old.revision,p->stats.checkpoint_tokens,nullptr,0);
    if(rc==CXF_OK){p->output_pending=false;p->output_prefix.clear();}return rc;
}
extern "C" void agent_native_template_cache(AgentNative *p, bool enabled) {
    if (p) { p->cache_enabled = enabled; p->cache_ready = false; }
}

extern "C" cxf_result agent_native_prepare(AgentNative *p, const AgentLlmCfg *cfg,
                                             const char *log, uint32_t len) {
    return agent_native_prepare_thinking(p,cfg,log,len,false);
}
extern "C" cxf_result agent_native_prepare_thinking(AgentNative *p,const AgentLlmCfg *cfg,
                                                  const char *log,uint32_t len,bool thinking) {
    if (p) p->prepared = false;
    if (!p || !cfg || (len && !log)) return CXF_ERR_RANGE;
    if (!agent_policy_text(cfg->policy,log,len) || !agent_policy_text(cfg->policy,cfg->system,(uint32_t)std::strlen(cfg->system))) return CXF_ERR_RANGE;
    p->policy=cfg->policy;
    if (len > MAX_RENDER || cfg->tools_len > MAX_RENDER) return CXF_ERR_FULL;
    if (!cfg->model[0]) return CXF_ERR_FULL;
    if (!log) log = "";
    try {
        uint64_t stage = tick();
        common_chat_templates_inputs input;
#ifdef CXF_CONTEXT_AUDIT
        std::string request; common_json json;
        uint64_t audit_payload = 0;
        bool legacy = p->input_mode == NATIVE_INPUT_REQUEST_JSON || p->input_mode == NATIVE_INPUT_RETAINED_JSON;
        if (legacy) {
            uint32_t need = agent_llm_build_request(cfg,log,len,nullptr,0);
            if (!need || need > MAX_RENDER) return CXF_ERR_FULL;
            request.resize(need); agent_llm_build_request(cfg,log,len,&request[0],need);
            CONTEXT_AUDIT("request",0,0,0);
            json = common_json::parse(request);
            input.messages=common_chat_msgs_parse_oaicompat(json.at("messages"));
            if(json.contains("tools"))input.tools=common_chat_tools_parse_oaicompat(json.at("tools"));
            if(p->audit)audit_payload=message_bytes(input);
            CONTEXT_AUDIT("messages",0,0,0);
            if(compact_temporaries(p)){std::string().swap(request);json=common_json();}
        } else
#endif
        {
            if (cfg->tools && cfg->tools_len)
                input.tools=common_chat_tools_parse_oaicompat(common_json::parse(std::string(cfg->tools,cfg->tools_len)));
        }
        input.enable_thinking = thinking;
        input.reasoning_format = COMMON_REASONING_FORMAT_DEEPSEEK;
        auto normalized_tools = common_chat_tools_to_json_oaicompat(input.tools);
        auto tools_text = normalized_tools.dump(); unsigned char tools_hash[32];
        digest(tools_text.data(), tools_text.size(), tools_hash);
        bool cached = p->fast_template && p->cache_enabled && p->cache_ready && p->cache_thinking==thinking &&
                      !std::memcmp(tools_hash, p->tools_hash, sizeof tools_hash);
        common_chat_params rendered;
        if (cached) {
            if(optimized_pipeline(p)) {
                CONTEXT_AUDIT("borrowed",0,0,0);
                jinja::value_array messages;uint64_t payload;
                cxf_result rc=agent_native_collect_values(cfg,log,len,&messages,&payload);
                if(rc!=CXF_OK)return rc;
#ifdef CXF_CONTEXT_AUDIT
                audit_payload=payload;
#endif
                CONTEXT_AUDIT("jinja_values",audit_payload,0,0);
                rendered.prompt=agent_native_render_values(*p->fast_template,std::move(messages),std::move(normalized_tools),thinking);
            } else {
            autoparser::generation_params params;
#ifdef CXF_CONTEXT_AUDIT
            if (legacy) {
                params.messages = common_json::array();
                for (const auto &message : input.messages) params.messages.push_back(message.to_json_oaicompat());
                for (auto &message : params.messages) if (message.contains("tool_calls")) {
                    for (auto &call : message["tool_calls"]) {
                        auto &args=call["function"]["arguments"];
                        if(args.is_string())args=common_json::parse(args.get<std::string>());
                    }
                }
            } else
#endif
            {
                CONTEXT_AUDIT("borrowed",0,0,0);
                uint64_t payload;
                cxf_result rc=agent_native_collect_messages(cfg,log,len,nullptr,&params.messages,&payload);
                if(rc!=CXF_OK)return rc;
#ifdef CXF_CONTEXT_AUDIT
                audit_payload=payload;
#endif
            }
            params.tools = std::move(normalized_tools);
            params.enable_thinking = thinking;
            params.reasoning_format = COMMON_REASONING_FORMAT_DEEPSEEK;
            params.parallel_tool_calls = false; params.add_generation_prompt = true;
            if (compact_temporaries(p)) input.messages.clear();
            CONTEXT_AUDIT("normalized", audit_payload, 0, 0);
            rendered.prompt = common_chat_template_direct_apply(*p->fast_template, params);
            }
            p->stats.template_cache_hits++;
        } else {
#ifdef CXF_CONTEXT_AUDIT
            if (!legacy)
#endif
            {
                CONTEXT_AUDIT("borrowed",0,0,0);
                uint64_t payload;
                cxf_result rc=agent_native_collect_messages(cfg,log,len,&input,nullptr,&payload);
                if(rc!=CXF_OK)return rc;
#ifdef CXF_CONTEXT_AUDIT
                audit_payload=payload;
#endif
                CONTEXT_AUDIT("messages",0,0,0);
            }
            rendered = common_chat_templates_apply(p->templates.get(), input);
            p->stats.template_cache_misses++;
        }
        if (compact_temporaries(p)) input.messages.clear();
        CONTEXT_AUDIT("rendered", 0, rendered.prompt.size(), 0);
        p->stats.render_ns += tick() - stage;
        stage = tick();
        if (!rendered.additional_stops.empty()) return CXF_ERR_DEP;
        if (rendered.prompt.size() > MAX_RENDER) return CXF_ERR_FULL;
        size_t boundary = rendered.prompt.rfind(HEADER);
        if (boundary == std::string::npos || rendered.prompt.substr(boundary) !=
            HEADER + (thinking ? "" : "<think>\n\n</think>\n\n")) return CXF_ERR_DEP;
        uint32_t keep = p->stats.checkpoint_tokens, offset = p->stats.checkpoint_bytes;
        unsigned char hash[32];
        bool reuse = offset && offset <= boundary;
        if (reuse) {
            digest(rendered.prompt.data(), offset, hash);
            reuse = !std::memcmp(hash, p->prefix_hash, sizeof hash);
        }
        if (!reuse) { keep = offset = 0; }
        p->stats.guard_ns += tick() - stage;
        /* The delimiter is an independent special token. Both sides can be
         * tokenized separately without an ordinary-text BPE boundary guess. */
        auto delta = tokenize(p, rendered.prompt.data() + offset, (uint32_t)boundary - offset);
        auto header = tokenize(p, rendered.prompt.data() + boundary,
                               (uint32_t)(rendered.prompt.size() - boundary));
        stage = tick();
        if (delta.size() + header.size() > p->context - keep) return CXF_ERR_FULL;
        uint32_t checkpoint = keep + (uint32_t)delta.size();
        delta.insert(delta.end(), header.begin(), header.end());
        if(p->output_pending){
            auto current=view(p);uint32_t common=0;
            if(reuse&&current.committed>=keep){
                std::vector<int32_t> tail(current.committed-keep);
                if(!tail.empty()){auto read=cxf_llama_read(p->engine,p->session,keep,tail.data(),(uint32_t)tail.size());if(read!=CXF_OK)return read;}
                while(common<tail.size()&&common<delta.size()&&tail[common]==delta[common])common++;
                uint32_t adopted=std::min(current.evaluated,keep+common);
                if(adopted>keep)p->stats.output_adopted+=adopted-keep;
                if(current.evaluated>keep+common)p->stats.output_discarded+=current.evaluated-keep-common;
                keep+=common;delta.erase(delta.begin(),delta.begin()+common);
            }else if(current.evaluated>p->stats.checkpoint_tokens)p->stats.output_discarded+=current.evaluated-p->stats.checkpoint_tokens;
        }
        common_chat_parser_params parser;
        if (!cached) {
            parser = common_chat_parser_params(rendered);
            parser.reasoning_format = COMMON_REASONING_FORMAT_DEEPSEEK;
            if (!rendered.parser.empty()) parser.parser.load(rendered.parser);
        }
        cxf_result rc = cxf_llama_replace(p->engine, p->session, view(p).revision,
                                          keep, delta.data(), (uint32_t)delta.size());
        if (rc != CXF_OK) return rc;
        p->output_pending=false;p->output_prefix.clear();
        CONTEXT_AUDIT("committed", 0, rendered.prompt.size(), (delta.capacity() + header.capacity()) * sizeof(int32_t));
        p->stats.reused_prefix_tokens += keep;
        if (!reuse && p->stats.checkpoint_bytes) p->stats.rebuilds++;
        /* Publish metadata only after canonical replacement succeeds. */
        p->stats.checkpoint_tokens = checkpoint;
        p->stats.checkpoint_bytes = (uint32_t)boundary;
        digest(rendered.prompt.data(), boundary, p->prefix_hash);
        if (!cached) {
            p->parser = std::move(parser);
            p->cache_thinking = thinking;
            std::memcpy(p->tools_hash, tools_hash, sizeof tools_hash); p->cache_ready = true;
        }
        p->stats.commit_ns += tick() - stage;
        stage = tick();
        uint32_t done;
        rc = cxf_llama_prefill(p->engine, p->session, view(p).revision, UINT32_MAX, true, &done);
        p->stats.prefill_ns += tick() - stage;
        if (rc == CXF_OK) p->prepared = true;
        return rc;
    } catch (const std::bad_alloc &) { return CXF_ERR_NOMEM;
    } catch (const std::length_error &) { return CXF_ERR_FULL;
    } catch (...) { return CXF_ERR_FORMAT; }
}

extern "C" cxf_result agent_native_actions(AgentNative *p, const char *text, uint32_t n,
                                             char *out, uint32_t cap, uint32_t *written) {
    if (written) *written = 0;
    if (!p || !p->prepared || !text || !out || !written) return CXF_ERR_RANGE;
    if(!agent_policy_text(p->policy,text,n))return CXF_ERR_RANGE;
    try {
        auto message = common_chat_parse(std::string(text, n), false, p->parser);
        /* Upstream parses leniently. Never turn an unfinished tool envelope
         * into prose/DONE or execute only a valid prefix of a batch. */
        std::string completion(text, n);
        size_t opens = 0, closes = 0;
        for (size_t at = 0; (at = completion.find("<tool_call>", at)) != std::string::npos; at += 11) opens++;
        for (size_t at = 0; (at = completion.find("</tool_call>", at)) != std::string::npos; at += 12) closes++;
        if (opens != closes || opens != message.tool_calls.size()) return CXF_ERR_FORMAT;
        auto msg = message.to_json_oaicompat();
        /* The existing protocol decoder treats a present [] as malformed. */
        if (msg.contains("tool_calls") && msg["tool_calls"].empty()) msg.erase("tool_calls");
        common_json response = {{"choices", common_json::array({{{"message", msg}}})}};
        std::string json = response.dump();
        uint32_t count;
        if (cap <= AGENT_ACT_HEADROOM + 8) return CXF_ERR_FULL;
        cxf_result rc = agent_llm_actions_from_response(json.data(), (uint32_t)json.size(), out, cap, written, &count);
        if(rc==CXF_OK&&!agent_policy_text(p->policy,out,*written)) {
            std::memset(out,0,*written);*written=0;return CXF_ERR_RANGE;
        }
        return rc == CXF_OK && !*written ? CXF_ERR_FORMAT : rc;
    } catch (const std::bad_alloc &) { return CXF_ERR_NOMEM;
    } catch (...) { return CXF_ERR_FORMAT; }
}

extern "C" cxf_result agent_native_chat_action(AgentNative *p, AgentLlmCfg *cfg,
                                                 const char *log, uint32_t len,
                                                 char *raw, uint32_t rawcap,
                                                 char *out, uint32_t cap, uint32_t *written) {
    if (written) *written = 0;
    if (raw && rawcap) raw[0] = '\0';
    if (!p || !cfg || !raw || !rawcap || !out || !written) return CXF_ERR_RANGE;
    cfg->last_status = 0;
    cxf_result rc = agent_native_prepare(p, cfg, log, len);
    try {
        std::string completion;
        bool ended = false;
        for (uint32_t i = 0; rc == CXF_OK && i < p->max_generate; i++) {
            int32_t token;
            rc = cxf_llama_greedy(p->engine, p->session, view(p).revision, &token);
            if (rc != CXF_OK) break;
            if (cxf_llama_is_eog(p->engine, token)) { ended = true; break; }
            char piece[256]; uint32_t bytes;
            rc = cxf_llama_piece(p->engine, token, piece, sizeof piece, &bytes);
            if (rc != CXF_OK) break;
            if (bytes >= rawcap - completion.size()) { rc = CXF_ERR_FULL; break; }
            completion.append(piece, bytes);
            rc = cxf_llama_append(p->engine, p->session, view(p).revision, &token, 1);
            if (rc != CXF_OK) break;
            uint32_t done;
            rc = cxf_llama_prefill(p->engine, p->session, view(p).revision, 1, true, &done);
        }
        std::memcpy(raw, completion.data(), completion.size()); raw[completion.size()] = '\0';
        if (rc == CXF_OK && !ended) rc = CXF_ERR_FULL;
        if (rc == CXF_OK) rc = agent_native_actions(p, completion.data(), (uint32_t)completion.size(), out, cap, written);
        p->stats.generations++;
    } catch (const std::bad_alloc &) { rc = CXF_ERR_NOMEM;
    } catch (...) { rc = CXF_ERR_FORMAT; }
    cfg->last_rc = (int16_t)rc;
    return rc;
}
extern "C" void agent_native_stats(AgentNative *p, AgentNativeStats *out) {
    if (p && out) *out = p->stats;
}
extern "C" cxf_llama *agent_native_engine(AgentNative *p, cxf_llama_session *session) {
    if (!p) return nullptr;
    if (session) *session = p->session;
    return p->engine;
}
