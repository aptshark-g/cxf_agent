#include "agent_native_messages.h"
#include <cstdio>
#include <cstring>
#include <new>

bool agent_native_input_utf8(const char *p, uint32_t n) {
    for (uint32_t i=0;i<n;) {
        unsigned char a=(unsigned char)p[i++]; if(a<0x80)continue;
        uint32_t v, extra, minimum;
        if(a>=0xc2 && a<=0xdf){v=a&31;extra=1;minimum=0x80;}
        else if(a>=0xe0 && a<=0xef){v=a&15;extra=2;minimum=0x800;}
        else if(a>=0xf0 && a<=0xf4){v=a&7;extra=3;minimum=0x10000;}
        else return false;
        if(extra>n-i)return false;
        while(extra--){unsigned char b=(unsigned char)p[i++];if((b&0xc0)!=0x80)return false;v=(v<<6)|(b&63);}
        if(v<minimum || v>0x10ffff || (v>=0xd800 && v<=0xdfff))return false;
    }
    return true;
}
struct Sink { common_chat_templates_inputs *input; common_json *normalized; jinja::value_array values; uint64_t bytes=0; };
static jinja::value_string input_string(const char *p,uint32_t n) {
    auto v=jinja::mk_val<jinja::value_string>();
    v->val_str.parts.push_back({true,std::string(p,n)}); // move one owned string; preserve is_input
    return v;
}
static jinja::value_string input_string(const char *p) { return input_string(p,(uint32_t)std::strlen(p)); }
static void content(Sink &s, const char *role, const char *p, uint32_t n, const char *id=nullptr) {
    if(s.values) {
        auto m=jinja::mk_val<jinja::value_object>();
        m->insert("role",input_string(role));m->insert("content",input_string(p,n));
        if(id)m->insert("tool_call_id",input_string(id));
        s.values->push_back(std::move(m));
    } else if(s.input) {
        common_chat_msg m; m.role=role; m.content.assign(p,n); if(id)m.tool_call_id=id;
        s.input->messages.push_back(std::move(m));
    } else {
        // Fill the final array slot: the upstream push_back(common_json_value)
        // otherwise deep-copies an entire temporary message tree.
        common_json &m=(*s.normalized)[s.normalized->size()];
        m["role"]=role;m["content"]=std::string(p,n);
        if(id)m["tool_call_id"]=id;
    }
    s.bytes+=n;
}
static cxf_result message_visit(void *ctx, const AgentConvoView *v) {
    auto &s=*(Sink *)ctx;
    try {
        if(v->kind!=AGENT_CONVO_CALL) {
            if(!agent_native_input_utf8(v->text,v->text_len))return CXF_ERR_FORMAT;
            char id[12]; std::snprintf(id,sizeof id,"c%u",v->call_id);
            content(s,v->kind==AGENT_CONVO_USER?"user":"tool",v->text,v->text_len,
                    v->kind==AGENT_CONVO_TOOL?id:nullptr);
        } else {
            if(!agent_native_input_utf8(v->name,v->name_len)||!agent_native_input_utf8(v->args,v->args_len))return CXF_ERR_FORMAT;
            char id[12]; std::snprintf(id,sizeof id,"c%u",v->call_id);
            if(s.values) {
                auto m=jinja::mk_val<jinja::value_object>();
                m->insert("role",input_string("assistant"));m->insert("content",input_string(""));
                auto calls=jinja::mk_val<jinja::value_array>();auto call=jinja::mk_val<jinja::value_object>();
                call->insert("id",input_string(id));call->insert("type",input_string("function"));
                auto fn=jinja::mk_val<jinja::value_object>();auto args=jinja::mk_val<jinja::value_object>();
                fn->insert("name",input_string(v->name,v->name_len));args->insert("args",input_string(v->args,v->args_len));
                fn->insert("arguments",args);call->insert("function",fn);calls->push_back(std::move(call));
                m->insert("tool_calls",calls);s.values->push_back(std::move(m));
            } else if(s.input) {
                common_chat_msg m; m.role="assistant";
                common_chat_tool_call call; call.name.assign(v->name,v->name_len); call.id=id;
                // Only a single tool argument needs this opaque JSON string on
                // the upstream cold API; no whole-conversation serialization.
                uint32_t escaped=agent_llm_json_esc(v->args,v->args_len,nullptr,0);
                call.arguments="{\"args\":\"";
                size_t prefix=call.arguments.size();call.arguments.resize(prefix+escaped+2);
                agent_llm_json_esc(v->args,v->args_len,&call.arguments[prefix],escaped);
                call.arguments[prefix+escaped]='"';call.arguments[prefix+escaped+1]='}';
                m.tool_calls.push_back(std::move(call)); s.input->messages.push_back(std::move(m));
            } else {
                common_json &m=(*s.normalized)[s.normalized->size()];
                m["role"]="assistant";m["content"]="";
                common_json &call=m["tool_calls"][(size_t)0];
                call["id"]=id;call["type"]="function";
                common_json &fn=call["function"];
                fn["name"]=std::string(v->name,v->name_len);
                fn["arguments"]["args"]=std::string(v->args,v->args_len);
            }
        }
        return CXF_OK;
    } catch(const std::bad_alloc &){return CXF_ERR_NOMEM;
    } catch(...){return CXF_ERR_FORMAT;}
}
static cxf_result collect(const AgentLlmCfg *cfg,const char *log,uint32_t len,
    common_chat_templates_inputs *input,common_json *normalized,jinja::value_array *values,uint64_t *bytes) {
    if(bytes)*bytes=0;
    if(!cfg || (len&&!log) || ((!!input)+(!!normalized)+(!!values)!=1))return CXF_ERR_RANGE;
    try {
        if(input)input->messages.clear();else if(normalized)*normalized=common_json::array();else *values=jinja::mk_val<jinja::value_array>();
        Sink sink={input,normalized,values?*values:nullptr,0};
        uint32_t n=(uint32_t)std::strlen(cfg->system);
        if(!agent_native_input_utf8(cfg->system,n)||!agent_native_input_utf8(cfg->model,(uint32_t)std::strlen(cfg->model)))return CXF_ERR_FORMAT;
        if(n)content(sink,"system",cfg->system,n);
        cxf_result rc=agent_llm_visit_conversation(log,len,message_visit,&sink);
        if(rc!=CXF_OK) {
            if(input)input->messages.clear();else if(normalized)*normalized=common_json::array();else values->reset();
            return rc;
        }
        if(bytes)*bytes=sink.bytes;
        return CXF_OK;
    } catch(const std::bad_alloc &){return CXF_ERR_NOMEM;
    } catch(...){return CXF_ERR_FORMAT;}
}
cxf_result agent_native_collect_messages(const AgentLlmCfg *cfg,const char *log,uint32_t len,
    common_chat_templates_inputs *input,common_json *normalized,uint64_t *bytes) {
    return collect(cfg,log,len,input,normalized,nullptr,bytes);
}
cxf_result agent_native_collect_values(const AgentLlmCfg *cfg,const char *log,uint32_t len,
    jinja::value_array *values,uint64_t *bytes) {
    return collect(cfg,log,len,nullptr,nullptr,values,bytes);
}
