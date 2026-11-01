#include "agent_native.h"
#include "../src/llama_internal.h"
#include "chat.h"
#include "agent_native_messages.h"
#include <cassert>
#include <cstring>

static void check_provenance(const jinja::value &v) {
    if(jinja::is_val<jinja::value_string>(v)) {
        assert(!v->val_str.parts.empty());for(const auto &p:v->val_str.parts)assert(p.is_input);
    } else if(jinja::is_val<jinja::value_array>(v)) {
        for(const auto &x:v->as_array())check_provenance(x);
    } else if(jinja::is_val<jinja::value_object>(v)) {
        for(const auto &pair:v->as_ordered_object()) {
            for(const auto &part:pair.first->val_str.parts)assert(!part.is_input);
            check_provenance(pair.second);
        }
    }
}

static void compare_message_views(const AgentLlmCfg &cfg,const std::string &log) {
    auto need=agent_llm_build_request(&cfg,log.data(),(uint32_t)log.size(),nullptr,0);
    std::string request(need,'\0');agent_llm_build_request(&cfg,log.data(),(uint32_t)log.size(),&request[0],need);
    auto json=common_json::parse(request);
    auto expected=common_chat_msgs_parse_oaicompat(json["messages"]);
    common_chat_templates_inputs direct;common_json normalized;uint64_t bytes;
    assert(agent_native_collect_messages(&cfg,log.data(),(uint32_t)log.size(),&direct,nullptr,&bytes)==CXF_OK);
    assert(expected==direct.messages);
    assert(agent_native_collect_messages(&cfg,log.data(),(uint32_t)log.size(),nullptr,&normalized,&bytes)==CXF_OK);
    assert(expected==common_chat_msgs_parse_oaicompat(normalized));
    jinja::value_array values;
    assert(agent_native_collect_values(&cfg,log.data(),(uint32_t)log.size(),&values,&bytes)==CXF_OK);
    check_provenance(values);
    assert(expected==common_chat_msgs_parse_oaicompat(common_json::parse(jinja::value_to_json(values))));
}
extern "C" void native_message_views_reference(void) {
    auto tree=jinja::mk_val<jinja::value_array>();auto text=jinja::mk_val<jinja::value_string>();
    text->val_str.parts.push_back({true,std::string("a\0b",3)});text->val_str.parts.push_back({false,"|"});
    tree->push_back(text);auto nested=jinja::mk_val<jinja::value_array>();
    nested->push_back(jinja::mk_val<jinja::value_int>(-17));nested->push_back(jinja::mk_val<jinja::value_float>(1.25));
    nested->push_back(jinja::mk_val<jinja::value_bool>(true));nested->push_back(jinja::mk_val<jinja::value_none>());
    nested->push_back(jinja::mk_val<jinja::value_object>());tree->push_back(nested);
    auto expected_flat=jinja::runtime::gather_string_parts(tree)->as_string().str();
    assert(agent_native_flatten_values(tree,expected_flat.size())==expected_flat);
    assert(text->val_str.parts[0].is_input && !text->val_str.parts[1].is_input);
    bool limited=false;try{agent_native_flatten_values(tree,expected_flat.size()-1);}catch(const std::length_error &){limited=true;}assert(limited);
    AgentLlmCfg cfg={};std::strcpy(cfg.model,"native-qwen3");std::strcpy(cfg.system,"system 中文\n");
    for(const char *text : {"", "plain\n", "#call broken\n", "#result fs.read 0B\n\n",
        "q\n#call fs.read 0\n\n", "q\n#call fs.read 0\n\n#result fs.read 0B\n\n",
        "q\n#call fs.read 0\n\n#fail 1 fs.read io\nleftovers\n#budget: dropped 5B\n"}) compare_message_views(cfg,text);
    uint32_t seed=7319;
    for(int trial=0;trial<256;trial++) {
        std::string log="问题🙂\n";
        for(int j=0;j<3;j++) {
            std::string args="路径 ";std::string body;
            for(int k=0;k<31;k++){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;args.push_back((char)(seed&127));body.push_back((char)((seed>>8)&127));}
            body+="\n#call fake 0\n\n";
            log+="#call fs.read "+std::to_string(args.size())+"\n"+args+"\n#result fs.read "+std::to_string(body.size())+"B\n"+body+"\n";
        }
        if(trial%3==0)log+="#budget: dropped 7B\n";
        if(trial%5==0)log+="#call fs.read 0\n\n";
        compare_message_views(cfg,log);
    }
    for(const auto &bad : {std::string("\xc0\xaf",2),std::string("\xed\xa0\x80",3),std::string("\xf4\x90\x80\x80",4),std::string("\xe4\xb8",2)}) {
        common_chat_templates_inputs out;common_json normalized;
        assert(agent_native_collect_messages(&cfg,bad.data(),(uint32_t)bad.size(),&out,nullptr)==CXF_ERR_FORMAT);
        assert(agent_native_collect_messages(&cfg,bad.data(),(uint32_t)bad.size(),nullptr,&normalized)==CXF_ERR_FORMAT);
        jinja::value_array values;
        assert(agent_native_collect_values(&cfg,bad.data(),(uint32_t)bad.size(),&values)==CXF_ERR_FORMAT);
    }
}

