#include "agent_native.h"
#include "agent_action.h"
#include "agent_speculate.h"
#include "../src/llama_internal.h"
#include "chat.h"
extern "C" {
#include "sha256.h"
}
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <cstring>
using cxf_spec::Call;
using json=common_json;
static uint64_t tick(){return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
static double ms(uint64_t n){return n/1e6;}
static void check(cxf_result rc){if(rc!=CXF_OK)throw std::runtime_error("cxf rc="+std::to_string(rc));}
static std::string hash(const std::string &s){
    unsigned char digest[32];sha256_t ctx;sha256_init(&ctx);sha256_update(&ctx,(const unsigned char *)s.data(),s.size());sha256_final(&ctx,digest);
    static const char hex[]="0123456789abcdef";std::string out;for(auto c:digest){out+=hex[c>>4];out+=hex[c&15];}return out;
}
static cxf_llama_view view(cxf_llama *e,cxf_llama_session s){cxf_llama_view v;check(cxf_llama_session_view(e,s,&v));return v;}
static std::vector<int32_t> tokenize(cxf_llama *e,const std::string &text){
    std::vector<int32_t> tokens(text.size()+32);uint32_t n=0;
    check(cxf_llama_tokenize(e,text.data(),(uint32_t)text.size(),false,tokens.data(),(uint32_t)tokens.size(),&n));tokens.resize(n);return tokens;
}
static void append(cxf_llama *e,cxf_llama_session s,const std::vector<int32_t> &tokens){
    check(cxf_llama_append(e,s,view(e,s).revision,tokens.data(),(uint32_t)tokens.size()));uint32_t done;
    check(cxf_llama_prefill(e,s,view(e,s).revision,UINT32_MAX,true,&done));
}
struct Generation {std::string text;uint32_t tokens=0;cxf_result rc=CXF_OK;bool ended=false;};
static bool step(cxf_llama *e,cxf_llama_session s,Generation &g){
    int32_t token;g.rc=cxf_llama_greedy(e,s,view(e,s).revision,&token);if(g.rc!=CXF_OK)return false;
    if(cxf_llama_is_eog(e,token)){g.ended=true;return false;}
    char piece[256];uint32_t n=0;g.rc=cxf_llama_piece(e,token,piece,sizeof piece,&n);if(g.rc!=CXF_OK)return false;
    if(g.text.size()+n>65536){g.rc=CXF_ERR_FULL;return false;}
    g.text.append(piece,n);g.tokens++;
    try{append(e,s,{token});}catch(...){g.rc=CXF_ERR_IO;return false;}return true;
}
/* Only acceptance calls this injected path. It evaluates real KV, but its
 * scripted generation is never a model prediction or a timing sample. */
static bool replay_step(cxf_llama *e,cxf_llama_session s,Generation &g,const std::vector<int32_t> &tokens){
    if(g.tokens==tokens.size()){g.ended=true;return false;}
    int32_t token=tokens[g.tokens];char piece[256];uint32_t n=0;
    g.rc=cxf_llama_piece(e,token,piece,sizeof piece,&n);if(g.rc!=CXF_OK)return false;
    g.text.append(piece,n);g.tokens++;append(e,s,{token});return true;
}
static bool single(AgentNative *p,const std::string &text,Call &call){
    char bytes[8192];uint32_t n=0;AgentAct act;
    if(agent_native_actions(p,text.data(),(uint32_t)text.size(),bytes,sizeof bytes,&n)!=CXF_OK ||
       !agent_act_decode(bytes,n,&act) || agent_act_bytes(&act)!=n || act.kind!=AGENT_ACT_CALL)return false;
    call.name.assign(act.cap,act.cap_len);call.args.assign(act.args,act.args_len);return true;
}
static const char tools[]=R"([{"type":"function","function":{"name":"fs.read","description":"Read a relative workspace text file. args is path, optionally newline and byte offset.","parameters":{"type":"object","properties":{"args":{"type":"string"}},"required":["args"],"additionalProperties":false}}}])";
static AgentLlmCfg cfg(const AgentPolicy *policy){
    AgentLlmCfg c={};std::strcpy(c.model,"native-self-speculation");
    std::strcpy(c.system,"Use fs.read to read the requested file, then answer using its contents. Do not invent file contents. Make exactly one read call before answering.");
    c.tools=tools;c.tools_len=sizeof tools-1;c.policy=policy;c.scope=AGENT_SCOPE_LOCAL;return c;
}
static void reference(AgentNative *p,AgentLlmCfg &c,const std::string &task,bool thinking){
    auto need=agent_llm_build_request(&c,task.data(),(uint32_t)task.size(),nullptr,0);std::string request(need,'\0');
    agent_llm_build_request(&c,task.data(),(uint32_t)task.size(),request.data(),need);auto j=json::parse(request);
    cxf_llama_session s;auto *e=agent_native_engine(p,&s);auto templates=common_chat_templates_init(cxf_llama_native_model(e),"");
    common_chat_templates_inputs input;input.messages=common_chat_msgs_parse_oaicompat(j["messages"]);input.tools=common_chat_tools_parse_oaicompat(j["tools"]);
    input.enable_thinking=thinking;input.reasoning_format=COMMON_REASONING_FORMAT_DEEPSEEK;
    auto expected=tokenize(e,common_chat_templates_apply(templates.get(),input).prompt);auto v=view(e,s);std::vector<int32_t> actual(v.committed);
    check(cxf_llama_read(e,s,0,actual.data(),(uint32_t)actual.size()));
    if(actual!=expected || v.committed!=v.evaluated)throw std::runtime_error("template/token oracle mismatch");
}
static json run(AgentNative *,AgentLlmCfg &,const BasicFsCfg &,const json &,bool,const json &,bool fixture=false);
static void acceptance(AgentNative *p,AgentLlmCfg &c,const std::string &root){
    std::string task="Read alpha.txt and report its first line.";unsigned count=0;
    auto pass=[&](const char *name){std::cout<<json{{"kind","correctness"},{"case",name},{"passed",true}}.dump()<<'\n';count++;};
    for(bool thinking:{false,true,false,true}){
        check(agent_native_prepare_thinking(p,&c,task.data(),(uint32_t)task.size(),thinking));reference(p,c,task,thinking);
    }pass("thinking_mode_cache_and_upstream_token_oracle");
    cxf_llama_session parent,child;auto *e=agent_native_engine(p,&parent);auto before=view(e,parent);cxf_llama_stats a,b;
    cxf_llama_get_stats(e,&a);int32_t expected,actual;check(cxf_llama_greedy(e,parent,before.revision,&expected));
    check(cxf_llama_fork(e,parent,before.revision,before.committed,&child));cxf_llama_get_stats(e,&b);
    if(a.stored_tokens!=b.stored_tokens || b.sessions!=a.sessions+1 || b.shared_kv_tokens<=a.shared_kv_tokens)throw std::runtime_error("fork sharing oracle");
    pass("fork_shares_canonical_and_evaluated_prefix");
    append(e,child,tokenize(e,"\n</think>\n<tool_call>\n"));Generation g;for(int i=0;i<3;i++)if(!step(e,child,g))break;
    check(cxf_llama_greedy(e,parent,view(e,parent).revision,&actual));if(actual!=expected)throw std::runtime_error("parent logits changed");
    auto after=view(e,parent);if(after.revision!=before.revision || after.committed!=before.committed)throw std::runtime_error("parent history changed");
    pass("probe_keeps_main_history_and_greedy");check(cxf_llama_session_close(e,child));
    check(cxf_llama_greedy(e,parent,view(e,parent).revision,&actual));if(actual!=expected)throw std::runtime_error("child close changed parent");pass("child_close_parent_continues");
    Call call;std::string raw="\n</think>\n<tool_call>\n{\"name\":\"fs.read\",\"arguments\":{\"args\":\"alpha.txt\"}}\n</tool_call>";
    if(!single(p,raw,call) || call.name!="fs.read" || call.args!="alpha.txt" || single(p,raw+raw,call))throw std::runtime_error("single-call parser oracle");
    pass("finished_single_call_parser_and_batch_rejection");check(agent_native_reset(p));cxf_llama_get_stats(e,&b);
    if(b.sessions!=1 || b.stored_tokens!=0)throw std::runtime_error("reset leak");pass("task_reset_reclaims_branch_tokens");
    BasicFsCfg fs={root.c_str(),c.policy};json taskj={{"id","fixture"},{"prompt",task},{"expected_args","alpha.txt"}};
    json config={{"main_tokens",256},{"probe_tokens",128},{"final_tokens",128},{"fork_after",1},{"fixture_probe_args","alpha.txt"}};
    auto off=run(p,c,fs,taskj,false,config,true),on=run(p,c,fs,taskj,true,config,true);
    if(!off["task_ok"].get<bool>() || !on["task_ok"].get<bool>() || !on["adopted"].get<bool>() ||
       off["main_sha256"]!=on["main_sha256"] || off["tool_sha256"]!=on["tool_sha256"] || off["final_sha256"]!=on["final_sha256"])throw std::runtime_error("scripted hit flow oracle");
    pass("scripted_native_full_flow_exact_adoption");config["fixture_probe_args"]="beta.txt";auto miss=run(p,c,fs,taskj,true,config,true);
    if(!miss["task_ok"].get<bool>() || miss["adopted"].get<bool>() || !miss["fallback"].get<bool>() || off["tool_sha256"]!=miss["tool_sha256"])throw std::runtime_error("scripted miss flow oracle");
    pass("scripted_native_full_flow_mismatch_fallback");
    std::cout<<json{{"kind","closed"},{"cases",count},{"passed",true}}.dump()<<std::endl;
}
static json run(AgentNative *p,AgentLlmCfg &c,const BasicFsCfg &fs,const json &task,bool enabled,const json &config,bool fixture){
    auto start=tick();std::string prompt=task.at("prompt");json r={{"kind","model_task"},{"case",task.at("id")},{"mode",enabled?"on":"off"},{"main_ok",false},{"task_ok",false},{"prediction_valid",false},{"tool_name_hit",false},{"exact_hit",false},{"adopted",false},{"probe_rc",(int)CXF_ERR_EMPTY}};
    check(agent_native_reset(p));auto stage=tick();auto rc=agent_native_prepare_thinking(p,&c,prompt.data(),(uint32_t)prompt.size(),true);r["prepare_ms"]=ms(tick()-stage);r["prepare_rc"]=(int)rc;
    cxf_llama_session main;auto *e=agent_native_engine(p,&main);cxf_spec::ReadSpeculation speculation(fs);Generation g,probe,final;
    std::vector<int32_t> main_fixture,probe_fixture,final_fixture;
    if(fixture){
        r["kind"]="functional_flow";r["injected_generation"]=true;
        main_fixture=tokenize(e,"<think>\nRead requested file first.\n</think>\n<tool_call>\n{\"name\":\"fs.read\",\"arguments\":{\"args\":\"alpha.txt\"}}\n</tool_call>");
        probe_fixture=tokenize(e,"{\"name\":\"fs.read\",\"arguments\":{\"args\":\""+config.at("fixture_probe_args").get<std::string>()+"\"}}\n</tool_call>");
        final_fixture=tokenize(e,"The requested file has been read.");
    }
    auto advance=[&](cxf_llama_session s,Generation &out,const std::vector<int32_t> &tokens){return fixture?replay_step(e,s,out,tokens):step(e,s,out);};
    bool resolved=false;
    auto cleanup=[&]{auto t=tick();if(!resolved)speculation.cancel();const auto &s=speculation.report();r["cancelled"]=s.cancelled;r["speculative_reads"]=s.speculative_reads;r["normal_reads"]=s.normal_reads;r["spec_read_ms"]=ms(s.read_ns);check(agent_native_reset(p));r["cleanup_ms"]=ms(tick()-t);r["total_ms"]=ms(tick()-start);};
    if(rc!=CXF_OK){cleanup();return r;}
    cxf_llama_stats stats_before; cxf_llama_get_stats(e,&stats_before);
    bool attempted=false;Call predicted;uint64_t probe_ns=0,issued_ns=0,main_begin=tick();
    uint32_t fork_after=config.value("fork_after",1u),main_limit=config.value("main_tokens",1024u),probe_limit=config.value("probe_tokens",128u);
    while(g.tokens<main_limit && advance(main,g,main_fixture)){
        if(enabled && !attempted && g.tokens>=fork_after){
            attempted=true;auto t=tick();cxf_llama_session child={};bool opened=false;
            try{
                auto v=view(e,main);rc=cxf_llama_fork(e,main,v.revision,v.committed,&child);check(rc);opened=true;
                cxf_llama_stats s;cxf_llama_get_stats(e,&s);r["fork_shared_kv_tokens"]=s.shared_kv_tokens-stats_before.shared_kv_tokens;
                r["fork_stored_tokens"]=s.stored_tokens;r["fork_logical_tokens"]=s.logical_tokens;r["fork_payload_capacity_bytes"]=s.token_payload_capacity;
                probe.text="\n</think>\n<tool_call>\n";append(e,child,tokenize(e,probe.text));
                while(probe.tokens<probe_limit && advance(child,probe,probe_fixture))if(probe.text.find("</tool_call>")!=std::string::npos)break;
                if(probe.rc==CXF_OK && probe.tokens==probe_limit && probe.text.find("</tool_call>")==std::string::npos)probe.rc=CXF_ERR_FULL;
                r["probe_rc"]=(int)probe.rc;
                if(probe.rc==CXF_OK && probe.text.find("</tool_call>")!=std::string::npos && single(p,probe.text,predicted)){
                    r["prediction_valid"]=true;r["predict_start_rc"]=(int)speculation.predict(predicted,1);issued_ns=tick();
                }
            }catch(...){r["probe_rc"]=(int)CXF_ERR_IO;}
            if(opened)check(cxf_llama_session_close(e,child));
            probe_ns+=tick()-t;
        }
    }
    if(g.rc==CXF_OK && !g.ended)g.rc=CXF_ERR_FULL;
    r["main_span_ms"]=ms(tick()-main_begin);r["probe_ms"]=ms(probe_ns);r["main_decode_ms"]=r["main_span_ms"].get<double>()-ms(probe_ns);
    r["main_tokens"]=g.tokens;r["probe_tokens"]=probe.tokens;r["main_sha256"]=hash(g.text);r["main_rc"]=(int)g.rc;r["main_eog"]=g.ended;r["probe_attempted"]=attempted;
    Call actual;bool valid=g.rc==CXF_OK && g.ended && single(p,g.text,actual);r["main_ok"]=valid && actual.name=="fs.read";
    if(!valid || actual.name!="fs.read"){cleanup();return r;}
    r["tool_name_hit"]=r["prediction_valid"].get<bool>() && predicted.name==actual.name;
    r["exact_hit"]=r["tool_name_hit"].get<bool>() && predicted.args==actual.args;r["actual_args_sha256"]=hash(actual.args);
    r["expected_call"]=task.contains("expected_args") && actual.args==task.at("expected_args").get<std::string>();
    auto finish_begin=tick();auto result=speculation.finish(actual,1);resolved=true;auto report=speculation.report();r["post_call_ms"]=ms(tick()-finish_begin);
    r["issued_lead_ms"]=issued_ns?ms(finish_begin-issued_ns):0;
    r["read_finished_before_call"]=report.read_end_ns && report.read_end_ns<=finish_begin;
    r["adopted"]=report.adopted;r["fallback"]=report.fallback;r["tool_rc"]=(int)result.rc;r["tool_sha256"]=hash(result.text);
    r["spec_read_ms"]=ms(report.read_ns);r["normal_read_ms"]=ms(report.normal_ns);r["join_ms"]=ms(report.join_ns);r["validate_ms"]=ms(report.validate_ns);
    r["speculative_reads"]=report.speculative_reads;r["normal_reads"]=report.normal_reads;
    if(result.rc==CXF_OK){
        std::string log=prompt+"\n#call fs.read "+std::to_string(actual.args.size())+"\n"+actual.args+"\n\n#result fs.read "+std::to_string(result.text.size())+"B\n"+result.text+"\n\n\n";
        stage=tick();rc=agent_native_prepare(p,&c,log.data(),(uint32_t)log.size());r["return_prepare_ms"]=ms(tick()-stage);
        stage=tick();if(rc==CXF_OK)while(final.tokens<config.value("final_tokens",256u) && advance(main,final,final_fixture)){}
        if(final.rc==CXF_OK && !final.ended)final.rc=CXF_ERR_FULL;
        r["final_ms"]=ms(tick()-stage);r["final_tokens"]=final.tokens;r["final_rc"]=(int)(rc==CXF_OK?final.rc:rc);r["final_eog"]=final.ended;r["final_sha256"]=hash(final.text);
        char actions[8192];uint32_t n=0;AgentAct act;
        bool done=rc==CXF_OK && final.rc==CXF_OK && final.ended && agent_native_actions(p,final.text.data(),(uint32_t)final.text.size(),actions,sizeof actions,&n)==CXF_OK &&
            agent_act_decode(actions,n,&act) && agent_act_bytes(&act)==n && act.kind==AGENT_ACT_DONE;
        r["task_ok"]=done; // completion, not semantic answer-quality grading
    }
    cxf_llama_stats stats_after;cxf_llama_get_stats(e,&stats_after);r["evaluated_tokens"]=stats_after.evaluated_tokens-stats_before.evaluated_tokens;
    r["logits_replay_tokens"]=stats_after.logits_replay_tokens-stats_before.logits_replay_tokens;
    cleanup();return r;
}
int main(int argc,char **argv){
    if(argc!=4){std::cerr<<"usage: self-speculate-native MODEL BACKEND CONFIG.json\n";return 2;}
    AgentNative *p=nullptr;
    try{
        std::ifstream in(argv[3]);if(!in)throw std::runtime_error("config open failed");
        std::string content((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
        if(content.size()>2*1024*1024)throw std::runtime_error("config too large");auto config=json::parse(content);
        auto params=cxf_llama_default_params();params.model_path=argv[1];params.backend_path=argv[2];params.context_tokens=config.value("context_tokens",8192u);
        params.max_sessions=4;params.threads=config.value("threads",4);params.gpu_layers=config.value("gpu_layers",99);
        auto t=tick();check(agent_native_open(&params,config.value("main_tokens",1024u),&p));
        std::cout<<json{{"kind","metadata"},{"load_ms",ms(tick()-t)},{"context_tokens",params.context_tokens},{"threads",params.threads},{"gpu_layers",params.gpu_layers},{"single_owner",true}}.dump()<<std::endl;
        AgentPolicy policy={};auto c=cfg(&policy);
        if(config.value("acceptance",false)){acceptance(p,c,config.at("root").get<std::string>());agent_native_close(p);return 0;}
        std::string root=config.at("root");BasicFsCfg fs={root.c_str(),&policy};
        uint32_t repeats=config.value("repeats",7u),warmup=config.value("warmup",1u);
        for(uint32_t block=0;block<repeats+warmup;block++)for(const auto &task:config.at("tasks"))for(int order=0;order<2;order++){
            bool enabled=(block+order)%2;auto row=run(p,c,fs,task,enabled,config);row["block"]=block;row["warmup"]=block<warmup;row["order"]=order;
            std::cout<<row.dump()<<std::endl;
        }
        agent_native_close(p);return 0;
    }catch(const std::exception &error){if(p)agent_native_close(p);std::cerr<<"self-speculation failure: "<<error.what()<<'\n';return 1;}
}
