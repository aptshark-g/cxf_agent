#!/usr/bin/env python3
"""Offline development audit of archived E17; no model, network, or private config."""
import itertools
import json
import math
from pathlib import Path
import statistics

ROOT=Path(__file__).resolve().parents[1]

def solve(a,b):
    n=len(b);m=[list(row)+[v] for row,v in zip(a,b)]
    for k in range(n):
        p=max(range(k,n),key=lambda i:abs(m[i][k]))
        if abs(m[p][k])<1e-12:return None
        m[k],m[p]=m[p],m[k];v=m[k][k];m[k]=[x/v for x in m[k]]
        for i in range(n):
            if i!=k:
                v=m[i][k];m[i]=[x-v*y for x,y in zip(m[i],m[k])]
    return [r[-1] for r in m]

def fit(xs,ys,weights=None,ridge=0):
    w=weights or [1.0]*len(ys);total=sum(w);d=len(xs[0])
    scale=[math.sqrt(sum(ww*x[j]*x[j] for x,ww in zip(xs,w))/total) or 1 for j in range(d)]
    z=[[v/s for v,s in zip(x,scale)] for x in xs]
    a=[[sum(ww*x[i]*x[j] for x,ww in zip(z,w))+(ridge*total if i==j else 0) for j in range(d)] for i in range(d)]
    b=[sum(ww*x[i]*y for x,y,ww in zip(z,ys,w)) for i in range(d)]
    result=solve(a,b)
    return [v/s for v,s in zip(result,scale)] if result is not None else None

def nnls(xs,ys,weights=None):
    best=[0.0]*len(xs[0]);w=weights or [1.0]*len(ys)
    loss=lambda c:sum(ww*(sum(a*b for a,b in zip(c,x))-y)**2 for x,y,ww in zip(xs,ys,w))
    best_loss=loss(best)
    for size in range(1,len(best)+1):
        for columns in itertools.combinations(range(len(best)),size):
            small=fit([[x[i] for i in columns] for x in xs],ys,w)
            if small is None or min(small)<-1e-8:continue
            c=[0.0]*len(best)
            for i,v in zip(columns,small):c[i]=max(0,v)
            if loss(c)<best_loss:best,best_loss=c,loss(c)
    return best

def features(r,residual=False,vec=False):
    n=r['new_tokens'];b=r['batch_tokens'];calls=(n+b-1)//b
    if not residual:return [r['shape_linear_ms'],r['shape_attention_ms'],calls]
    x=[calls,n/512,r['shape_attention_ms']/100]
    if vec:x.append(r['fa_vec_calls'])
    return x

def summarize(rows,predict):
    errors=[predict(r)-r['wall_ms'] for r in rows]
    return {'mae_ms':statistics.mean(abs(v) for v in errors),'bias_ms':statistics.mean(errors),
            'max_abs_error_ms':max(abs(v) for v in errors)}

def audit():
    data=json.loads((ROOT/'docs/evidence/cost-shapes-v2-20261030.json').read_text());out=[]
    for b in [512,128]:
        train=[r for r in data['rows'] if r.get('scenario')=='calibration' and r['batch_tokens']==b]
        test=[r for r in data['rows'] if r.get('scenario')=='evaluation' and r['batch_tokens']==b]
        old=next(r['shape'] for r in data['rows'] if r['kind']=='fitted_models' and r['batch_tokens']==b)
        xs=[features(r) for r in train];ys=[r['wall_ms'] for r in train];exact=nnls(xs,ys)
        oldloss=sum((sum(a*c for a,c in zip(old,x))-y)**2 for x,y in zip(xs,ys))
        loss=sum((sum(a*c for a,c in zip(exact,x))-y)**2 for x,y in zip(xs,ys))
        candidates=[]
        for name,residual,vec,power in [('nnls_exact',False,False,0),('nnls_weighted',False,False,1),('residual_uniform',True,False,0),('residual_weighted',True,False,1),('residual_relative',True,False,2),('residual_vec',True,True,1)]:
            x=[features(r,residual,vec) for r in train]
            target=[r['wall_ms']-(r['shape_linear_ms']+r['shape_attention_ms'] if residual else 0) for r in train]
            weights=[1/max(r['wall_ms'],20)**power for r in train]
            coef=fit(x,target,weights,0.01) if residual else nnls(x,target,weights)
            def predict(r):return max(0,(r['shape_linear_ms']+r['shape_attention_ms'] if residual else 0)+sum(a*c for a,c in zip(coef,features(r,residual,vec))))
            candidates.append({'mode':name,'coefficient':coef,'train':summarize(train,predict),'evaluation_development_only':summarize(test,predict)})
        # Squared-error contribution grouped by workload size, before fitting.
        shares=[];total=sum((r['shape_linear_ms']+r['shape_attention_ms']-r['wall_ms'])**2 for r in train)
        for n in [32,128,512]:
            shares.append({'new_tokens':n,'raw_sse_share':sum((r['shape_linear_ms']+r['shape_attention_ms']-r['wall_ms'])**2 for r in train if r['new_tokens']==n)/total})
        out.append({'batch_tokens':b,'recorded_coefficient':old,'exact_nnls_coefficient':exact,'recorded_train_sse':oldloss,'exact_train_sse':loss,
                    'max_coefficient_difference':max(abs(a-c) for a,c in zip(old,exact)),'raw_sse_shares':shares,'candidates':candidates})
    return out

if __name__=='__main__':print(json.dumps(audit(),indent=2))
