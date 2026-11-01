#ifndef AGENT_RESOURCE_H
#define AGENT_RESOURCE_H
/* Optional sparse diagnostics. Counters are raw operations/pages, not bytes.
 * VM counters belong to the whole machine; gauges may decrease between samples. */
#include <stdint.h>
#include <sys/resource.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif
typedef struct AgentResourceSample {
    uint64_t user_ns,system_ns,minor_faults,major_faults,in_ops,out_ops,voluntary,involuntary;
    uint64_t resident_bytes,footprint_bytes,pageins,pageouts,compressions,decompressions,swapins,swapouts;
    uint32_t valid;
} AgentResourceSample;
static inline AgentResourceSample agent_resource_sample(void){
    AgentResourceSample s={0};struct rusage r;
    if(!getrusage(RUSAGE_SELF,&r)){
        s.valid|=1;s.user_ns=(uint64_t)r.ru_utime.tv_sec*1000000000ull+(uint64_t)r.ru_utime.tv_usec*1000;
        s.system_ns=(uint64_t)r.ru_stime.tv_sec*1000000000ull+(uint64_t)r.ru_stime.tv_usec*1000;
        s.minor_faults=r.ru_minflt;s.major_faults=r.ru_majflt;s.in_ops=r.ru_inblock;s.out_ops=r.ru_oublock;
        s.voluntary=r.ru_nvcsw;s.involuntary=r.ru_nivcsw;
    }
#ifdef __APPLE__
    task_vm_info_data_t t;mach_msg_type_number_t n=TASK_VM_INFO_COUNT;
    if(task_info(mach_task_self(),TASK_VM_INFO,(task_info_t)&t,&n)==KERN_SUCCESS){s.valid|=2;s.resident_bytes=t.resident_size;s.footprint_bytes=t.phys_footprint;}
    vm_statistics64_data_t v;n=HOST_VM_INFO64_COUNT;mach_port_t host=mach_host_self();
    if(host_statistics64(host,HOST_VM_INFO64,(host_info64_t)&v,&n)==KERN_SUCCESS){s.valid|=4;s.pageins=v.pageins;s.pageouts=v.pageouts;s.compressions=v.compressions;s.decompressions=v.decompressions;s.swapins=v.swapins;s.swapouts=v.swapouts;}
    mach_port_deallocate(mach_task_self(),host);
#endif
    return s;
}
#endif
