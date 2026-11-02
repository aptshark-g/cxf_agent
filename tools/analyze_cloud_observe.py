#!/usr/bin/env python3
"""Offline E62/E63 arithmetic and safe numeric evidence archiving; no API access."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import re
import statistics
import sys
ROOT=Path(__file__).resolve().parents[1]

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def span(values):return dict(median=statistics.median(values),min=min(values),max=max(values))
def cloud_pairs(tasks):
    result=[]
    for case in sorted({t['case'] for t in tasks}):
        for b in sorted({t['block'] for t in tasks if t['case']==case and not t['warmup']}):
            a,c=[next(t for t in tasks if t['case']==case and t['block']==b and t['mode']==m) for m in ('before','after')]
            r={'case':case,'block':b,'both_passed':a['passed'] and c['passed']}
            for f in ('task_ms','api_wait_ms','tool_exec_ms','tool_policy_ms','relay_local_ms','client_residual_ms','outside_provider_tool_ms','client_request_bytes','upstream_request_bytes','api_calls'):
                r[f+'_delta']=c[f]-a[f]
            result.append(r)
    return result

def cloud_summary(tasks):
    pairs=cloud_pairs(tasks);result=[]
    for case in sorted({t['case'] for t in tasks}):
        rows=[r for r in pairs if r['case']==case]
        result.append(dict(case=case,pairs=len(rows),after_slower=sum(r['task_ms_delta']>0 for r in rows),**{k:span([r[k] for r in rows]) for k in rows[0] if k.endswith('_delta')}))
    return result

def serialization_summary(rows):
    return [dict(prompt=p,encoding=e,samples=len(g),request_bytes=g[0]['request_bytes'],serialize_us=span([r['serialize_us'] for r in g]))
            for p in ['before','after'] for e in ['ascii_spaced','ascii_compact','utf8_compact']
            for g in [[r for r in rows if r['prompt']==p and r['encoding']==e and not r['warmup']]]]

def mode_summary(tasks,rounds):
    result=[]
    for case in sorted({t['case'] for t in tasks}):
        for mode in ['before','after']:
            ts=[t for t in tasks if t['case']==case and t['mode']==mode and not t['warmup']]
            rs=[r for r in rounds if r['case']==case and r['mode']==mode and not r['warmup']]
            r={'case':case,'mode':mode,'tasks':len(ts),'rounds':len(rs)}
            for f in ['task_ms','tool_exec_ms','tool_policy_ms','relay_local_ms','client_residual_ms','api_wait_ms']:
                r[f]=span([t[f] for t in ts])
            for f in ['client_request_bytes','upstream_request_bytes']:
                values={t[f] for t in ts};assert len(values)==1;r[f]=next(iter(values))
            for f in ['prompt_tokens','completion_tokens','total_tokens','cached_tokens','prompt_cache_hit_tokens','prompt_cache_miss_tokens']:
                values=[v[f] for v in rs if v[f] is not None]
                r[f+'_present']=len(values);r[f+'_sum']=sum(values) if values else None
            r['reported_cached_fraction']=r['cached_tokens_sum']/r['prompt_tokens_sum'] if r['cached_tokens_present']==r['prompt_tokens_present']==len(rs) and r['prompt_tokens_sum'] else None
            result.append(r)
    return result

def check(data):
    ts,rs=data['tasks'],data['rows']
    assert len(ts)==20 and len(rs)==50 and len(data['tool_events'])==30
    assert cloud_pairs(ts)==data['pairs'] and cloud_summary(ts)==data['summary']
    assert mode_summary(ts,rs)==data['mode_summary']
    for t in ts:
        group=[r for r in rs if all(r[k]==t[k] for k in ['case','block','mode'])]
        assert len(group)==t['api_calls'] and t['passed'] and t['answer_39_marker'] and t['refusal_marker']
        for f in ['api_wait_ms','relay_local_ms','client_request_bytes','upstream_request_bytes']:
            assert abs(sum(r[f] for r in group)-t[f])<1e-6
    for r in rs:
        assert r['http_status']==200 and r['delivered'] and r['error'] is None and not r['usage_issues']
        assert r['prompt_tokens']+r['completion_tokens']==r['total_tokens']
        assert abs(r['api_headers_ms']+r['api_body_ms']-r['api_wait_ms'])<1e-6
        assert abs(r['api_wait_ms']+r['relay_local_ms']-r['relay_ms'])<1e-6
        assert r['prompt_cache_hit_tokens'] is None and r['prompt_cache_miss_tokens'] is None
        assert 0<=r['cached_tokens']<=r['prompt_tokens']
        assert r['tool_exec_ms']>=0 and r['tool_policy_ms']>=0
    for case in ['direct','listed']:
        for block in range(5):
            a,b=[[r for r in rs if r['case']==case and r['block']==block and r['mode']==m] for m in ['before','after']]
            assert len(a)==len(b)
            assert all(x['prompt_tokens']-y['prompt_tokens']==18 for x,y in zip(a,b))
    for name,digest in data['previous_evidence_sha256'].items():assert sha(ROOT/name)==digest
    for name,digest in data['previous_figures_sha256'].items():assert sha(ROOT/name)==digest

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--run',type=Path,required=True);ap.add_argument('--source',type=Path,required=True)
    args=ap.parse_args();run,source=args.run,args.source
    raw=json.loads((run/'summary.json').read_text());old=json.loads((source/'prior-public-hashes.json').read_text())
    tasks=raw['tasks'];events=[]
    for t in tasks:
        directory=run/f"{t['case']}-{t['block']}-{t['mode']}"
        stdout=(directory/'stdout.txt').read_text();t['answer_39_marker']=bool(re.search(r'\b39\b',stdout));t['refusal_marker']=any(s in stdout for s in ['无法修改','不能修改','没有','无权','不可修改','不能进行修改','无修改权限'])
        timings=[json.loads(s[len('[timing] '):]) for s in (directory/'trace.txt').read_text().splitlines() if s.startswith('[timing] ')]
        round_number=0
        for event in timings:
            if event['kind']=='provider':round_number+=1;continue
            assert event['valid']
            events.append(dict(case=t['case'],mode=t['mode'],block=t['block'],warmup=t['warmup'],after_round=round_number,**event))
    for row in raw['rows']:
        matching=[e for e in events if all(e[k]==row[k] for k in ['case','mode','block']) and e['after_round']==row['round']]
        row['tool_exec_ms']=sum(e['exec_ns'] for e in matching)/1e6
        row['tool_policy_ms']=sum(e['before_ns']+e['after_ns'] for e in matching)/1e6
        row['tool_impls']=[e['name'] for e in matching]
    validation={'offline_and_cli':{'passed':True,'suites':3,'capability_combinations':64,'accepted_combinations':48,'existing_mutual_exclusion_refusals':16,'forward_paths':16},'asan_ubsan':{'passed':True,'suites':3,'capability_combinations':64},'default_regression':{'passed':True},'real_cloud':{'passed':raw['passed'],'tasks':20,'api_requests':50},'native_inference':{'rerun':False,'reason':'Only CLI system/trace and test relay changed; historical native timing is not current-version performance.'}}
    base={'schema_version':1,'date':'2026-11-02','hardware':'Apple M5,16GiB unified memory; macOS','runtime':{'platform':platform.platform(),'python':platform.python_version()},
          'previous_evidence_sha256':{n:v for n,v in old.items() if n.startswith('docs/evidence/')},'previous_figures_sha256':{n:v for n,v in old.items() if n.startswith('docs/benchmarks/figures/')},
          'identity':raw['identity'],'measured_source_sha256':json.loads((source/'source-hashes.json').read_text()),'baseline_ctl_sha256':sha(source/'baseline-agentctl.c'),
          'validation':validation}
    a=dict(base,scope='Actual nonstreaming cloud CLI: before/after permission prompt and relay encoding; no direct cloud KV scheduling.',model=raw['model'],thinking=raw['thinking'],max_tokens=raw['max_tokens'],
           rows=raw['rows'],tasks=tasks,tool_events=events,pairs=cloud_pairs(tasks),summary=cloud_summary(tasks),mode_summary=mode_summary(tasks,raw['rows']),
           methodology={'baseline':'Current E59-E61 worktree plus identical timing; old prompt and ASCII spaced relay. Not old Git HEAD.',
             'candidate':'Permission-shaped prompt plus compact UTF-8 relay; tools/permissions/temperature=0/thinking disabled/max_tokens unchanged.',
             'workload':'2 synthetic read-only tasks; each1 warmup+4 evaluated paired blocks, reversed order per block; common immutable file per task preserves exact version receipt.',
             'timing':'CLI task ends before relay shutdown. API wait includes network/queue/inference. Headers are not TTFT. Local relay measured separately; provider overlaps API and relay. Client residual includes C preparation/transport/parse; not exact network-only attribution.',
             'tool_timing':'before/after are outer text-policy checks; exec includes internal file policy and I/O. Tool events are real C guard measurements, associated with preceding model round.',
             'cache':'Only forwarded cached_tokens reported. Dedicated hit/miss fields absent=null, not zero. No reset/forced-cold guarantee. Fraction = sum reported cached /sum prompt tokens, evaluation only.',
             'model_acceptance':'All requested tools and file immutability checked; final answer contains39 and a refusal marker. Text markers are limited checks, not a general capability score.',
             'privacy':'Explicit selected profile read once; actual endpoint/key remain in memory; public evidence contains only synthetic numeric metadata/hashes. Private logs excluded. Selected upstream HTTP, no TLS claim.'},
           failures=[{'phase':'test_development','observed':'Fixture incorrectly expected an invalid JSON shape with HTTP200 to become HTTP error; assertion fixed, real HTTP status retained.'},{'phase':'test_development','observed':'Fixture assumed create/write could coexist; existing16 invalid combinations now checked as refusal,48 legal combinations accepted.'},{'phase':'cloud_evaluation','observed':'4/8 paired tasks slower after. Listed before block1 includes8.531s and7.709s API waits; retained without trimming.'}],
           limits=['4 evaluation pairs per task, not tail/confidence guarantees. Network/cache/model-output drift uncontrolled.','Same observed tool sequence and fixed inputs do not force identical generated text/completion tokens. No causal model-compute speedup established.','Native/cloud SSE/KV scheduling/Windows not measured. C token/prompt compression beyond scoped instruction cleanup is not implemented.'])
    check(a)
    b=dict(base,scope='Offline same synthetic request2 prompts x3 serializers; no API token/cache/latency measurement.',rows=raw['serialization_rows'],summary=serialization_summary(raw['serialization_rows']),
           methodology={'iterations':'10 blocks per cell, first warm;1000 serializations/block; serialize_us is mean per serialization, not total block time.',
                        'input':'One direct first-turn request, only system changed; same parsed object across encodings. ASCII spaced /ASCII compact /UTF8 compact isolate whitespace and escaping.',
                        'limits':'Microseconds under local interpreter; no HTTP/network/model included. Unicode representation does not imply model-token reduction.'},
           limits=['UTF-8 reduces bytes but local serialize CPU may increase; all negative cells retained.','18 input-token reduction is observed in E62 prompt change, not an E63 token result.'])
    for name,data in [('cloud-request-paired-20261102.json',a),('cloud-serialization-20261102.json',b)]:
        p=ROOT/'docs/evidence'/name
        with p.open('x') as f:json.dump(data,f,indent=2,ensure_ascii=False);f.write('\n')
    print('Archived50 cloud rounds +20 task/30 tool auxiliary records and60 serialization blocks; prior62 evidence unchanged')
if __name__=='__main__':main()
