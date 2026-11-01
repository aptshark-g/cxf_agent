"""Offline matched task contrasts; positive means candidate saves time."""
import statistics as st
FIELDS=['task_ms','tool_sum_ms','post_tool_sum_ms','prefill_sum_ms','stream_front_ms','join_ms','wrapper_ms']
def contrasts(rows,pairs):
    groups={}
    for r in rows:
        if r['kind']=='task_timing' and r['phase']=='evaluation':groups.setdefault((r['words'],r['producer_iterations']),{}).setdefault(r['mode'],{})[r['rep']]=r
    out=[]
    for case,modes in sorted(groups.items()):
        for baseline,candidate in pairs:
            if baseline not in modes or candidate not in modes:continue
            a,b=modes[baseline],modes[candidate];assert a.keys()==b.keys()
            fields={}
            for f in FIELDS:
                v=[a[k][f]-b[k][f] for k in sorted(a)]
                fields[f]={'median_saving':st.median(v),'min_saving':min(v),'max_saving':max(v),'positive_pairs':sum(x>0 for x in v)}
            out.append(dict(words=case[0],producer_iterations=case[1],baseline=baseline,mode=candidate,pairs=len(a),**fields))
    return out
