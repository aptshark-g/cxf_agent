"""Metal command intervals, not shader busy counters or an exclusive queue model."""
import statistics as st
from analyze_decode_schedule import tasks as base_tasks

def merged(xs):
 out=[]
 for a,b in sorted((a,b) for a,b in xs if b>a):
  if out and a<=out[-1][1]:out[-1]=(out[-1][0],max(out[-1][1],b))
  else:out.append((a,b))
 return out

def length(xs):return sum(b-a for a,b in merged(xs))*1000

def intersection(xs,ys):
 return merged([(max(a,c),min(b,d)) for a,b in merged(xs) for c,d in merged(ys) if min(b,d)>max(a,c)])

def scope(raw):
 cs=[r for r in raw if r['kind']=='metal_command'];vs=[r for r in cs if r['valid']];ws=[r for r in raw if r['kind']=='metal_wait']
 gpu=merged([(r['gpu_start'],r['gpu_end']) for r in vs]);qw=merged([(r['commit'],r['gpu_start']) for r in vs]);driver=merged([(r['kernel_start'],r['kernel_end']) for r in vs if r['kernel_start']>0])
 same=[]
 for r in vs:
  same+=intersection([(r['commit'],r['gpu_start'])],[(p['gpu_start'],p['gpu_end']) for p in vs if p['queue']==r['queue'] and p['gpu_start']<r['gpu_start']])
 out=dict(buffers=len(cs),valid_buffers=len(vs),gpu_union_ms=length(gpu),queue_union_ms=length(qw),queue_own_gpu_ms=length(intersection(qw,gpu)),queue_same_gpu_ms=length(same),queue_no_own_gpu_ms=length(qw)-length(intersection(qw,gpu)),queue_driver_ms=length(intersection(qw,driver)),gpu_sum_ms=sum((r['gpu_end']-r['gpu_start'])*1000 for r in vs),clock_inversions=sum(r['gpu_start']<r['commit'] for r in vs),driver_buffers=sum(r['kernel_start']>0 and r['kernel_end']>=r['kernel_start'] for r in vs),post_commit_queue_ms=length([(r['commit_end'],r['gpu_start']) for r in vs]))
 for phase,label in [(1,'decode'),(2,'sync'),(0,'other')]:
  subset=[w for w in ws if w['api_phase']==phase];out[label+'_wait_ms']=sum((w['end']-w['begin'])*1000 for w in subset)
  known=gap=wake=late=unknown=0.;maxwake=0.
  for w in subset:
   a,b,e=w['begin'],w['end'],w['gpu_end']
   if e<=0:unknown+=(b-a)*1000
   elif a>=e:late+=(b-a)*1000
   else:
    until=min(b,e);g=length(intersection([(a,until)],gpu));known+=g;gap+=(until-a)*1000-g
    if b>e:wake+=(b-e)*1000;maxwake=max(maxwake,(b-e)*1000)
  out.update({label+'_gpu_overlap_ms':known,label+'_preexec_gap_ms':gap,label+'_return_lag_ms':wake,label+'_late_check_ms':late,label+'_unknown_ms':unknown,label+'_return_max_ms':maxwake})
  assert abs(out[label+'_wait_ms']-known-gap-wake-late-unknown)<1e-5
 cmap={r['cb']:r for r in cs}
 out['notified_buffers']=sum(r.get('notify_registered',False) for r in cs)
 out['notification_ready_buffers']=sum(r.get('notify_registered',False) and r.get('notify_end',0)>0 for r in cs)
 for phase,label in [(1,'decode'),(2,'sync')]:
  delivery=marker=resumption=post=0.;covered=missing=0
  for w in ws:
   if w['api_phase']!=phase:continue
   c=cmap.get(w['cb'],{});n=c.get('notify_begin',0);f=c.get('notify_end',0);e=w['gpu_end'];a=w['begin'];b=w['end']
   if not c.get('notify_registered',False):continue
   if not (e>0 and n>0 and f>=n and f<=b+.000001):missing+=1;continue
   covered+=1;left=max(a,e)
   if b<=left:continue
   post+=(b-left)*1000
   delivery+=length(intersection([(left,b)],[(e,n)]))
   marker+=length(intersection([(left,b)],[(n,f)]))
   resumption+=length(intersection([(left,b)],[(f,b)]))
  out.update({label+'_notify_waits':covered,label+'_notify_missing_waits':missing,label+'_completion_delivery_ms':delivery,label+'_marker_body_ms':marker,label+'_after_notify_return_ms':resumption,label+'_post_gpu_marked_ms':post})
  assert abs(post-delivery-marker-resumption)<.01
 return out

def tasks(d):
 rows=d['rows'];raw=d['metal_commands'];groups={}
 for r in raw:
  if 'scope_id' in r:groups.setdefault(r['scope_id'],[]).append(r)
 ts=base_tasks(rows)
 for t in ts:
  rs=[r for r in rows if r.get('phase')=='evaluation' and (r.get('words'),r.get('producer_iterations'),r.get('mode'),r.get('rep'))==(t['words'],t['producer_iterations'],t['mode'],t['rep']) and r['kind'] in ('step','tool')]
  prof=all('metal' in r for r in rs);t['metal_profiled']=prof
  values=[(r,scope(groups.get(r['metal']['scope_id'],[]))) for r in rs] if prof else []
  if values:
   for f in values[0][1]:t['metal_'+f]=sum(v[f] for _,v in values) if not f.endswith('_max_ms') else max(v[f] for _,v in values)
   for label,kind in [('final','step'),('stream','tool')]:
    for f in values[0][1]:
     t[label+'_metal_'+f]=(max if f.endswith('_max_ms') else sum)(v[f] for r,v in values if r['kind']==kind)
    t[label+'_metal_sync_other_host_ms']=t[label+'_sync_ms']-t[label+'_metal_sync_wait_ms']
 return ts

def contrasts(d,pairs):
 ts=tasks(d);out=[]
 for w,it in sorted({(r['words'],r['producer_iterations']) for r in ts}):
  modes={m:{r['rep']:r for r in ts if (r['words'],r['producer_iterations'],r['mode'])==(w,it,m)} for m in {r['mode'] for r in ts}}
  for a,b in pairs:
   if not modes.get(a) or not modes.get(b):continue
   x,y=modes[a],modes[b];assert x.keys()==y.keys();fields={};first=next(iter(x));keys=set(x[first])&set(y[first])
   for f in sorted(keys-{'words','producer_iterations','mode','rep','metal_profiled'}):
    v=[x[k][f]-y[k][f] for k in x];fields[f]=dict(median_saving=st.median(v),min_saving=min(v),max_saving=max(v),positive_pairs=sum(t>0 for t in v))
   out.append(dict(words=w,producer_iterations=it,baseline=a,mode=b,pairs=len(x),**fields))
 return out

def check(d):
 assert d['metal_tasks']==tasks(d) and d['metal_contrasts']==contrasts(d,d['contrast_pairs'])
 assert all(r['valid'] and r['status']==4 for r in d['metal_commands'] if r['kind']=='metal_command')
 for r in d['rows']:
  if r['kind'] in ('step','tool'):
   if r['mode'].endswith('-bare'):assert 'metal' not in r
   else:assert 'metal' in r and r['metal']['valid_buffers']==r['metal']['buffers']
