"""Owner decode-shape attribution; not individual shader timings."""
from analyze_metal_timeline import scope

def batches(d):
 owners={r.get('metal',{}).get('scope_id',r.get('scope_id')):(i,r) for i,r in enumerate(d['rows']) if 'metal' in r or r['kind']=='isolation'}
 commands={};waits={}
 for r in d['metal_commands']:
  if r['kind']=='metal_command':commands.setdefault((r['scope_id'],r.get('batch',0)),[]).append(r)
  elif r['kind']=='metal_wait':waits.setdefault((r['scope_id'],r['cb']),[]).append(r)
 out=[]
 for b in d['metal_commands']:
  if b['kind']!='metal_batch':continue
  i,r=owners[b['scope_id']];cs=commands.get((b['scope_id'],b['batch']),[]);ws=[w for c in cs for w in waits.get((b['scope_id'],c['cb']),[])]
  out.append(dict(source_record_index=i,source_kind=r['kind'],**{k:r[k] for k in ['phase','mode','pressure','policy','rep','words','producer_iterations','prefix_tokens','new_tokens'] if k in r},scope_id=b['scope_id'],batch=b['batch'],position=b['position'],tokens=b['tokens'],begin=b['begin'],**scope(cs+ws)))
 return out

def check(d):
 bs=batches(d);sums={}
 for r in bs:sums[r['scope_id']]=sums.get(r['scope_id'],0)+r['tokens']
 ids={(r['scope_id'],r['batch']) for r in bs}
 assert all((r['scope_id'],r.get('batch')) in ids for r in d['metal_commands'] if r['kind']=='metal_command')
 for r in d['rows']:
  if 'metal' in r:assert sums.get(r['metal']['scope_id'],0)==r['evaluated_tokens']
  elif r['kind']=='isolation':assert sums[r['scope_id']]==r['new_tokens']
 return bs
