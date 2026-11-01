"""Same-workload controls, CPU/decode accounting; no causal subtraction of medians."""
import statistics as st
from analyze_output_reap import stages

def tasks(rows):
    result=stages(rows);steps={}
    for r in rows:
        if r['kind']=='step' and r['phase']=='evaluation':steps.setdefault((r['words'],r['producer_iterations'],r['mode'],r['rep']),[]).append(r)
    tools={}
    for x in rows:
        if x['kind']=='tool' and x['phase']=='evaluation':tools.setdefault((x['words'],x['producer_iterations'],x['mode'],x['rep']),[]).append(x)
    for r in result:
        ts=tools[(r['words'],r['producer_iterations'],r['mode'],r['rep'])]
        r['pretool_ms']=sum((x['tool_start_ns']-x['entered_ns'])/1e6 for x in ts)
        if 'consumer_threads' in ts[0]:
            r['consumer_threads']=sum(x['consumer_threads'] for x in ts)
            r['consumer_launch_ms']=sum(x['consumer_launch_ms'] for x in ts)
        s=steps[(r['words'],r['producer_iterations'],r['mode'],r['rep'])];assert len(s)==4
        for f in ['prepare_ms','prepare_cpu_ms','prepare_thread_cpu_ms','decode_calls','evaluated_tokens','tokenize_ms','render_ms']:
            r['final_'+f]=sum(x[f] for x in s)
        r['final_prefill_ms']=sum(x['prefill_ms'] for x in s)
    return result

def contrasts(rows,pairs):
    ts=tasks(rows);out=[]
    for w,it in sorted({(r['words'],r['producer_iterations']) for r in ts}):
        modes={m:{r['rep']:r for r in ts if (r['words'],r['producer_iterations'],r['mode'])==(w,it,m)} for m in {r['mode'] for r in ts}}
        for a,b in pairs:
            if not modes.get(a) or not modes.get(b):continue
            x,y=modes[a],modes[b];assert x.keys()==y.keys();fields={}
            for f in x[next(iter(x))]:
                if f in ['words','producer_iterations','mode','rep']:continue
                v=[x[k][f]-y[k][f] for k in x];fields[f]=dict(median_saving=st.median(v),min_saving=min(v),max_saving=max(v),positive_pairs=sum(t>0 for t in v))
            out.append(dict(words=w,producer_iterations=it,baseline=a,mode=b,pairs=len(x),**fields))
    return out

if __name__=='__main__':
    import sys,json
    d=json.load(open(sys.argv[1]));pairs=[['off-race','idle-race'],['idle-race','thread-race'],['thread-race','token-race'],['token-race','progress-race'],['off-race','progress-race']]
    print(json.dumps(contrasts(d['rows'],pairs),indent=2))
