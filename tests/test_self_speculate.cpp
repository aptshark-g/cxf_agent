#include "agent_speculate.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <unistd.h>
using namespace cxf_spec;
static unsigned cases=0;
static void pass(const char *name){cases++;std::printf("{\"kind\":\"unit\",\"case\":\"%s\",\"passed\":true}\n",name);}
static void put(const std::string &path,const std::string &text){std::ofstream f(path);f<<text;assert(f.good());}
int main(){
    char root[]="/tmp/cxf-spec-test-XXXXXX";assert(mkdtemp(root));
    std::string a=std::string(root)+"/a.txt",b=std::string(root)+"/b.txt";
    put(a,"alpha\n");put(b,"beta\n");
    AgentPolicy policy={};BasicFsCfg cfg={root,&policy};Call ca={"fs.read","a.txt"},cb={"fs.read","b.txt"};
    {ReadSpeculation s(cfg);assert(s.predict(ca,1)==CXF_OK);auto r=s.finish(ca,1);assert(r.rc==CXF_OK && r.text.find("alpha")!=std::string::npos);assert(s.report().adopted && !s.report().normal_reads);pass("exact_adopt");}
    {ReadSpeculation s(cfg);assert(s.predict(ca,1)==CXF_OK);auto r=s.finish(cb,1);assert(r.rc==CXF_OK && r.text.find("beta")!=std::string::npos);assert(!s.report().adopted && s.report().fallback);pass("argument_mismatch");}
    {ReadSpeculation s(cfg);assert(s.predict(ca,1)==CXF_OK);auto r=s.finish({"fs.read","a.txt\n1"},1);assert(r.rc==CXF_OK && r.text.find("lpha")!=std::string::npos);assert(!s.report().exact);pass("offset_mismatch");}
    {ReadSpeculation s(cfg);assert(s.predict({"test.run",""},1)!=CXF_OK);auto r=s.finish(ca,1);assert(r.rc==CXF_OK && s.report().fallback && !s.report().started);pass("unknown_prediction");}
    {ReadSpeculation s(cfg);assert(s.predict(ca,1)==CXF_OK);auto r=s.finish({"fs.edit","a.txt"},1);assert(r.rc!=CXF_OK && !s.report().normal_reads && !s.report().adopted);pass("no_write_dispatch");}
    {ReadSpeculation s(cfg);assert(s.predict(ca,1)==CXF_OK);s.wait();put(a,"changed alpha\n");auto r=s.finish(ca,1);assert(r.rc==CXF_OK && r.text.find("changed")!=std::string::npos && s.report().stale && s.report().fallback);pass("changed_file");}
    {ReadSpeculation s(cfg);assert(s.predict(ca,1)==CXF_OK);s.wait();put(b,"replacement\n");assert(!rename(b.c_str(),a.c_str()));auto r=s.finish(ca,1);assert(r.rc==CXF_OK && r.text.find("replacement")!=std::string::npos && s.report().stale);pass("replaced_file");put(b,"beta\n");}
    {ReadSpeculation s(cfg);assert(s.predict(ca,1)==CXF_OK);auto r=s.finish(ca,2);assert(r.rc==CXF_OK && s.report().stale && !s.report().adopted);pass("epoch_changed");}
    {ReadSpeculation s(cfg);assert(s.predict(ca,1)==CXF_OK);s.cancel();assert(s.finish(ca,1).rc!=CXF_OK && s.report().cancelled && !s.report().normal_reads);pass("cancel_drain");}
    {ReadSpeculation s(cfg);assert(s.predict(ca,1)==CXF_OK);assert(s.finish(ca,1).rc==CXF_OK);assert(s.finish(ca,1).rc==CXF_ERR_EMPTY && !s.report().normal_reads);assert(s.predict(ca,1)!=CXF_OK);pass("once_only");}
    {ReadSpeculation s(cfg);assert(s.predict(ca,1)==CXF_OK);assert(s.predict(cb,1)==CXF_ERR_FULL);assert(s.finish(ca,1).rc==CXF_OK);pass("one_candidate_capacity");}
    {ReadSpeculation s(cfg);auto r=s.finish(ca,1);assert(r.rc==CXF_OK && !s.report().started && s.report().normal_reads==1);pass("off_normal");}
    {ReadSpeculation s(cfg);assert(s.predict({"fs.read","missing"},1)==CXF_OK);auto r=s.finish(ca,1);assert(r.rc==CXF_OK && s.report().fallback);pass("read_error_fallback");}
    put(std::string(root)+"/.env","synthetic protected\n");
    {ReadSpeculation s(cfg);Call c={"fs.read",".env"};assert(s.predict(c,1)==CXF_OK);auto r=s.finish(c,1);assert(r.rc!=CXF_OK && r.text.empty() && !s.report().adopted);pass("sensitive_file");}
    {ReadSpeculation s(cfg);Call c={"fs.read","../outside"};assert(s.predict(c,1)==CXF_OK);auto r=s.finish(c,1);assert(r.rc!=CXF_OK && r.text.empty());pass("traversal");}
    assert(!symlink(".env",(std::string(root)+"/alias").c_str()));
    {ReadSpeculation s(cfg);Call c={"fs.read","alias"};assert(s.predict(c,1)==CXF_OK);auto r=s.finish(c,1);assert(r.rc!=CXF_OK && r.text.empty());pass("symlink");}
    {ReadSpeculation s(cfg);assert(s.predict(ca,1)==CXF_OK);s.wait();assert(!unlink(a.c_str()));assert(!symlink(".env",a.c_str()));auto r=s.finish(ca,1);assert(r.rc!=CXF_OK && r.text.empty() && s.report().stale);pass("adoption_policy_recheck");assert(!unlink(a.c_str()));put(a,"alpha\n");}
    AgentPolicy secret={};assert(agent_policy_secret(&secret,"synthetic-spec-private"));BasicFsCfg protected_cfg={root,&secret};put(b,"synthetic-spec-private\n");
    {ReadSpeculation s(protected_cfg);assert(s.predict(cb,1)==CXF_OK);auto r=s.finish(cb,1);assert(r.rc!=CXF_OK && r.text.empty() && !s.report().adopted);pass("sensitive_content");}
    {ReadSpeculation s(cfg);assert(s.predict(ca,1)==CXF_OK);}pass("destructor_drain");
    {assert(!chmod(a.c_str(),0644));ReadSpeculation s(cfg);assert(s.predict(ca,1)==CXF_OK);s.wait();assert(!chmod(a.c_str(),0600));auto r=s.finish(ca,1);assert(r.rc==CXF_OK && s.report().stale && !s.report().adopted);pass("permission_version_changed");assert(!chmod(a.c_str(),0644));}
    {ReadSpeculation s(cfg);Call c={"fs.read","missing"};assert(s.predict(c,1)==CXF_OK);auto r=s.finish(c,1);assert(r.rc!=CXF_OK && r.text.empty() && s.report().fallback && s.report().normal_reads==1);pass("matched_read_failure_falls_back");}
    {ReadSpeculation s(cfg);assert(s.predict(cb,1)==CXF_OK);s.wait();assert(!unlink(b.c_str()));auto r=s.finish(cb,1);assert(r.rc!=CXF_OK && r.text.empty() && s.report().stale);pass("deleted_file");put(b,"beta\n");}
    {ReadSpeculation s(cfg);Call c={"fs.read","a.txt\nnot-an-offset"};assert(s.predict(c,1)==CXF_OK);auto r=s.finish(c,1);assert(r.rc==CXF_ERR_FORMAT && r.text.empty() && !s.report().adopted);pass("malformed_args");}
    {ReadSpeculation s(cfg);Call bad={"fs.read",std::string("a.txt\0tail",10)};assert(s.predict(bad,1)!=CXF_OK);assert(s.finish(bad,1).rc!=CXF_OK);pass("embedded_nul");}
    std::filesystem::remove_all(root);
    std::printf("{\"kind\":\"closed\",\"cases\":%u,\"passed\":true}\n",cases);
}
