#include "agent_speculate.h"
#include <chrono>
#include <algorithm>
#include <cstring>

namespace cxf_spec {
static uint64_t now() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
static bool allowed(const BasicFsCfg &cfg,const Call &call) {
    if(call.name!="fs.read" || call.args.empty() || call.args.size()>1024 ||
       call.args.find('\0')!=std::string::npos || !cfg.root) return false;
    return agent_policy_text(cfg.policy,call.args.data(),(uint32_t)call.args.size());
}
static Result read(const BasicFsCfg &cfg,const Call &call,BasicReadVersion *version) {
    Result result;
    if(!allowed(cfg,call)){result.rc=CXF_ERR_RANGE;return result;}
    char text[BASIC_OUT_HINT]={};uint32_t written=0;
    result.rc=basic_read_versioned(&cfg,call.args.data(),(uint32_t)call.args.size(),text,sizeof text,&written,version);
    if(result.rc==CXF_OK && !agent_policy_text(cfg.policy,text,written))result.rc=CXF_ERR_RANGE;
    if(result.rc==CXF_OK)result.text.assign(text,written);
    std::memset(text,0,sizeof text);
    return result;
}
void ReadSpeculation::drain() {
    auto start=now();
    if(worker_.joinable())worker_.join();
    report_.join_ns+=now()-start;
}
void ReadSpeculation::erase() {
    std::fill(candidate_result_.text.begin(),candidate_result_.text.end(),'\0');
    candidate_result_.text.clear();candidate_result_.rc=CXF_ERR_EMPTY;version_.valid=false;
}
ReadSpeculation::~ReadSpeculation(){drain();erase();}
cxf_result ReadSpeculation::predict(const Call &call,uint64_t epoch) {
    if(consumed_ || report_.offered)return CXF_ERR_FULL;
    report_.offered=true;
    if(!allowed(cfg_,call))return CXF_ERR_RANGE;
    candidate_=call;epoch_=epoch;
    try {
        worker_=std::thread([this]{
            auto start=now();
            report_.read_begin_ns=start;
            try{candidate_result_=read(cfg_,candidate_,&version_);}
            catch(...){candidate_result_.rc=CXF_ERR_NOMEM;}
            report_.read_end_ns=now();report_.read_ns=report_.read_end_ns-start;
        });
        report_.started=true;report_.speculative_reads=1;
    } catch(...) {return CXF_ERR_NOMEM;}
    return CXF_OK;
}
Result ReadSpeculation::finish(const Call &call,uint64_t epoch) {
    if(consumed_)return {};
    consumed_=true;drain();
    report_.exact=report_.started && candidate_.name==call.name && candidate_.args==call.args;
    if(report_.cancelled){erase();return {};}
    if(!allowed(cfg_,call)){erase();return {CXF_ERR_RANGE,{}};}
    if(report_.exact && epoch_==epoch && candidate_result_.rc==CXF_OK) {
        auto start=now();
        bool fresh=basic_read_validate(&cfg_,call.args.data(),(uint32_t)call.args.size(),&version_) &&
            agent_policy_text(cfg_.policy,candidate_result_.text.data(),(uint32_t)candidate_result_.text.size());
        report_.validate_ns=now()-start;
        if(fresh){report_.adopted=true;Result result=std::move(candidate_result_);erase();return result;}
        report_.stale=true;
    }
    if(report_.exact && epoch_!=epoch)report_.stale=true;
    erase();report_.fallback=true;report_.normal_reads=1;
    auto start=now();auto result=read(cfg_,call,nullptr);report_.normal_ns=now()-start;return result;
}
void ReadSpeculation::cancel(){report_.cancelled=true;drain();erase();}
}
