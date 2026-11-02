#ifndef AGENT_SPECULATE_H
#define AGENT_SPECULATE_H
#include <cstdint>
#include <string>
#include <thread>
extern "C" {
#include "agent_basic.h"
}

/* Experimental host controller; no model dependency. Borrowed cfg/root/policy
 * are immutable until finish/cancel/destructor drains the bounded read. */
namespace cxf_spec {
struct Call { std::string name,args; };
struct Result { cxf_result rc=CXF_ERR_EMPTY; std::string text; };
struct Report {
    bool offered=false,started=false,exact=false,adopted=false,fallback=false;
    bool stale=false,cancelled=false;
    uint32_t speculative_reads=0,normal_reads=0;
    uint64_t read_ns=0,join_ns=0,validate_ns=0,normal_ns=0;
    uint64_t read_begin_ns=0,read_end_ns=0;
};
class ReadSpeculation {
public:
    explicit ReadSpeculation(BasicFsCfg cfg):cfg_(cfg) {}
    ~ReadSpeculation();
    ReadSpeculation(const ReadSpeculation &)=delete;
    ReadSpeculation &operator=(const ReadSpeculation &)=delete;
    cxf_result predict(const Call &,uint64_t epoch);
    Result finish(const Call &,uint64_t epoch);
    void wait() { drain(); } /* drain only; does not expose candidate contents */
    void cancel();
    const Report &report() const { return report_; }
private:
    void drain();
    void erase();
    BasicFsCfg cfg_;
    Call candidate_;
    uint64_t epoch_=0;
    std::thread worker_;
    Result candidate_result_;
    BasicReadVersion version_={};
    Report report_;
    bool consumed_=false;
};
}
#endif
