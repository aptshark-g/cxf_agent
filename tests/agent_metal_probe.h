#ifndef AGENT_METAL_PROBE_H
#define AGENT_METAL_PROBE_H
#ifdef __cplusplus
extern "C" {
#endif
void agent_metal_probe_api(int);
void agent_metal_probe_batch(unsigned,unsigned);
void agent_metal_probe_init(const char *);
void agent_metal_probe_completion(int);
void agent_metal_probe_enable(int);
void agent_metal_probe_begin(int);
void agent_metal_probe_end(void);
#ifdef __cplusplus
}
#endif
#endif