/* Independent full rendering + one-shot tokenization: no checkpoint/token
 * segmentation from the production provider. Test-only transient history. */
static common_chat_params reference_render(AgentNative *p, const AgentLlmCfg *cfg,
                                           const char *log, uint32_t len) {
    auto need = agent_llm_build_request(cfg, log, len, nullptr, 0);
    std::string request(need, '\0');
    agent_llm_build_request(cfg, log, len, &request[0], need);
    auto json = common_json::parse(request);
    auto *engine = agent_native_engine(p, nullptr);
    auto tmpls = common_chat_templates_init(cxf_llama_native_model(engine), "");
    common_chat_templates_inputs input;
    input.messages = common_chat_msgs_parse_oaicompat(json["messages"]);
    if (json.contains("tools")) input.tools = common_chat_tools_parse_oaicompat(json["tools"]);
    input.enable_thinking = false;
    input.reasoning_format = COMMON_REASONING_FORMAT_DEEPSEEK;
    return common_chat_templates_apply(tmpls.get(), input);
}
extern "C" void native_chat_reference(AgentNative *p, const AgentLlmCfg *cfg,
                                       const char *log, uint32_t len) {
    auto text = reference_render(p, cfg, log, len).prompt;
    cxf_llama_session session; auto *engine = agent_native_engine(p, &session);
    cxf_llama_view v;
    assert(cxf_llama_session_view(engine, session, &v) == CXF_OK);
    std::vector<int32_t> tokens(v.committed + 16), actual(v.committed); uint32_t count;
    assert(cxf_llama_tokenize(engine, text.data(), (uint32_t)text.size(), false,
                              tokens.data(), (uint32_t)tokens.size(), &count) == CXF_OK);
    assert(v.committed == count && v.evaluated == count);
    assert(cxf_llama_read(engine, session, 0, actual.data(), count) == CXF_OK);
    assert(!std::memcmp(tokens.data(), actual.data(), count * sizeof(int32_t)));
}

extern "C" void native_actions_reference(AgentNative *p, const AgentLlmCfg *cfg,
    const char *log, uint32_t len, const char *completion, uint32_t bytes) {
    auto rendered = reference_render(p, cfg, log, len);
    common_chat_parser_params parser(rendered);
    parser.reasoning_format = COMMON_REASONING_FORMAT_DEEPSEEK;
    if (!rendered.parser.empty()) parser.parser.load(rendered.parser);
    auto message = common_chat_parse(std::string(completion, bytes), false, parser).to_json_oaicompat();
    if (message.contains("tool_calls") && message["tool_calls"].empty()) message.erase("tool_calls");
    common_json response = {{"choices", common_json::array({{{"message", message}}})}};
    auto json = response.dump();
    char expected[8192], actual[8192]; uint32_t ne, na, count;
    assert(agent_llm_actions_from_response(json.data(), (uint32_t)json.size(), expected, sizeof expected, &ne, &count) == CXF_OK);
    assert(agent_native_actions(p, completion, bytes, actual, sizeof actual, &na) == CXF_OK);
    assert(ne == na && !std::memcmp(expected, actual, na));
}
