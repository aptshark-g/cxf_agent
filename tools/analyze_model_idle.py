"""Four-state CPU-supply controls; bracket averages do not remove time drift."""
import statistics as st
from analyze_decode_schedule import producer

def controls(rows,evaluation=True):
 return [dict(words=r['words'],rep=r['rep'],mode=r['mode'],phase=r['phase'],control=r['control'],model_cpu_ms=r['model_cpu_ms'],parent_cpu_ms=r['parent_cpu_ms'],**producer(r['producer'])) for r in rows if r['kind']=='idle_control' and (not evaluation or r['phase']=='evaluation')]
def summary(rows):
 cs=controls(rows);out=[]
 for w,m in sorted({(r['words'],r['mode']) for r in cs}):
  a=[r for r in cs if (r['words'],r['mode'])==(w,m)]
  out.append(dict(words=w,mode=m,samples=len(a),**{f:dict(median=st.median(r[f] for r in a),min=min(r[f] for r in a),max=max(r[f] for r in a)) for f in ['wall_ms','cpu_ms','ready_proxy_ms','blocked_proxy_ms','model_cpu_ms','parent_cpu_ms','involuntary']}))
 return out

def pairs(rows):
 cs=controls(rows);out=[]
 for w in sorted({r['words'] for r in cs}):
  for rep in range(7):
   modes={m:[r for r in cs if (r['words'],r['rep'],r['mode'])==(w,rep,m)] for m in ['absent-before','absent-after','resident-immediate','resident-settled']};assert all(len(x)==3 for x in modes.values())
   for m in ['resident-immediate','resident-settled']:
    fields={}
    for f in ['wall_ms','cpu_ms','ready_proxy_ms','blocked_proxy_ms']:
     base=(sum(r[f] for r in modes['absent-before'])+sum(r[f] for r in modes['absent-after']))/2
     fields[f+'_saving']=base-sum(r[f] for r in modes[m])
    out.append(dict(words=w,rep=rep,mode=m,**fields))
 return out

def check(d):
 rows=d['rows'];assert len(rows)==211 and len(controls(rows,False))==192
 fixtures=[r for r in rows if r['kind']=='model_fixture'];assert len(fixtures)==16 and all(r['correct'] and r['tool_invocations']==3 and r['canonical_greedy_equal'] for r in fixtures)
 assert rows[-1]['no_model_process_left'] and rows[-2]['passed']
 assert d['idle_summary']==summary(rows) and d['idle_pairs']==pairs(rows)
