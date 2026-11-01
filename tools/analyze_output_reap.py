"""Matched process-stage attribution. Positive contrasts mean time saved.
Child end precedes telemetry I/O/exit; tail is NOT pure OS scheduling cost.
"""
import statistics as st
from collections import defaultdict

def stages(rows):
    groups=defaultdict(lambda:defaultdict(float))
    for r in rows:
        if r['kind']!='tool' or r['phase']!='evaluation':continue
        p=r['producer']; key=(r['words'],r['producer_iterations'],r['mode'],r['rep'])
        assert r['tool_start_ns']<=p['begin_ns']<=p['end_ns']<=r['tool_end_ns']
        g=groups[key]
        for name,value in {
            'startup_ms':p['begin_ns']-r['tool_start_ns'],
            'producer_wall_ms':p['end_ns']-p['begin_ns'],
            'producer_cpu_ms':p['cpu_ns'],
            'producer_noncpu_ms':p['end_ns']-p['begin_ns']-p['cpu_ns'],
            'after_producer_ms':r['tool_end_ns']-p['end_ns'],
            'after_eof_ms':max(0,r['reaped_ns']-r['eof_ns']),
        }.items():g[name]+=value/1e6
        for name in ['eof_poll_ms','callback_ms','owner_queue_ms','owner_work_ms','eof_polls']:g[name]+=r[name]
    for r in rows:
        if r['kind']=='task_timing' and r['phase']=='evaluation':
            g=groups[(r['words'],r['producer_iterations'],r['mode'],r['rep'])]
            for f in ['task_ms','tool_sum_ms','post_tool_sum_ms','prefill_sum_ms','join_ms']:g[f]=r[f]
            assert abs(g['startup_ms']+g['producer_wall_ms']+g['after_producer_ms']-g['tool_sum_ms'])<1e-5
    return [dict(words=k[0],producer_iterations=k[1],mode=k[2],rep=k[3],**v) for k,v in sorted(groups.items())]

def analyze(rows,pairs):
    tasks=stages(rows);out=[]
    for words,it in sorted({(r['words'],r['producer_iterations']) for r in tasks}):
        modes={m:{r['rep']:r for r in tasks if (r['words'],r['producer_iterations'],r['mode'])==(words,it,m)} for m in {r['mode'] for r in tasks}}
        for a,b in pairs:
            if not modes.get(a) or not modes.get(b):continue
            x,y=modes[a],modes[b];assert x.keys()==y.keys()
            fields={}
            for f in x[next(iter(x))]:
                if f in ['words','producer_iterations','mode','rep']:continue
                v=[x[k][f]-y[k][f] for k in x]
                fields[f]=dict(median_saving=st.median(v),min_saving=min(v),max_saving=max(v),positive_pairs=sum(t>0 for t in v))
            out.append(dict(words=words,producer_iterations=it,baseline=a,mode=b,pairs=len(x),**fields))
    return out

if __name__=='__main__':
    import json,sys
    d=json.load(open(sys.argv[1]));rows=d['rows'] if isinstance(d,dict) else d
    print(json.dumps(analyze(rows,[('off','on'),('off','progress'),('off','thread'),('on','progress')]),indent=2))
