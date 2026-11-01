"""Decode/sync spans and single-thread producer runnable accounting.
Ready/blocked proxies retain clock-update error; never identify competing PID.
"""
import statistics as st
from analyze_output_contention import tasks as base_tasks

def producer(p):
    assert p['proc_start_rc']==p['proc_end_rc']==0
    scale=p['timebase_numer']/p['timebase_denom']/1e6
    wall=(p['end_ns']-p['begin_ns'])/1e6
    return dict(compute_ms=p['compute_ns']/1e6,output_ms=p['output_ns']/1e6,setup_ms=p['setup_ns']/1e6,
        runnable_ms=p['runnable_ticks']*scale,proc_cpu_ms=p['proc_cpu_ticks']*scale,
        ready_proxy_ms=(p['runnable_ticks']-p['proc_cpu_ticks'])*scale,
        blocked_proxy_ms=wall-p['runnable_ticks']*scale,
        wall_ms=wall,cpu_ms=p['cpu_ns']/1e6,voluntary=p['voluntary'],involuntary=p['involuntary'],instructions=p['instructions'],cycles=p['cycles'])

def tasks(rows):
    ts=base_tasks(rows)
    for t in ts:
        rs=[r for r in rows if r.get('phase')=='evaluation' and (r.get('words'),r.get('producer_iterations'),r.get('mode'),r.get('rep'))==(t['words'],t['producer_iterations'],t['mode'],t['rep'])]
        tools=[r for r in rs if r['kind']=='tool'];steps=[r for r in rs if r['kind']=='step']
        for label,group in [('final',steps),('stream',tools)]:
            for f in ['decode_ms','sync_ms','decode_cpu_ms','sync_cpu_ms','sync_calls']:t[label+'_'+f]=sum(r[f] for r in group)
        for f in producer(tools[0]['producer']):t['producer_'+f]=sum(producer(r['producer'])[f] for r in tools)
    return ts

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

def check(d):
    for r in d['rows']:
        if r['kind'] in ('tool','producer_control'):producer(r['producer'])
        if r['kind'] in ('tool','step'):
            assert r['decode_ms']>=0 and r['sync_ms']>=0
            assert r['decode_ms']+r['sync_ms']<=r['first_ms' if r['kind']=='step' else 'prefill_ms']+.1
            if r['mode'] not in ('off-drain','progress-drain'):assert r['decode_calls']==r['sync_calls']
            else:assert 0<=r['sync_calls']<=r['decode_calls'] and (r['decode_calls']==0 or r['sync_calls']>0)
    assert tasks(d['rows'])==d['decode_tasks']
    assert contrasts(d['rows'],d['contrast_pairs'])==d['decode_contrasts']
