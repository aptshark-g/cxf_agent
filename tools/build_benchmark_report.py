#!/usr/bin/env python3
"""Build/check the offline benchmark publication from immutable evidence; no model needed.

Standard-library only. Figures are rendered separately with ReportLab.
Never reads local model configuration, credentials, request traces or engine logs.
"""
import argparse
import collections
import csv
import hashlib
import io
import json
import math
from pathlib import Path
import re
import statistics
from urllib.parse import unquote
import zipfile

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'docs/benchmarks'
SPECS = [
    ('E01', 'llama-context-direct', 'Direct libllama: prefix, stream, fork, state', 'raw', 61, 'llama-context-final-20261030'),
    ('E02', 'llama-context-slow-stream', 'Direct libllama: slow token arrivals', 'raw_source_unrecorded', 13, 'llama-context-slow-stream-20261030'),
    ('E03', 'llama-context-v1', 'Native context correctness', 'functional', 0, 'llama-native-v1-final-20261030'),
    ('E04', 'llama-chat-v1', 'Native chat, tools, CLI and model probes', 'functional', 0, 'native-chat-final2-20261030'),
    ('E05', 'llama-chat-compat', 'Chat checkpoint compatibility', 'raw', 20, 'native-chat-compat-final2-20261030'),
    ('E06', 'llama-template-cache', 'Template preparation cache', 'raw', 40, 'native-template-cache-final-20261030'),
    ('E07', 'context-memory', 'Temporary lifetimes and shared prefix audit', 'raw', 99, 'context-memory-final-20261030'),
    ('E08', 'direct-messages', 'Borrowed messages versus request JSON', 'raw', 99, 'direct-messages-final2-20261030'),
    ('E09', 'jinja-values', 'Direct Jinja values and bounded tokenizer', 'raw', 99, 'jinja-values-final-20261030'),
    ('E10', 'kv-worker', 'Asynchronous KV restoration', 'raw', 92, 'kv-worker-final2-20261030'),
    ('E11', 'stream-pipeline', 'Streaming prefill and complete-file tools', 'raw', 191, 'stream-final3-20261030'),
    ('E12', 'basic-agent-probes', 'Historical autonomous repair failures', 'sanitized_summary', 0, ''),
    ('E13', 'cost-estimator', 'Calibrated warm-prefill time estimation', 'raw', 161, 'cost-estimator-final3-20261030'),
    ('E14', 'roofline-estimator', 'Roofline hardware proxies versus empirical estimates', 'raw', 195, 'roofline-estimator-final-20261030'),
    ('E15', 'roofline-diagnosis', 'Roofline diagnosis, dependent-graph rates and operator correction', 'raw', 278, 'roofline-diagnose-final-20261030'),
    ('E16', 'cost-shapes', 'Batch/context diagnosis and shape interpolation negative result', 'raw', 916, 'cost-shapes-final-20261030'),
    ('E17', 'cost-shapes-v2', 'Time interpolation on new batch/context shapes', 'raw', 916, 'cost-shapes-v2-final-20261030'),
    ('E18', 'frozen-calibration-incomplete', 'Incomplete frozen-calibration run: cap64 setup rejection', 'raw_incomplete', 849, 'frozen-calibration-final-20261030'),
    ('E19', 'frozen-calibration', 'Frozen calibration: coverage, objective, residual basis and new batches', 'raw', 1162, 'frozen-calibration-final2-20261030'),
    ('E20', 'kalman-cost', 'Optional residual Kalman: fresh timings and prequential C replay', 'raw', 1162, 'kalman-cost-final-20261031'),
    ('E21', 'tool-prepare', 'Actual tool-window EWMA prefill and KV restoration', 'raw', 931, 'tool-prepare-final-20261031'),
    ('E22', 'restore-timing', 'Restore attribution: process launch, timer, queue and completion', 'raw', 547, 'restore-timing-final-20261031'),
    ('E23', 'restore-balance', 'Spawn and parent-progress ETA on new real-tool fixtures', 'raw', 979, 'restore-balance-final-20261031'),
    ('E24', 'tool-taskcost', 'Complete task audit, callback control and early snapshot release', 'raw', 659, 'tool-taskcost-final-20261031'),
    ('E25', 'tool-isolate', 'Snapshot unlink and last-close resource isolation', 'raw', 227, 'tool-isolate-final-20261031'),
    ('E26', 'tool-launch', 'First executable launch and incremental-build task validation', 'raw', 167, 'tool-launch-final-20261031'),
    ('E27', 'resident-flow', 'Normal resident KV six-turn actual-tool Native flow', 'raw', 199, 'resident-flow-final-20261031'),
    ('E28', 'resident-stream', 'Resident streaming from an actual local process, no sleep gaps', 'raw', 87, 'resident-stream-final-20261031'),
    ('E29', 'tool-output-cli', 'Actual agentctl resident tool-output prefill off/on', 'raw', 515, 'tool-output-final-20261031'),
    ('E30', 'tool-output-adaptive', 'Adaptive resident tool-output observation gate', 'raw', 515, 'tool-output-adaptive-final-20261031'),
    ('E31', 'tool-output-isolation', 'Matched output strategies and thread/token controls', 'raw', 1283, 'tool-output-isolation-final-20261031'),
    ('E32', 'tool-output-progress', 'Output-growth gate on new matched tasks', 'raw', 771, 'tool-output-progress-final-20261031'),
    ('E33', 'tool-output-reap', 'Producer CPU, EOF reaping and worker attribution', 'raw', 1027, 'tool-output-reap-final2-20261031'),
    ('E34', 'tool-output-event', 'Exit-event wait on fresh matched tasks', 'raw', 1539, 'tool-output-event-final-20261031'),
    ('E35', 'tool-output-race', 'Exit-in-progress bounded retry on fresh tasks', 'raw', 1283, 'tool-output-race-final-20261031'),
    ('E36', 'tool-output-contention', 'Same-load dormant/thread/token/prefill and CPU isolation', 'raw', 1283, 'tool-output-contention-final-20261031'),
    ('E37', 'tool-output-lazy', 'Lazy output-consumer launch on fresh tasks', 'raw', 771, 'tool-output-lazy-final2-20261031'),
    ('E38', 'tool-output-decode', 'Decode/sync spans and runnable scheduler isolation', 'raw', 1123, 'tool-output-decode-final2-20261031'),
    ('E39', 'tool-output-drain', 'Deferred internal sync on fresh tasks', 'raw', 1123, 'tool-output-drain-final-20261031'),
    ('E40', 'model-idle', 'Producer CPU supply with model absent or idle', 'raw', 211, 'model-idle-final-20261031'),
    ('E41', 'model-idle-stream', 'CPU supply after streaming model tasks', 'raw', 211, 'model-idle-stream-final-20261031'),
    ('E42', 'tool-output-metal', 'Metal command execution, queue overlap and host return', 'raw', 1539, 'tool-output-metal-final5-20261031'),
    ('E43', 'tool-output-notify', 'Completion marker and observer overhead controls', 'raw', 1539, 'tool-output-notify-final2-20261031'),
    ('E44', 'tool-output-coalesce', 'Bounded tail-batch coalescing on new tasks', 'raw', 1539, 'tool-output-coalesce-final-20261031'),
    ('E45', 'tool-output-tailwait', 'Matched sustained tail: GPU, completion and host return', 'raw', 1539, 'tool-output-tailwait-final-20261031'),
    ('E46', 'tool-output-capacity', 'Rotating capacity512/640/1024 controls on fresh tasks', 'raw', 4095, 'tool-output-capacity-final-20261031'),
    ('E47', 'tool-output-contention640', 'Same-capacity thread/token/GPU tool isolation', 'raw', 1539, 'tool-output-contention640-final-20261031'),
    ('E48', 'device-isolation', 'Fixed-shape native CPU versus memory pressure', 'raw', 387, 'device-isolation-final-20261031'),
    ('E49', 'tool-output-coarse640', 'Larger publication quantum on fresh actual tasks', 'raw', 1539, 'tool-output-coarse640-final-20261031'),
    ('E50', 'agent-foundation', 'Guarded editing, clean test environment and local text helper', 'functional', 0, 'agent-foundation-20261101'),
    ('E51', 'agent-data-policy', 'Unified protected files and default-deny cloud data egress', 'functional', 0, 'agent-data-policy-20261101'),
    ('E52', 'test-sandbox', 'OS test.run file/network isolation and matched overhead', 'raw', 230, 'test-sandbox-overhead-final-20261101'),
    ('E53', 'test-sandbox-host', 'Protected host environment, narrowed system information and final overhead', 'raw', 230, 'test-sandbox-host-overhead-final-20261101'),
    ('E54', 'sandbox-cc-diagnostic', 'Xcode lookup-cache, launch and denied-IPC controls', 'raw', 126, 'sandbox-cc-diagnostic-final-20261101'),
    ('E55', 'sandbox-cc-tasks', 'Explicit same-Xcode/SDK binding on fresh actual CLI tasks', 'raw', 108, 'sandbox-cc-tasks-final-20261101'),
    ('E56', 'sandbox-cpp-diagnostic', 'Fixed-target C++ compiler/header/cache and host coordination controls', 'raw', 270, 'sandbox-cpp-diagnostic-final-20261102'),
    ('E57', 'sandbox-cpp-tasks', 'Fresh C++ tasks: cold private PCH and bounded-regex candidate', 'raw', 180, 'sandbox-cpp-tasks-final3-20261102'),
    ('E58', 'self-speculation', 'Read-only same-model fork probe: functional preparation and target-machine handoff', 'functional', 0, 'self-spec-handoff-check-20261102'),
    ('L01', 'legacy-observations', 'Earlier service, budget and copy observations', 'reported_only', 0, ''),
]
DIMS = ('kind', 'scenario', 'shape', 'size', 'prefix_lines', 'words', 'records',
        'history_records', 'producer_iterations', 'window_ms', 'gap_ms', 'mode', 'rc', 'prefix_tokens', 'new_tokens', 'batch_tokens', 'depth','coverage','anchor_stage','phase','step','pressure','policy')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def dump(value):
    return json.dumps(value, indent=2, ensure_ascii=False) + '\n'


def csv_text(fields, rows):
    stream = io.StringIO(newline='')
    writer = csv.DictWriter(stream, fieldnames=fields, lineterminator='\n')
    writer.writeheader()
    writer.writerows(rows)
    return stream.getvalue()


def evidence_date(eid):
    if eid in ('E56','E57','E58'): return '20261102'
    if eid in ('E50','E51','E52','E53','E54','E55'): return '20261101'
    return '20261031' if eid in ('E20','E21','E22','E23','E24','E25','E26','E27','E28','E29','E30','E31','E32','E33','E34','E35','E36','E37','E38','E39','E40','E41','E42','E43','E44','E45','E46','E47','E48','E49') else '20261020' if eid=='L01' else '20261030'


def load_evidence():
    result = {}
    for eid, stem, title, level, count, directory in SPECS:
        date = evidence_date(eid)
        path = ROOT / f'docs/evidence/{stem}-{date}.json'
        data = json.loads(path.read_text())
        assert len(data.get('rows', [])) == count, (eid, 'unexpected record count')
        result[eid] = data
    return result


def samples(evidence, eid, **filters):
    key = 'filter_rows' if filters.get('kind','').startswith('filter_') else 'rows'
    return [r for r in evidence[eid].get(key, [])
            if all(r.get(k, 'timing' if k == 'kind' else None) == v for k, v in filters.items())]


def stats(evidence, eid, field, **filters):
    if field.startswith('isolation:'):
        rows=[r for r in evidence[eid]['isolation_samples'] if all(r.get(k)==v for k,v in filters.items())];assert rows
        key=field.split(':',1)[1];values=[r['metal_gpu_union_ms']/r['new_tokens'] if key=='gpu_per_token_ms' else r[key] for r in rows]
        return {'n':len(values),'median':statistics.median(values),'min':min(values),'max':max(values)}
    if field.startswith('metal:'):
        rows=[r for r in evidence[eid]['metal_tasks'] if all(r.get(k)==v for k,v in filters.items())];assert rows
        values=[r[field.split(':',1)[1]] for r in rows]
        return {'n':len(values),'median':statistics.median(values),'min':min(values),'max':max(values)}
    if field.startswith('idle:'):
        from analyze_model_idle import controls
        rows=[r for r in controls(evidence[eid]['rows']) if all(r.get(k)==v for k,v in filters.items())];assert rows
        values=[r[field.split(':',1)[1]] for r in rows]
        return {'n':len(values),'median':statistics.median(values),'min':min(values),'max':max(values)}
    if field.startswith('decode:'):
        rows=[r for r in evidence[eid]['decode_tasks'] if all(r.get(k)==v for k,v in filters.items())];assert rows
        values=[r[field.split(':',1)[1]] for r in rows]
        return {'n':len(values),'median':statistics.median(values),'min':min(values),'max':max(values)}
    if field.startswith('producer:'):
        from analyze_decode_schedule import producer
        rows=samples(evidence,eid,**filters);assert rows
        values=[producer(r['producer'])[field.split(':',1)[1]] for r in rows]
        return {'n':len(values),'median':statistics.median(values),'min':min(values),'max':max(values)}
    if field.startswith('contention:'):
        rows=[r for r in evidence[eid]['contention_tasks'] if all(r.get(k)==v for k,v in filters.items())];assert rows
        values=[r[field.split(':',1)[1]] for r in rows]
        return {'n':len(values),'median':statistics.median(values),'min':min(values),'max':max(values)}
    rows = samples(evidence, eid, **filters)
    assert rows, (eid, filters)
    if field.startswith('stage:'):
        if eid=='E22':from bench_restore_timing import parts
        elif eid=='E23':from bench_restore_balance import parts
        elif eid=='E24':from bench_tool_taskcost import parts
        elif eid=='E25':from bench_tool_isolate import parts
        elif eid=='E26':from bench_tool_launch import parts
        else:raise AssertionError('unknown stage source')
        values=[parts(r)[field[6:]] for r in rows]
    else:
        values = [max(0, r['sampled_heap_peak'] - r['heap_before']) if field == 'heap_extra_bytes' else r[field] for r in rows]
    return {'n': len(values), 'median': statistics.median(values), 'min': min(values), 'max': max(values)}


def fmt(stat, scale=1, precision=1):
    return f"{stat['median']/scale:.{precision}f} [{stat['min']/scale:.{precision}f}–{stat['max']/scale:.{precision}f}]"


def frozen_training(data):
    from analyze_frozen_calibration import features
    from bench_cost_shapes import error_summary
    out=[]
    for batch in [256,64]:
        fitted=next(r for r in data['rows'] if r['kind']=='fitted_models' and r['batch_tokens']==batch)
        for coverage in ['sparse','diverse']:
            rows=[r for r in data['rows'] if r.get('scenario')=='calibration' and r['batch_tokens']==batch and r['coverage']==coverage]
            for mode in ['raw','legacy','weighted','residual']:
                if mode=='weighted':coefficient=next(c['coefficient'] for c in data['offline_coefficients'] if c['batch_tokens']==batch and c['coverage']==coverage)
                elif mode!='raw':coefficient=fitted[mode+'_'+coverage]
                values=[]
                for r in rows:
                    prediction=r['raw_ms'] if mode=='raw' else max(0,(r['raw_ms'] if mode=='residual' else 0)+sum(a*b for a,b in zip(coefficient,features(r,mode=='residual'))))
                    values.append(dict(r,training_ms=prediction))
                result=error_summary(values,'training');result['mode']=mode
                out.append(dict(batch_tokens=batch,coverage=coverage,**result))
    return out


def tool_restore_components(data):
    out=[]
    for words in [128,2048]:
        for mode in ['restore_late','restore_immediate','restore_timed']:
            rows=[r for r in data['rows'] if r['kind']=='timing' and r['phase']=='evaluation' and r['scenario']=='compile_test' and r['words']==words and r['mode']==mode]
            values={key:[] for key in ['prepare_sum_ms','native_restore_sum_ms','outside_native_sum_ms']}
            for rep in range(7):
                group=[r for r in rows if r['rep']==rep];assert len(group)==3
                total=sum(r['prepare_ms'] for r in group);native=sum(r['restore_ms'] for r in group)
                values['prepare_sum_ms'].append(total);values['native_restore_sum_ms'].append(native);values['outside_native_sum_ms'].append(total-native)
            out.append(dict(words=words,mode=mode,samples=7,**{key+'_'+stat:fn(v) for key,v in values.items() for stat,fn in [('median',statistics.median),('min',min),('max',max)]}))
    return out


def table(headers, rows):
    return '\n'.join(['| ' + ' | '.join(headers) + ' |',
                      '| ' + ' | '.join('---' for _ in headers) + ' |'] +
                     ['| ' + ' | '.join(str(x) for x in row) + ' |' for row in rows])


def check_summaries(evidence):
    comparisons = 0
    for eid, data in evidence.items():
        if eid in ('E52','E53','E54','E55','E56','E57'):continue  # Paired overhead has its own independent replay below.
        for summary in data.get('summary', []):
            filters = {k: summary[k] for k in DIMS if k in summary and isinstance(summary[k],(str,int,float,bool)) and summary[k] != ''}
            # Direct-API fork/state rows have no mode; memory summaries are memory-only.
            if eid in ('E07', 'E08', 'E09'):
                filters['kind'] = 'memory'
            elif eid in ('E05', 'E06', 'E10', 'E11'):
                filters['kind'] = 'timing'
            rows = samples(evidence, eid, **filters)
            assert len(rows) == summary.get('samples', summary.get('n')), (eid, filters, 'sample count')
            for key, value in summary.items():
                if isinstance(value, dict) and set(value) == {'median', 'min', 'max'}:
                    actual = stats(evidence, eid, key, **filters)
                    for which in value:
                        assert math.isclose(actual[which], value[which], abs_tol=1e-6), (eid, key, which)
                        comparisons += 1
                else:
                    for suffix in ('_median', '_min', '_max'):
                        if key.endswith(suffix):
                            metric = key[:-len(suffix)]
                            if metric == 'heap_extra':
                                metric = 'heap_extra_bytes'
                            actual = stats(evidence, eid, metric, **filters)[suffix[1:]]
                            assert math.isclose(actual, value, abs_tol=1e-6), (eid, key)
                            comparisons += 1
    # Independent invariants: completed input, same input-token work, ordered timestamps.
    for r in samples(evidence, 'E11', kind='timing'):
        assert r['correct'] and r['frames'] == 17
        assert r['input_evaluated_tokens'] == (701 if r['scenario'] == 'text_prefill' else 619)
        assert r['source_start_ns'] <= r['input_end_ns'] <= r['model_ready_ns'] <= r['first_token_ns'] <= r['done_ns']
        for name, start, end in [('input_ms', 'source_start_ns', 'input_end_ns'),
                                 ('total_ready_ms', 'source_start_ns', 'model_ready_ns'),
                                 ('post_input_ready_ms', 'input_end_ns', 'model_ready_ns')]:
            assert math.isclose(r[name], (r[end]-r[start])/1e6, abs_tol=1e-6)
    for p in evidence['E11']['paired_summary']:
        filt = dict(kind='timing', scenario=p['scenario'], gap_ms=p['gap_ms'])
        base = {r['rep']: r for r in samples(evidence, 'E11', mode='wait_all', **filt)}
        other = {r['rep']: r for r in samples(evidence, 'E11', mode=p['mode'], **filt)}
        differences = [base[i]['total_ready_ms'] - other[i]['total_ready_ms'] for i in sorted(base)]
        assert len(differences) == p['pairs'] == 7
        assert sum(x > 0 for x in differences) == p['positive_total_pairs']
        assert math.isclose(statistics.median(differences), p['median_paired_total_savings_ms'], abs_tol=1e-6)
    from bench_cost_estimator import summarize
    cost=evidence['E13'];computed=summarize(cost['rows'])
    profile={k:cost[k] for k in ['hardware','model','model_sha256','llama_commit','runtime_libraries_sha256','configuration']}
    assert hashlib.sha256(json.dumps(profile,sort_keys=True).encode()).hexdigest()==cost['profile_sha256']
    assert next(r for r in cost['rows'] if r['kind']=='metadata')['profile_tag']==cost['profile_tag']==cost['profile_sha256'][:16]
    assert len(computed)==len(cost['prediction_summary'])==9
    for a,b in zip(computed,cost['prediction_summary']):
        for key,value in a.items():
            if isinstance(value,(int,float)):
                assert math.isclose(value,b[key],abs_tol=1e-6),(key,'E13 prediction summary')
                comparisons+=1
            else:assert value==b[key]
    train=samples(evidence,'E13',scenario='calibration');test=samples(evidence,'E13',scenario='evaluation')
    assert len(train)==63 and len(test)==84
    assert not ({(r['prefix_tokens'],r['new_tokens']) for r in train}&{(r['prefix_tokens'],r['new_tokens']) for r in test})
    fitted=next(r for r in cost['rows'] if r['kind']=='calibration_model')
    rate=sum(r['wall_ms'] for r in train)/sum(r['new_tokens'] for r in train)
    for r in test:
        assert r['correct'] and r['wall_ms']>0
        n=r['new_tokens'];p=r['prefix_tokens'];batch=r['batch_tokens']
        x=[(n+batch-1)//batch,n/512,(p*n+n*(n-1)/2)/1048576]
        assert math.isclose(r['frozen_ms'],sum(a*b for a,b in zip(fitted['coefficient'],x)),abs_tol=1e-5)
        assert math.isclose(r['fixed_ms'],rate*n,abs_tol=1e-5)
        for mode in ['fixed','frozen','online']:
            assert math.isclose(r[mode+'_abs_error_ms'],abs(r[mode+'_ms']-r['wall_ms']),abs_tol=3e-6)
            comparisons+=1
    from bench_roofline_estimator import summarize as roof_summary
    roof=evidence['E14']
    profile={k:roof[k] for k in ['estimator_schema','hardware','model','model_sha256','llama_commit','runtime_libraries_sha256','configuration']}
    assert hashlib.sha256(json.dumps(profile,sort_keys=True).encode()).hexdigest()==roof['profile_sha256']
    assert next(r for r in roof['rows'] if r['kind']=='metadata')['profile_tag']==roof['profile_tag']==roof['profile_sha256'][:16]
    computed=roof_summary(roof['rows'])
    assert len(computed)==len(roof['prediction_summary'])==15
    for a,b in zip(computed,roof['prediction_summary']):
        for key,value in a.items():
            if isinstance(value,(int,float)):
                assert math.isclose(value,b[key],abs_tol=1e-6),(key,'E14 prediction summary')
                comparisons+=1
            else:assert value==b[key]
    train=samples(evidence,'E14',scenario='calibration');test=samples(evidence,'E14',scenario='evaluation')
    assert len(train)==63 and len(test)==84
    assert not ({(r['prefix_tokens'],r['new_tokens']) for r in train}&{(r['prefix_tokens'],r['new_tokens']) for r in test})
    device=next(r for r in roof['rows'] if r['kind']=='hardware_profile')
    for mode,field,work in [('q8_gemm','linear_flops_s','flops'),('f16_gemm','attention_flops_s','flops'),('copy_64mib','bandwidth_bytes_s','traffic_bytes')]:
        probes=samples(evidence,'E14',kind='hardware_probe',mode=mode)
        assert len(probes)==7 and all(r['correct'] for r in probes)
        rate=probes[0][work]/(statistics.median(r['wall_ms'] for r in probes)/1000)
        assert math.isclose(rate,device[field],rel_tol=1e-9)
        comparisons+=1
    fitted=next(r for r in roof['rows'] if r['kind']=='roofline_calibration')
    for r in train+test:
        assert r['correct'] and r['wall_ms']>0
        assert math.isclose(r['roofline_ms'],r['roofline_linear_ms']+r['roofline_attention_ms'],abs_tol=2e-6)
        comparisons+=1
    for r in test:
        calls=(r['new_tokens']+r['batch_tokens']-1)//r['batch_tokens']
        x=[r['roofline_linear_ms'],r['roofline_attention_ms'],calls]
        assert math.isclose(r['roofline_calibrated_ms'],sum(a*b for a,b in zip(fitted['coefficient'],x)),abs_tol=1e-5)
        for mode in ['fixed','frozen','online','roofline','roofline_calibrated']:
            assert math.isclose(r[mode+'_abs_error_ms'],abs(r[mode+'_ms']-r['wall_ms']),abs_tol=3e-6)
            comparisons+=1
    from bench_roofline_diagnose import summarize as diagnosis_summary, audit_e14
    diag=evidence['E15'];computed=diagnosis_summary(diag['rows'])
    profile={k:diag[k] for k in ['estimator_schema','hardware','model','model_sha256','llama_commit','runtime_libraries_sha256','configuration']}
    assert hashlib.sha256(json.dumps(profile,sort_keys=True).encode()).hexdigest()==diag['profile_sha256']
    assert next(r for r in diag['rows'] if r['kind']=='metadata')['profile_tag']==diag['profile_tag']==diag['profile_sha256'][:16]
    assert diag['audit_e14']==audit_e14()
    assert len(computed)==len(diag['prediction_summary'])==33
    for a,b in zip(computed,diag['prediction_summary']):
        for key,value in a.items():
            if isinstance(value,(int,float)):
                assert math.isclose(value,b[key],abs_tol=1e-6),(key,'E15 prediction summary')
                comparisons+=1
            else:assert value==b[key]
    for rate in samples(evidence,'E15',kind='graph_rate'):
        first=samples(evidence,'E15',kind='graph_probe',mode=rate['mode'],depth=1)
        many=samples(evidence,'E15',kind='graph_probe',mode=rate['mode'],depth=16)
        assert len(first)==len(many)==7 and min(r['wall_ms'] for r in many)>max(r['wall_ms'] for r in first)
        assert all(r['correct'] for r in first+many)
        a=statistics.median(r['wall_ms'] for r in first);b=statistics.median(r['wall_ms'] for r in many)
        assert math.isclose(rate['per_op_ms'],(b-a)/15,abs_tol=1e-9)
        assert math.isclose(rate['graph_intercept_ms'],a-(b-a)/15,abs_tol=1e-9)
        field='traffic_bytes' if rate['mode']=='copy_chain' else 'flops'
        assert math.isclose(rate['effective_rate'],(many[0][field]-first[0][field])/((b-a)/1000),rel_tol=1e-9)
        comparisons+=3
    train=samples(evidence,'E15',scenario='calibration');test=samples(evidence,'E15',scenario='evaluation')
    assert len(train)==63 and len(test)==84
    assert not ({(r['prefix_tokens'],r['new_tokens']) for r in train}&{(r['prefix_tokens'],r['new_tokens']) for r in test})
    fitted=next(r for r in diag['rows'] if r['kind']=='roofline_calibration')
    modes=['fixed','frozen','online','legacy','math_only','bandwidth_only','linear_only','attention_only','rate_only','corrected','corrected_calibrated']
    for r in test:
        assert r['correct'] and r['wall_ms']>0 and r['math_only_ms']<=r['legacy_ms']
        assert math.isclose(r['corrected_ms'],r['corrected_linear_ms']+r['corrected_attention_ms'],abs_tol=2e-6)
        x=[r['corrected_linear_ms'],r['corrected_attention_ms'],(r['new_tokens']+r['batch_tokens']-1)//r['batch_tokens']]
        assert math.isclose(r['corrected_calibrated_ms'],sum(a*b for a,b in zip(fitted['coefficient'],x)),abs_tol=1e-5)
        for mode in modes:
            assert math.isclose(r[mode+'_abs_error_ms'],abs(r[mode+'_ms']-r['wall_ms']),abs_tol=3e-6)
            comparisons+=1
    from bench_cost_shapes import summarize as shape_summary, grouped as shape_groups, historical_e15, MODES
    for eid in ['E16','E17']:
        data=evidence[eid]
        profile={k:data[k] for k in ['estimator_schema','hardware','model','model_sha256','llama_commit','runtime_libraries_sha256','configuration']}
        assert hashlib.sha256(json.dumps(profile,sort_keys=True).encode()).hexdigest()==data['profile_sha256']
        assert next(r for r in data['rows'] if r['kind']=='metadata')['profile_tag']==data['profile_tag']==data['profile_sha256'][:16]
        assert shape_summary(data['rows'])==data['prediction_summary']
        assert shape_groups(data['rows'])==data['cell_summary']
        assert historical_e15()==data['historical_e15']
        for rate in samples(evidence,eid,kind='shape_rate'):
            filt={k:rate[k] for k in ['mode','prefix_tokens','new_tokens']}
            one=samples(evidence,eid,kind='shape_probe',depth=1,**filt)
            many=samples(evidence,eid,kind='shape_probe',depth=16,**filt)
            assert len(one)==len(many)==7 and min(r['wall_ms'] for r in many)>max(r['wall_ms'] for r in one)
            delta=statistics.median(r['wall_ms'] for r in many)-statistics.median(r['wall_ms'] for r in one)
            assert math.isclose(rate['per_op_ms'],delta/15,abs_tol=1e-9)
            assert math.isclose(rate['effective_rate'],(many[0]['flops']-one[0]['flops'])/(delta/1000),rel_tol=1e-9)
            comparisons+=2
        for batch in [512,128]:
            train=[r for r in data['rows'] if r.get('scenario')=='calibration' and r['batch_tokens']==batch]
            test=[r for r in data['rows'] if r.get('scenario')=='evaluation' and r['batch_tokens']==batch]
            assert len(train)==63 and len(test)==147
            assert not ({(r['prefix_tokens'],r['new_tokens']) for r in train}&{(r['prefix_tokens'],r['new_tokens']) for r in test})
            assert all(r['correct'] and r['wall_ms']>0 for r in train+test)
            for r in test:
                assert r['chunk_calls']==(r['new_tokens']+batch-1)//batch==r['fa_vec_calls']+r['fa_dense_calls']
                assert math.isclose(r['roof_raw_ms'],r['roof_linear_ms']+r['roof_attention_ms'],abs_tol=2e-6)
                assert math.isclose(r['shape_raw_ms'],r['shape_linear_ms']+r['shape_attention_ms'],abs_tol=2e-6)
                for mode in MODES:
                    assert math.isclose(r[mode+'_abs_error_ms'],abs(r[mode+'_ms']-r['wall_ms']),abs_tol=3e-6)
                    comparisons+=1
            # Independent replay verifies that every online value uses only
            # earlier labels, with the same 64-slot window and refit budget.
            for family in ['empirical','roof','shape']:
                coef=[0.0]*3;window=[];slot=0
                for r in train+test:
                    n=r['new_tokens'];p=r['prefix_tokens'];calls=(n+batch-1)//batch
                    x=([calls,n/512,(p*n+n*(n-1)/2)/1048576] if family=='empirical' else
                       [r[family+'_linear_ms'],r[family+'_attention_ms'],calls])
                    if r['scenario']=='evaluation':
                        assert math.isclose(sum(a*b for a,b in zip(coef,x)),r[family+'_online_ms'],abs_tol=0.01), (eid,batch,family,'prequential replay')
                        comparisons+=1
                    if len(window)<64:window.append((x,r['wall_ms']))
                    else:window[slot]=(x,r['wall_ms'])
                    slot=(slot+1)%64
                    a=[[sum(v[0][i]*v[0][j] for v in window) for j in range(3)] for i in range(3)]
                    b=[sum(v[0][i]*v[1] for v in window) for i in range(3)]
                    for _ in range(128):
                        for j in range(3):coef[j]=max(0,(b[j]-sum(a[j][k]*coef[k] for k in range(3) if k!=j))/a[j][j]) if a[j][j] else 0
        assert sum(r['fresh_shapes'] for r in samples(evidence,eid,kind='correctness') if r['passed'])==60
    from bench_frozen_calibration import summarize as frozen_summary, grouped as frozen_groups, weighted_controls, anchors as frozen_anchors
    from analyze_frozen_calibration import audit as frozen_audit, features as frozen_features, fit as residual_fit
    data=evidence['E19'];profile={k:data[k] for k in ['estimator_schema','hardware','model','model_sha256','llama_commit','runtime_libraries_sha256','configuration']}
    assert hashlib.sha256(json.dumps(profile,sort_keys=True).encode()).hexdigest()==data['profile_sha256']
    assert next(r for r in data['rows'] if r['kind']=='metadata')['profile_tag']==data['profile_tag']==data['profile_sha256'][:16]
    coefs,predictions=weighted_controls(data['rows'])
    assert coefs==data['offline_coefficients'] and predictions==data['offline_predictions']
    assert len(predictions)==294 and frozen_summary(data)==data['prediction_summary'] and frozen_groups(data)==data['cell_summary']
    assert frozen_anchors(data['rows'])==data['anchor_summary'] and frozen_audit()==data['development_e17']['audit']
    assert sha(ROOT/data['development_e17']['source'])==data['development_e17']['source_sha256']
    assert sum(r['fresh_shapes'] for r in samples(evidence,'E19',kind='correctness') if r['passed'])==78
    table=next(r for r in data['rows'] if r['kind']=='shape_table')
    assert table['contexts']==[0,2048,6144,7168] and table['widths']==[8,16,19,20,128,512]
    for rate in samples(evidence,'E19',kind='shape_rate'):
        filt={k:rate[k] for k in ['mode','prefix_tokens','new_tokens']}
        one=samples(evidence,'E19',kind='shape_probe',depth=1,**filt)
        many=samples(evidence,'E19',kind='shape_probe',depth=16,**filt)
        assert len(one)==len(many)==7 and min(r['wall_ms'] for r in many)>max(r['wall_ms'] for r in one)
        delta=statistics.median(r['wall_ms'] for r in many)-statistics.median(r['wall_ms'] for r in one)
        assert math.isclose(rate['per_op_ms'],delta/15,abs_tol=1e-9)
        assert math.isclose(rate['effective_rate'],(many[0]['flops']-one[0]['flops'])/(delta/1000),rel_tol=1e-9)
        i=table['widths'].index(rate['new_tokens'])
        if rate['mode']=='q8_chain':assert math.isclose(table['linear_flops_s'][i],rate['effective_rate'],rel_tol=1e-12)
        else:assert math.isclose(table['attention_ms'][i][table['contexts'].index(rate['prefix_tokens'])],rate['per_op_ms'],abs_tol=1e-9)
        comparisons+=3
    for batch in [256,64]:
        fitted=next(r for r in data['rows'] if r['kind']=='fitted_models' and r['batch_tokens']==batch)
        evaluation=samples(evidence,'E19',scenario='evaluation',batch_tokens=batch)
        assert len(evaluation)==147
        for coverage in ['sparse','diverse']:
            train=samples(evidence,'E19',scenario='calibration',batch_tokens=batch,coverage=coverage)
            assert len(train)==63
            assert not ({(r['prefix_tokens'],r['new_tokens']) for r in train}&{(r['prefix_tokens'],r['new_tokens']) for r in evaluation})
            coefficient=residual_fit([frozen_features(r,True) for r in train],
                                     [r['wall_ms']-r['raw_ms'] for r in train],
                                     [1/max(r['wall_ms'],20) for r in train],0.01)
            assert all(math.isclose(a,b,abs_tol=1e-5) for a,b in zip(coefficient,fitted['residual_'+coverage]))
            for r in evaluation:
                old=sum(a*b for a,b in zip(fitted['legacy_'+coverage],frozen_features(r)))
                new=max(0,r['raw_ms']+sum(a*b for a,b in zip(coefficient,frozen_features(r,True))))
                assert math.isclose(old,r['legacy_'+coverage+'_ms'],abs_tol=1e-6)
                assert math.isclose(new,r['residual_'+coverage+'_ms'],abs_tol=1e-5)
                comparisons+=2
        for r in evaluation:
            assert r['correct'] and r['wall_ms']>0
            assert r['chunk_calls']==(r['new_tokens']+batch-1)//batch==r['fa_vec_calls']+r['fa_dense_calls']
            assert math.isclose(r['raw_ms'],r['shape_linear_ms']+r['shape_attention_ms'],abs_tol=2e-9)
            assert math.isclose(r['raw_ms'],r['clip_linear_ms']+r['clip_attention_ms'],abs_tol=2e-9)
            for mode in ['raw','legacy_clipped_sparse','legacy_sparse','legacy_diverse','residual_sparse','residual_diverse','empirical_online','shape_online']:
                assert math.isclose(r[mode+'_abs_error_ms'],abs(r[mode+'_ms']-r['wall_ms']),abs_tol=3e-9)
                comparisons+=1
    incomplete=evidence['E18']
    assert incomplete['failure']['batch64_measurement_samples']==0
    assert all(r.get('batch_tokens')!=64 for r in incomplete['rows'] if r.get('kind')=='cost_sample')
    assert sum(r['fresh_shapes'] for r in samples(evidence,'E18',kind='correctness') if r['passed'])==39
    from bench_cost_shapes import error_summary as partial_error
    for summary in incomplete['partial_summary']:
        assert dict(batch_tokens=256,**partial_error(samples(evidence,'E18',scenario='evaluation'),summary['mode']))==summary
    from bench_kalman_cost import check_replay
    data=evidence['E20'];check_replay(data)
    assert len(data['filter_rows'])==893
    assert sum(r['fresh_shapes'] for r in data['rows'] if r['kind']=='correctness' and r['passed'])==78
    profile={k:data[k] for k in ['estimator_schema','hardware','model','model_sha256','llama_commit','runtime_libraries_sha256','configuration']}
    assert hashlib.sha256(json.dumps(profile,sort_keys=True).encode()).hexdigest()==data['profile_sha256']
    for row in data['rows']:
        if row.get('scenario')=='evaluation':
            assert row['correct'] and row['wall_ms']>0
            assert row['prefix_tokens'] in [1792,3840,5504] and row['new_tokens'] in [11,18,26,88,208,400,688]
            assert row['chunk_calls']==(row['new_tokens']+row['batch_tokens']-1)//row['batch_tokens']
    comparisons+=882*12
    from bench_tool_prepare import check as check_tool_prepare
    data=evidence['E21'];check_tool_prepare(data)
    assert data['verified_full_gpu_offload']
    assert all(data['validation'][k]['passed'] for k in ['native_sanitizers','default_regression','cli_integration'])
    comparisons+=696*12
    from bench_restore_timing import check as check_restore_timing
    from bench_restore_balance import check as check_restore_balance
    check_restore_timing(evidence['E22']);check_restore_balance(evidence['E23'])
    assert evidence['E23']['validation']['sanitizers']['passed'] and evidence['E23']['validation']['default_regression']['passed']
    assert evidence['E23']['validation']['cli_integration']['passed']
    from bench_tool_taskcost import check as check_taskcost
    check_taskcost(evidence['E24'])
    assert all(evidence['E24']['validation'][k]['passed'] for k in ['sanitizers','default_regression','cli_integration'])
    from bench_tool_isolate import check as check_isolate
    check_isolate(evidence['E25'])
    assert all(evidence['E25']['validation'][k]['passed'] for k in ['sanitizers','default_regression','cli_integration'])
    from bench_tool_launch import check as check_launch
    check_launch(evidence['E26'])
    assert evidence['E26']['validation']['sanitizers']['passed'] and evidence['E26']['validation']['unchanged_runtime_regression']['passed']
    from bench_resident import check as check_resident
    check_resident(evidence['E27']);check_resident(evidence['E28'])
    assert all(evidence[eid]['validation']['sanitizers']['passed'] and evidence[eid]['validation']['default_regression']['passed'] for eid in ['E27','E28'])
    from bench_tool_output import check as check_output
    assert all(evidence[eid]['validation']['sanitizers']['passed'] and evidence[eid]['validation']['default_regression']['passed'] for eid in ['E31','E32'])
    check_output(evidence['E29']);check_output(evidence['E30']);check_output(evidence['E31']);check_output(evidence['E32'])
    for r in evidence['E31']['rows']:
        if r['kind']=='tool' and r['mode'] in ('thread','token'):
            assert r['evaluated_tokens']==0 and r['prefill_ms']==0
            if r['mode']=='thread':assert r['tokenized_tokens']==0
    burst=[r for r in evidence['E32']['rows'] if r['kind']=='tool' and r['mode']=='progress' and r['phase']=='evaluation' and r['producer_iterations']==0]
    assert len(burst)==42 and all(r['updates']==0 and r['evaluated_tokens']==0 for r in burst)
    from analyze_output_isolation import contrasts
    for eid in ['E31','E32']:assert evidence[eid]['contrasts']==contrasts(evidence[eid]['rows'],evidence[eid]['contrast_pairs'])
    assert evidence['E29']['validation']['sanitizers']['passed'] and evidence['E29']['validation']['default_regression']['passed']
    from analyze_output_reap import analyze
    for eid in ['E33','E34','E35']:
        d=evidence[eid];check_output(d)
        assert d['attribution']==analyze(d['rows'],d['attribution_pairs'])
        assert d['validation']['sanitizers']['passed'] and d['validation']['default_regression']['passed']
    from analyze_output_contention import contrasts as contention_contrasts,tasks as contention_tasks
    for eid in ['E36','E37']:
        d=evidence[eid];check_output(d)
        assert d['contention_tasks']==contention_tasks(d['rows'])
        assert d['contention_contrasts']==contention_contrasts(d['rows'],d['contrast_pairs'])
        assert d['validation']['sanitizers']['passed'] and d['validation']['default_regression']['passed']
    for r in evidence['E36']['rows']:
        if r['kind']=='tool' and r['mode']!='progress-race':
            assert r['evaluated_tokens']==0 and r['prefill_ms']==0
            if r['mode']!='token-race':assert r['updates']==0
    for r in evidence['E37']['rows']:
        if r['kind']=='tool' and r['producer_iterations']==0 and r['mode']=='lazy-race':assert r['consumer_threads']==r['updates']==r['evaluated_tokens']==0
    from analyze_decode_schedule import check as check_decode
    for eid in ['E38','E39']:
        d=evidence[eid];check_output(d);check_decode(d)
        assert len([r for r in d['rows'] if r['kind']=='producer_control'])==96
        assert all(d['validation'][k]['passed'] for k in ['sanitizers','default_regression','native_long','stream'])
        for c in d['calibration']['rows']:
            if c['busy']:assert abs(c['proc_cpu_ticks']*c['timebase_numer']/c['timebase_denom']-c['cpu_clock_ns'])<5000000
    from analyze_model_idle import check as check_idle
    check_idle(evidence['E40']);check_idle(evidence['E41'])
    from analyze_metal_timeline import check as check_metal
    for eid in ['E42','E43','E44','E45','E47','E49']:
        d=evidence[eid];check_output(d);check_metal(d)
        assert d['validation']['sanitizers']['passed']
        assert len(d['metal_tasks'])==168
    assert all(evidence['E44']['validation'][k]['passed'] for k in ['default_regression','native_long','stream'])
    from bench_batch_capacity import check as check_capacity
    check_capacity(evidence['E46'])
    assert all(evidence['E46']['validation'][k]['passed'] for k in ['sanitizers','native_long','default_regression','stream'])
    from bench_device_contention import check as check_device
    from analyze_batch_trace import check as check_batch
    check_device(evidence['E48'])
    for eid in ['E47','E48','E49']:check_batch(evidence[eid])
    assert all(evidence['E49']['validation'][k]['passed'] for k in ['native_long','stream','default_regression'])
    probes=evidence['E35']['exit_probes']
    assert [len(p['rows']) for p in probes]==[100,200,200]
    assert all(r['rc']==0 for p in probes for r in p['rows'])
    slow=[r for r in probes[0]['rows'] if r['eof_wait_ms']>1]
    assert len(slow)==10 and all(r['last_errno']==3 and r['events']==0 for r in slow)
    assert all(r['eof_wait_ms']==0 for r in probes[-1]['rows'])
    comparisons+=56*18
    comparisons+=(408+732+492+168+123+168+84)*18
    foundation = evidence['E50']
    spec=evidence['E58']
    assert spec['evidence_level']=='functional' and spec['performance_rerun'] is False and not spec['rows']
    assert len(spec['validation']['controller_sanitizers']['cases'])==24
    assert len(spec['validation']['native_sanitizers']['cases'])==8
    assert all(v['passed'] for v in spec['validation'].values())
    assert len(spec['previous_evidence_sha256'])==58 and len(spec['previous_figures_sha256'])==82
    for group in ('previous_evidence_sha256','previous_figures_sha256'):
        for name,digest in spec[group].items():assert sha(ROOT/name)==digest,('historical evidence changed',name)
    assert foundation['evidence_level'] == 'functional' and foundation['performance_rerun'] is False
    assert all(v['passed'] for v in foundation['validation'].values())
    assert len(foundation['validation']['http_sanitizers']['suites']) == 8
    assert foundation['validation']['native_sanitizers']['long_context_tokens'] == 14357
    assert foundation['validation']['native_sanitizers']['tool_output_cases'] == 72
    assert len(foundation['previous_evidence_sha256']) == 50
    for name, digest in foundation['previous_evidence_sha256'].items():
        assert sha(ROOT/name) == digest, ('historical evidence changed', name)
    policy = evidence['E51']
    assert policy['evidence_level']=='functional' and policy['performance_rerun'] is False and not policy['rows']
    assert all(v['passed'] for v in policy['validation'].values())
    assert policy['validation']['policy_sanitizers']['checks']==445
    assert len(policy['validation']['http_sanitizers']['suites'])==12
    assert len(policy['validation']['foundation_sanitizers']['suites'])==8
    assert policy['validation']['native_sanitizers']['long_context_tokens']==14357
    assert policy['validation']['native_sanitizers']['tool_output_cases']==72
    assert len(policy['previous_evidence_sha256'])==51
    assert len(policy['previous_figures_sha256'])==74
    for group in ('previous_evidence_sha256','previous_figures_sha256'):
        for name,digest in policy[group].items():
            assert sha(ROOT/name)==digest,('historical artifact changed',name)
    sandbox=evidence['E52']
    assert len(sandbox['rows'])==230 and all(r['passed'] for r in sandbox['rows'])
    assert len(sandbox_pair_rows(sandbox['rows']))==93
    assert sandbox_pair_rows(sandbox['rows'])==sandbox['pairs']
    assert sandbox_summary(sandbox['rows'])==sandbox['summary']
    assert all(v['passed'] for v in sandbox['validation'].values())
    assert sandbox['validation']['c_sanitizers']['checks']==542
    assert len(sandbox['validation']['sandbox_sanitizers']['suites'])==11
    assert len(sandbox['validation']['data_policy_sanitizers']['suites'])==12
    assert len(sandbox['validation']['foundation_sanitizers']['suites'])==8
    assert sandbox['validation']['native_sanitizers']['long_context_tokens']==14357
    assert sandbox['loopback_negative_control']['synthetic_payload_received'] is True
    assert sandbox['loopback_negative_control']['decision'].startswith('Reject candidate')
    assert len(sandbox['previous_evidence_sha256'])==52 and len(sandbox['previous_figures_sha256'])==74
    for group in ['previous_evidence_sha256','previous_figures_sha256']:
        for name,digest in sandbox[group].items():assert sha(ROOT/name)==digest,('historical artifact changed',name)
    comparisons+=93*3
    final=evidence['E53']
    assert len(final['rows'])==230 and all(r['passed'] for r in final['rows'])
    assert final['pairs']==sandbox_pair_rows(final['rows']) and len(final['pairs'])==93
    assert final['summary']==sandbox_summary(final['rows'])
    assert all(v['passed'] for v in final['validation'].values())
    assert final['validation']['c_sanitizers']['checks']==542
    assert len(final['validation']['sandbox_sanitizers']['suites'])==12
    assert len(final['validation']['data_policy_sanitizers']['suites'])==12
    assert len(final['validation']['foundation_sanitizers']['suites'])==8
    assert final['validation']['native_sanitizers']['long_context_tokens']==14357
    assert final['host_environment_control']['before_marker_visible'] and not final['host_environment_control']['after_marker_visible']
    assert len(final['previous_evidence_sha256'])==53 and len(final['previous_figures_sha256'])==76
    for group in ['previous_evidence_sha256','previous_figures_sha256']:
        for name,digest in final[group].items():assert sha(ROOT/name)==digest,('historical artifact changed',name)
    comparisons+=93*3
    for eid,count in [('E54',126),('E55',108)]:
        d=evidence[eid]
        assert len(d['rows'])==count and all(r['passed'] for r in d['rows'])
        assert cc_mode_summary(d['rows'])==d['summary']
        assert cc_pair_rows(d['rows'])==d['pairs']
        assert cc_pair_summary(d['pairs'])==d['paired_summary']
        for group in ['previous_evidence_sha256','previous_figures_sha256']:
            for name,digest in d[group].items():assert sha(ROOT/name)==digest,('historical artifact changed',name)
        comparisons+=len(d['pairs'])*3
    assert len(evidence['E54']['ipc_rows'])==70
    assert all(r['success']==0 for r in evidence['E54']['ipc_rows'] if r['mode']=='on')
    v=evidence['E55']['validation']
    assert all(r['passed'] for r in v.values())
    assert v['c_sanitizers']['checks']==561 and len(v['sandbox_sanitizers']['suites'])==15
    assert len(v['data_policy_sanitizers']['suites'])==12 and len(v['foundation_sanitizers']['suites'])==8
    assert v['native_sanitizers']['long_context_tokens']==14357
    for eid,count in [('E56',270),('E57',180)]:
        d=evidence[eid];assert len(d['rows'])==count and all(r['passed'] for r in d['rows'])
        assert cpp_mode_summary(d['rows'])==d['summary'] and cpp_pair_rows(d['rows'])==d['pairs']
        assert cpp_pair_summary(d['pairs'])==d['paired_summary']
        for group in ['previous_evidence_sha256','previous_figures_sha256']:
            for name,digest in d[group].items():assert sha(ROOT/name)==digest,('historical artifact changed',name)
        comparisons+=len(d['pairs'])*3
    assert all(r['triple']=='arm64-apple-macosx26.0.0' and r['macro']==260000 for r in evidence['E56']['fixed_target_identity'])
    assert all(r['pcm_files']==0 for r in evidence['E56']['rows'] if r['case'] in ['object','reuse','chain'])
    assert all(r['pcm_files']>0 for r in evidence['E56']['rows'] if r['case']=='modules')
    assert evidence['E57']['validation']['recipe_sanitizers']['passed'] and len(evidence['E57']['validation']['recipe_sanitizers']['suites'])==6
    assert evidence['E57']['validation']['candidate_os']['passed'] and len(evidence['E57']['validation']['candidate_os']['suites'])==15
    return comparisons


def cpp_mode_summary(rows):
    out=[]
    for case,mode in sorted({(r['case'],r['mode']) for r in rows}):
        group=[r for r in rows if (r['case'],r['mode'],r['phase'])==(case,mode,'evaluation')]
        item=dict(case=case,mode=mode,n=len(group))
        keys=sorted(set.intersection(*(set(r) for r in group)))
        for k in keys:
            if k.endswith(('_ms','_bytes')) or k in ['pcm_files','pch_files','files','entries','eof_polls']:
                values=[r[k] for r in group];item[k]=dict(median=statistics.median(values),min=min(values),max=max(values))
        out.append(item)
    return out


def cpp_pair_rows(rows):
    groups=collections.defaultdict(dict)
    for r in rows:
        if r['phase']=='evaluation':
            key=(r['case'],r['rep']);assert r['mode'] not in groups[key];groups[key][r['mode']]=r
    out=[];cli=rows[0]['kind']=='cpp_cli'
    comparisons=[('isolation_overhead','off','on')]
    if cli:comparisons += [('group_saving','grouped_on','on'),('pch_saving','pch_on','on'),('pch_off_saving','pch_off','off'),('pch_isolation_overhead','pch_off','pch_on')]
    for (case,rep),g in sorted(groups.items()):
        for label,a,b in comparisons:
            if a not in g or b not in g:continue
            item=dict(case=case,rep=rep,comparison=label)
            for key in sorted(g[a].keys()&g[b].keys()):
                if key.endswith('_ms'):item[key[:-3]+'_delta_ms']=g[b][key]-g[a][key]
            out.append(item)
    return out


def cpp_pair_summary(rows):
    out=[]
    for case,comparison in sorted({(r['case'],r['comparison']) for r in rows}):
        group=[r for r in rows if (r['case'],r['comparison'])==(case,comparison)]
        item=dict(case=case,comparison=comparison,n=len(group),positive=sum(r['wall_delta_ms']>0 for r in group))
        for k in sorted(set.intersection(*(set(r) for r in group))):
            if k.endswith('_ms'):
                values=[r[k] for r in group];item[k]=dict(median=statistics.median(values),min=min(values),max=max(values))
        out.append(item)
    return out


def cpp_flat_summary(rows):
    return [dict(**{k:v for k,v in r.items() if not isinstance(v,dict)},metric=k,unit='ms' if k.endswith('_ms') else 'file_bytes' if k.endswith('_bytes') else 'count',median=v['median'],min=v['min'],max=v['max']) for r in rows for k,v in r.items() if isinstance(v,dict)]


def cc_mode_summary(rows):
    result=[]
    for case,mode in sorted({(r['case'],r['mode']) for r in rows}):
        g=[r for r in rows if (r['case'],r['mode'],r['phase'])==(case,mode,'evaluation')]
        item=dict(case=case,mode=mode,n=len(g))
        keys=['wall_ms','prepare_ms','cleanup_ms','spawn_ms','phase_first_ms','phase_second_ms','outside_phase_ms'] if rows[0]['kind']=='cc_diagnostic' else ['wall_ms','inner_ms','outside_inner_ms']
        for field in keys:
            values=[r[field] for r in g if field in r]
            if values:item[field]=dict(median=statistics.median(values),min=min(values),max=max(values))
        result.append(item)
    return result


def cc_pair_rows(rows):
    groups=collections.defaultdict(dict)
    for r in rows:
        if r['phase']=='evaluation':
            key=(r['case'],r['rep']);assert r['mode'] not in groups[key];groups[key][r['mode']]=r
    result=[];cli=rows[0]['kind']=='cc_cli'
    for (case,rep),g in sorted(groups.items()):
        comparisons=[('isolation_overhead','off','on')]
        if cli:comparisons += [('binding_saving','pinned_on','on'),('bound_isolation_overhead','pinned_off','pinned_on'),('binding_off_saving','pinned_off','off')]
        else:comparisons += [('nolog_overhead','on','quiet')]
        for label,a,b in comparisons:
            if a not in g or b not in g:continue
            field='inner_ms' if cli else 'phase_first_ms'
            wall=g[b]['wall_ms']-g[a]['wall_ms'];inner=g[b][field]-g[a][field]
            result.append(dict(case=case,rep=rep,comparison=label,wall_delta_ms=wall,inner_delta_ms=inner,outside_delta_ms=wall-inner))
    return result


def cc_pair_summary(pairs):
    result=[]
    for case,comparison in sorted({(r['case'],r['comparison']) for r in pairs}):
        g=[r for r in pairs if (r['case'],r['comparison'])==(case,comparison)]
        item=dict(case=case,comparison=comparison,n=len(g),positive=sum(r['wall_delta_ms']>0 for r in g))
        for field in ['wall_delta_ms','inner_delta_ms','outside_delta_ms']:
            values=[r[field] for r in g];item[field]=dict(median=statistics.median(values),min=min(values),max=max(values))
        result.append(item)
    return result


def cc_flat_summary(summary,paired=False):
    return [dict(**{k:v for k,v in item.items() if not isinstance(v,dict)},metric=field,median_ms=value['median'],min_ms=value['min'],max_ms=value['max']) for item in summary for field,value in item.items() if isinstance(value,dict)]


def sandbox_pair_rows(rows):
    groups=collections.defaultdict(dict)
    for r in rows:
        if r['phase']=='evaluation':
            key=(r['kind'],r['case'],r['files'],r['rep'])
            assert r['mode'] not in groups[key]
            groups[key][r['mode']]=r
    pairs=[]
    for (kind,case,files,rep),g in sorted(groups.items()):
        assert set(g)=={'off','on'}
        row=dict(kind=kind,case=case,files=files,rep=rep)
        for key in ['wall_ms','inner_ms','prepare_ms','cleanup_ms','spawn_ms']:
            if key in g['on']:row[key+'_overhead']=g['on'][key]-g['off'][key]
        row['outside_inner_ms_overhead']=row['wall_ms_overhead']-row['inner_ms_overhead']
        pairs.append(row)
    return pairs


def sandbox_summary(rows):
    out=[];pairs=sandbox_pair_rows(rows)
    for key in sorted({(r['kind'],r['case'],r['files']) for r in rows}):
        g=[r for r in pairs if (r['kind'],r['case'],r['files'])==key]
        r=dict(zip(['kind','case','files'],key));r.update(pairs=len(g),on_slower=sum(v['wall_ms_overhead']>0 for v in g))
        for field in g[0]:
            if field.endswith('_overhead'):
                values=[v[field] for v in g];r[field]=dict(median=statistics.median(values),min=min(values),max=max(values))
        for mode in ['off','on']:
            v=[x['wall_ms'] for x in rows if x['phase']=='evaluation' and (x['kind'],x['case'],x['files'],x['mode'])==(*key,mode)]
            r[mode+'_wall_median_ms']=statistics.median(v)
        out.append(r)
    return out


def data_products(evidence):
    catalog, raw, metrics = [], [], []
    for eid, stem, title, level, expected, directory in SPECS:
        date = evidence_date(eid)
        relative = f'docs/evidence/{stem}-{date}.json'
        rows = evidence[eid].get('rows', [])
        catalog.append({'id': eid, 'title': title, 'evidence_level': level, 'file': relative,
                        'sha256': sha(ROOT/relative), 'raw_records': len(rows),
                        'metal_auxiliary_records':len(evidence[eid].get('metal_commands',[])),
                        'record_kinds': dict(collections.Counter(r.get('kind', 'timing') for r in rows)),
                        'official_run': 'build/'+directory if directory else None,
                        'scope': evidence[eid].get('scope'),
                        'limits': evidence[eid].get('limits', []),
                        'historical_source_hashes_describe_the_measured_revision': True})
        if eid=='E55':catalog[-1].update(later_identified_confound='E56 found implicit deployment target26.0 outside versus10.4 inside. Compiler/SDK hashes and outputs alone did not prove equal target semantics; retain historical values, use fixed-target E56/E57 for new comparisons.')
        if eid=='E52':catalog[-1].update(superseded_by='E53',post_acceptance_gap='Parent startup environment remained visible via KERN_PROCARGS2; corrected with verified host protection in E53')
        groups = collections.defaultdict(list)
        for i, record in enumerate(rows):
            raw.append({'experiment': eid, 'source': relative, 'record_index': i, 'record': record})
            row = dict(record)
            row.setdefault('kind', 'timing')
            if row['kind'] in ('metadata', 'device', 'correctness', 'closed', 'unit'):
                continue
            if row['kind'] == 'memory':
                row['heap_extra_bytes'] = max(0, row['sampled_heap_peak']-row['heap_before'])
            dimensions = {k: row[k] for k in DIMS if k in row}
            if eid in ('E52','E53'):dimensions.update(case=row['case'],files=row['files'])
            if eid in ('E54','E55','E56','E57'):dimensions.update(case=row['case'])
            if eid=='E57':dimensions['units']=row['units']
            groups[json.dumps(dimensions, sort_keys=True)].append(row)
        for dimensions, group in sorted(groups.items()):
            dimensions = json.loads(dimensions)
            common = set.intersection(*(set(r) for r in group))
            for key in sorted(common-set(DIMS)-({'case','files','units'} if eid in ('E52','E53','E54','E55','E56','E57') else set())-{'rep','task','seq'}):
                values = [r[key] for r in group]
                if not all(isinstance(x, (int, float)) and not isinstance(x, bool) for x in values):
                    continue
                if key.endswith('_ns') or key in ('epoch','estimate_key'):
                    continue  # Absolute monotonic timestamps remain in raw records only.
                unit = (('bytes/s' if group[0].get('mode')=='copy_chain' else 'FLOP/s') if key=='effective_rate' else
                        'FLOP/s' if key.endswith('flops_s') else 'bytes/s' if key.endswith('bytes_s') else
                        'FLOP' if key in ('flops','dominant_flops') else
                        'ms' if key.endswith('_ms') else 'us' if key.endswith('_us') else
                        'bytes' if ('bytes' in key or key in ('heap_before','heap_after','sampled_heap_peak',
                                                          'rss_before','rss_after','payload_capacity','resident_before_clear','resident_after_clear')) else
                        'tokens' if 'tokens' in key or key in ('stored','logical','tokenized','evaluated') else
                        'count' if key.endswith('calls') or key in ('loops','checksum','branches','pending_peak','backpressure') else
                        'scalar')
                metrics.append({'experiment': eid, 'dimensions': json.dumps(dimensions, sort_keys=True, separators=(',', ':')),
                                'metric': key, 'unit': unit, 'n': len(values),
                                'median': statistics.median(values), 'min': min(values), 'max': max(values), 'source': relative})
    return catalog, raw, metrics


def validation_rows(evidence):
    rows = []
    def add(category, case, result, source, detail):
        rows.append(dict(category=category, case=case, result=result, evidence=source, detail=detail))
    for case in evidence['E58']['validation']['controller_sanitizers']['cases']:
        add('security','E58:'+case,'PASS','E58:validation.controller_sanitizers','Single fs.read candidate; exact args/epoch/file-version adoption; current file/content policy; no commands/network')
    for case in evidence['E58']['validation']['native_sanitizers']['cases']:
        add('functional','E58:'+case,'PASS','E58:validation.native_sanitizers','Real0.6B KV/template correctness only; single-owner probe; no hit-rate/speed claim')
    add('pending','E58:target_model_quality_and_performance','NOT_MEASURED','E58:limits','WSL2/CUDA and larger-model off/on tasks pending; failures/negative pairs retained by prepared runner')
    add('negative','E58:tiny_model_budget','RETAINED','E58:validation.model_negative_smoke','One sanitized0.6B synthetic pair: both main branches exhaust256 tokens with equal output; on predicts/reads but cancels without publication. Not a hit-rate or speed estimate.')
    for case in evidence['E57']['validation']['recipe_sanitizers']['suites']:
        add('security','E57:'+case,'PASS','E57:validation.recipe_sanitizers','Private one-task PCH lifecycle; cold preparation included; no new permissions or skipped validation')
    add('negative','E56:E55_implicit_target','CONFOUNDED','E56:implicit_target_identity','Old E55 default deployment target26.0 off versus10.4 on; old evidence unchanged, new controls explicitly use26.0')
    add('performance','E56:compiler_header_split','MEASURED','E56:rows;pairs','270 fixed-target stage/cache controls; header opens versus preprocessed input; default PCM absent')
    add('negative','E56:cold_caches','RETAINED','E56:rows','Module and stat-cache cold costs retained; private PCH single-use versus three-use costs separated')
    add('security','E57:group_candidate','PASS','E57:validation.candidate_os;group_equivalence','Experimental generated6-group union passes15 OS suites and differential corpus; not a new grant')
    add('performance','E57:new_tasks','MEASURED','E57:rows;pairs','180 actual CLI tasks,4 new cases,5 modes,2 warmups+7 evaluation blocks; no historical benefit accumulation')
    for case in evidence['E55']['validation']['sandbox_sanitizers']['suites']:
        add('security','E55:'+case,'PASS','E55:validation.sandbox_sanitizers','Explicit environment binding grants no files/network; original boundary and parent protection remain')
    add('sanitizer','E55:compatibility','PASS','E55:validation','561 C,15 OS,12 policy,8 foundation,14K Native,16 Stream,72 output; upstream libraries not instrumented')
    add('performance','E54:cache_and_launch','MEASURED','E54:rows;pairs;ipc_rows','126 main plus70 direct-API rows; cache invalidation/reuse causal control; IPC totals not full xcodebuild waiting')
    add('negative','E54:nolog_and_sdk_only','RETAINED','E54:rows;development_failures','No-log does not remove seconds-scale regression; SDKROOT alone insufficient; sample produced no call stacks')
    add('performance','E55:fresh_tasks','MEASURED','E55:rows;pairs','108 actual CLI tasks,3 new workloads,4 modes,2 warmups+7 evaluation blocks; unchanged permissions, explicit binding only')
    for case in evidence['E53']['validation']['sandbox_sanitizers']['suites']:
        add('security','E53:'+case,'PASS','E53:validation.sandbox_sanitizers','Final OS file/network policy and cxf startup-environment protection; explicit off positive control retained')
    add('sanitizer','E53:native_and_process','PASS','E53:validation','542 C checks, prior12/8 suites,14K Native,16 Stream and72 output checks; fixed upstream libraries not instrumented')
    add('negative','E52:parent_environment_gap','SUPERSEDED','E53:host_environment_control','E52/narrow-sysctl-only still exposed synthetic parent environment. E53 verifies host redaction before enabling restricted commands; other same-UID hosts remain outside the boundary.')
    add('performance','E53:final_matched_overhead','MEASURED','E53:rows;pairs','Current runtime230 rows /93 pairs, cost-positive; E52 not relabeled as final performance')
    for case in evidence['E52']['validation']['sandbox_sanitizers']['suites']:
        add('security','E52:'+case,'PASS','E52:validation.sandbox_sanitizers','OS file/network restriction, explicit grants, links/fd/descendant/rename protection; synthetic files and local receivers')
    add('sanitizer','E52:policy_and_process','PASS','E52:validation.c_sanitizers','542 rule/fd/lifecycle checks; existing445 policy checks and prior suites retained')
    add('sanitizer','E52:native_regression','PASS','E52:validation.native_sanitizers','14K,16 Stream and72 output cases; deterministic CLT Stream fixture, actual timeout receipts independently checked')
    add('negative','E52:loopback_candidate','REJECTED','E52:loopback_negative_control','localhost SBPL candidate also admitted a local non-loopback interface; removed, not shipped')
    add('performance','E52:matched_overhead','MEASURED','E52:rows;pairs','230 rows including44 warmups;93 evaluation pairs; cost-positive on-minus-off, all negative values retained')
    for case in evidence['E51']['validation']['http_sanitizers']['suites']:
        add('security', 'E51:'+case, 'PASS', 'E51:validation.http_sanitizers', 'Synthetic secrets, real loopback HTTP/files/processes; expected refusals and normal tool loop; no universal DLP/sandbox claim')
    add('sanitizer', 'E51:protected_files_and_egress', 'PASS', 'E51:validation.policy_sanitizers', '445 file/path/secret/scope/capacity checks under ASan/UBSan')
    add('sanitizer', 'E51:foundation_regression', 'PASS', 'E51:validation.foundation_sanitizers', 'Eight prior foundation suites revalidated with explicit local scope and credential-safe inheritance marker')
    add('sanitizer', 'E51:native_regression', 'PASS', 'E51:validation.native_sanitizers', '14K, Stream and output compatibility including Native input/prefill secret rejection; fixed upstream libraries not instrumented')
    for case in evidence['E50']['validation']['http_sanitizers']['suites']:
        add('functional', 'E50:'+case, 'PASS', 'E50:validation.http_sanitizers', 'Scripted primary/local HTTP providers; real files/processes; no model quality or speed claim')
    add('sanitizer', 'E50:guarded_edit_and_environment', 'PASS', 'E50:validation.basic_sanitizers', 'ASan/UBSan file editing and clean/inherit fork/spawn process checks')
    add('sanitizer', 'E50:native_regression', 'PASS', 'E50:validation.native_sanitizers', '14,357-token Native, 16 Stream cases and72 output cases; prebuilt upstream libraries not instrumented')
    add('implementation', 'native_context', 'PASS', 'E03:runs', 'Shared tokens, revisions, rollback, OOM, lifetime, independent libllama reference, capacity retry')
    add('implementation', 'chat_tokens_and_actions', 'PASS', 'E04:runs; E09:validation', 'Upstream token oracle and parser; 256 byte/escape corpora; Chinese tokenizer retry')
    add('implementation', 'long_context_14357_tokens', 'PASS', 'E11:validation.native_chat_sanitized_output', 'Independent full upstream tokenization and cached greedy result')
    add('executor', 'scripted_read_fail_fix_pass_read', 'PASS', 'E04:runs', 'Real tools and model prefill; action selection supplied by test script')
    for eid, name in [('E10', 'kv_worker'), ('E11', 'stream_pipeline')]:
        for row in samples(evidence, eid, kind='correctness'):
            for case in row['cases']:
                add('implementation', name+':'+case, 'PASS' if row['passed'] else 'FAIL', eid+':rows.correctness', 'Functional acceptance, not a performance sample')
    for key in ('replay_cli', 'native_cli'):
        data = evidence['E11']['validation'][key]
        cases = data.get('results', []) if isinstance(data, dict) else data
        for case in cases:
            add('cli', key+':'+case['case'], 'PASS' if case['passed'] else 'FAIL', 'E11:validation.'+key,
                'Expected exit='+str(case['exit'])+'; expected rejection can pass with a nonzero exit')
    add('regression', 'default_make_test', 'PASS', 'E11:validation.default_regression', 'Recorded at the measured revision; no inference benchmark rerun during report consolidation')
    add('sanitizer', 'native_chat_kv_stream', 'PASS', 'E11:validation', 'ASan/UBSan on cxf/adapters; precompiled llama/common/Metal libraries not instrumented')
    add('implementation','cost_estimator','PASS','E13:validation.unit','Profile isolation, invalid state atomicity, unseen synthetic workloads, bounded window and speed-drift correction')
    add('sanitizer','cost_estimator_asan_ubsan','PASS','E13:validation.sanitizers','Standalone C estimator and unit test')
    add('regression','default_make_test_after_cost','PASS','E13:validation.default_regression','Current estimator change; local loopback enabled, no GPU test running concurrently')
    add('implementation','cost_real_model_21_shapes','PASS','E13:rows.correctness','Canonical IDs, fresh-KV greedy, exact evaluated token/batch counts; estimator never changes inference')
    add('implementation','roofline_estimator','PASS','E14:validation.unit','Hand-derived FLOPs/bytes, GQA, chunk logits, missing rates, atomic failures and calibrated unseen shapes; original cost regression')
    add('sanitizer','roofline_asan_ubsan','PASS','E14:validation.sanitizers','C cost/roofline and unit tests')
    add('regression','default_make_test_after_roofline','PASS','E14:validation.default_regression','Completed before GPU performance run; local loopback enabled')
    add('implementation','roofline_real_model_21_shapes','PASS','E14:rows.correctness','Fresh-KV greedy, canonical tokens, exact token/batch counts; microprobe output verified')
    add('implementation','roofline_diagnosis','PASS','E15:validation.unit','Last-layer FFN pruning hand counts, original semantics and failure atomicity; E14 147-row independent arithmetic audit')
    add('sanitizer','roofline_diagnosis_asan_ubsan','PASS','E15:validation.sanitizers','C cost/roofline unit suites')
    add('regression','default_make_test_after_diagnosis','PASS','E15:validation.default_regression','Completed before performance; estimator only, inference/scheduling unchanged')
    add('implementation','diagnosis_real_model_21_shapes','PASS','E15:rows.correctness','Fresh-KV greedy and token/batch oracles; dependent primitive output checks and separated depth ranges')
    for eid in ['E16','E17']:
        add('implementation',eid+':shape_estimator','PASS',eid+':validation.unit','Interpolation hand counts, kernel boundary, advancing chunk context, extrapolation and atomic failures')
        add('sanitizer',eid+':shape_asan_ubsan','PASS',eid+':validation.sanitizers','C cost/roofline unit suites')
        add('regression',eid+':default_make_test','PASS',eid+':validation.default_regression','Completed before GPU timing')
        add('implementation',eid+':60_fresh_shapes','PASS',eid+':rows.correctness','Two batch limits, fresh KV greedy, canonical tokens and token/batch checks; fair prequential replay')
    add('implementation','E19:residual_calibration','PASS','E19:validation.unit','Analytic normalized ridge, collinear/zero features, signed correction, window/drift, profile and failure atomicity')
    add('sanitizer','E19:residual_asan_ubsan','PASS','E19:validation.sanitizers','Residual and shape estimator C unit suites')
    add('regression','E19:default_make_test','PASS','E19:validation.default_regression','Completed before GPU timing')
    add('implementation','E19:78_fresh_shapes','PASS','E19:rows.correctness','Two unseen batch limits, equal-label/interleaved coverage, new context/token shapes, canonical IDs/greedy/batch counts')
    add('implementation','E20:kalman','PASS','E20:validation.unit','Analytic gain/Joseph covariance, read-only prediction, EWMA equivalence, step and spike propagation, profile/range/atomic failure')
    add('sanitizer','E20:kalman_asan_ubsan','PASS','E20:validation.sanitizers','Standalone C Kalman tests')
    add('regression','E20:default_make_test','PASS','E20:validation.default_regression','Completed before native timing')
    add('implementation','E20:78_fresh_shapes','PASS','E20:rows.correctness','Fresh context/token grid, token/greedy/batch oracles; filter C replay checked independently in Python')
    add('implementation','E21:actual_tool_flow','PASS','E21:rows.correctness','Single owner, borrowed Native, cancellation/drain, real read and fail/repair/pass tools, canonical IDs and fresh greedy reference')
    add('sanitizer','E21:native_asan_ubsan','PASS','E21:validation.native_sanitizers','Tool preparation + existing Native/14K/KV/Stream regression; measured source hashes retained')
    add('regression','E21:default_make_test','PASS','E21:validation.default_regression','Completed before performance run')
    add('implementation','E21:cli_integration','PASS','E21:validation.cli_integration','Explicit native CLI option wraps registered test.run; model capability is separate')
    probe=evidence['E21']['validation']['model_cli_probe']
    add('model_capability','E21:native_cli_tool_probe','FAIL' if not probe['passed'] else 'PASS','E21:validation.model_cli_probe','Single0.6B probe emitted bare JSON as prose; no actual tool executed. Preserved separately from deterministic CLI and timing')
    add('implementation','E22:stage_trace','PASS','E22:validation','Same-clock parent process trace, exact adjacent-stage partition and old-strategy controls; past-label ETA replay')
    add('sanitizer','E22:stage_trace_asan_ubsan','PASS','E22:validation','Tool/KV acceptance and trace executor semantics before timing')
    add('implementation','E27:resident_multitool','PASS','E27:validation','Six actual read/test-fail/write/test-pass/read/test-pass calls; resident prefixes, direct vs wrapper,12 fresh references; save/evict/restore all zero')
    add('implementation','E28:resident_real_process_stream','PASS','E28:validation','12 complete units from real CPU/file process without sleep;84 canonical/greedy task checks,4 fresh references after normal close, no snapshots')
    add('validation_setup','E28:initial_oracle_capacity','FAIL','E28:validation.initial_qualification_failure','Diagnostic second full KV reference exceeded shared8192 capacity;9 completed sanitizer tasks excluded; reference moved after normal close without workload/capacity change')
    add('sanitizer','E27_E28:normal_resident','PASS','E27/E28:validation','Final flow/stream ASan; Native14K, existing Stream and default regressions; no snapshot/evict test required this round')
    add('implementation','E26:main_launch_and_incremental','PASS','E26:validation','New main/compute bounds, same-artifact rerun with matching exit, real correction rebuild, unchanged-source reuse, three fresh-KV reference prompts')
    add('sanitizer','E26:new_task_driver','PASS','E26:validation.sanitizers','ASan/UBSan new workload/driver; same production runtime as E25 prior full regressions')
    add('implementation','E25:unlink_hold','PASS','E25:validation','Owner-only retained fd; snapshot slot/budget held until last close; live-ticket and duplicate rejection, stale restore refusal, cancellation and final cleanup')
    add('sanitizer','E25:resource_phase_driver','PASS','E25:validation','ASan/UBSan Native14K/KV/Stream and actual CLI once plus default regression before isolated timing')
    add('implementation','E24:early_snapshot_release','PASS','E24:validation','Ticket drained before early drop; timer/inflight/ready cancellation, tool exception, snapshot reuse, token and fresh-greedy oracle')
    add('sanitizer','E24:asan_ubsan_and_cli','PASS','E24:validation','Native/14K/KV/Stream; actual CLI early release executes tool once; default regression passed before timing')
    add('implementation','E23:progress_and_spawn','PASS','E23:validation','Epoch/duplicate/time-bound progress, no-event fallback, cancellation, Native token/fresh-greedy oracle, one tool execution')
    add('sanitizer','E23:asan_ubsan_legacy','PASS','E23:validation.sanitizers','Current progress/spawn plus Native/14K/KV/Stream and CLI routing')
    add('regression','E23:default_make_test','PASS','E23:validation.default_regression','New macOS spawn backend: stdout/stderr/cwd/timeout/process group and existing regression before timing')
    add('implementation','E29:actual_cli_output','PASS','E29:rows; validation','Real CLI/store/test.run,192 actual tool invocations,256 full-token oracles, exact off/on transcript and canonical hashes; scripted actions, resident KV')
    add('sanitizer','E29:output_native_stream','PASS','E29:validation','Output13 cases, CLI16 tasks, Native14357-token and original Stream ASan/UBSan; default regression')
    probe=evidence['E29']['validation']['production_model_probe']
    add('model_capability','E29:production_native_tool_probe','PASS' if probe['passed'] else 'FAIL','E29:validation.production_model_probe','Single0.6B production completion failed Native format validation (rc7); zero actual tools; separate from deterministic-choice CLI timing')
    add('validation_setup','E29:initial_cli_qualification','FAIL','E29:validation.initial_qualification_failure','Initial CLI newline changed first user message and safely invalidated prefix; first collector expected tool-only executed count; failures preserved, no performance samples')
    add('implementation','E31:matched_isolation','PASS','E31:validation','13 ASan unit cases;40 CLI tasks; thread/token controls preserve final canonical/greedy')
    add('implementation','E33:process_attribution','PASS','E33:validation','Child CPU/wall, EOF waits, callback and worker observations; matched controls')
    add('implementation','E36:contention','PASS','E36:validation','13 native sanitizer cases and40 actual CLI tasks; same growth gate and CPU/decode observations')
    add('implementation','E38:decode_scheduler','PASS','E38:validation','22 sanitizer cases,32 real CLI tasks, Mach unit calibration and decode/sync accounting')
    add('implementation','E40/E41:idle_source','PASS','E40:validation;E41:validation','32 model fixtures,96 actual tools and384 producer controls; model CPU sampled without other PID inventory')
    add('implementation','E42/E43:metal_timeline','PASS','E42:validation;E43:validation','30 native sanitizer cases and48 actual CLI tasks each; per-buffer timestamps, completion-marker and bare controls; diagnostics only')
    add('implementation','E47/E48/E49:isolation_quantum','PASS','E47:validation;E48:validation;E49:validation','Same-capacity CLI controls, pressure/fresh-greedy isolation and larger-quantum timeout/cancel checks; final14K/Stream/default acceptance')
    add('implementation','E45/E46:tail_capacity','PASS','E45:validation;E46:validation','Matched device/notification controls and rotating fresh capacity validation; all negative pairs and allocation costs retained')
    add('implementation','E44:tail_coalescing','PASS','E44:validation','Budget, default reset, canonical/greedy, actual CLI and long-context/Stream/default acceptance; strict synchronized public returns')
    add('implementation','E39:deferred_sync','PASS','E39:validation','30 sanitizer cases,32 real CLI tasks; 14,357-token deferred native and full strict Stream/default regression')
    add('implementation','E37:lazy_consumer','PASS','E37:validation','22 eager/lazy sanitizer cases and24 actual CLI tasks; no eligible burst growth means no consumer')
    add('implementation','E35:exit_race','PASS','E35:validation','Bounded 1ms retry only after ESRCH or NOTE_EXIT; original modes retained')
    add('implementation','E34:exit_event','PASS','E34:validation','Optional macOS NOTE_EXIT wait; timeout/once semantics and six-mode actual CLI checks')
    add('implementation','E32:growth_gate','PASS','E32:validation','13 ASan cases;24 CLI tasks;42 evaluated burst tools perform zero speculative updates; default regression passed')
    probes = evidence['E04']['capability_probes']
    for case in probes['results']:
        if case['case'].startswith('real-'):
            add('model_capability', case['case'], 'PASS' if case['passed'] else 'FAIL', 'E04:capability_probes', case['scope']+'; n=1')
    for case in evidence['E12']['cases']:
        add('historical_model_capability', case['id'], 'FAIL', 'E12:cases', case['observation']+'; n=1; raw logs private')
    for name, detail in [
        ('native_windows_cuda', 'No build or performance acceptance on the second machine'),
        ('live_cloud_sse', 'Controlled replay only; no provider-specific SSE integration'),
        ('general_agent_scheduling', 'E21 opt-in serial native test.run wrapper only; no general multi-consumer, HTTP/cloud or cold-start scheduler'),
        ('physical_kv_buffer_release', 'Logical cells reusable, preallocated KV buffer size unchanged'),
        ('cold_ssd_and_tail_latency', 'Warm OS cache; n=7 does not support p95/p99 or stability claims'),
        ('general_coding_or_summary_quality', '0.6B used as compatibility vehicle, no capability benchmark')]:
        add('not_validated', name, 'NOT_MEASURED', 'E10:limits; E11:limits', detail)
    return rows


def inventory(evidence):
    """Metadata only: never import private config, argv, model paths or traces."""
    official = {directory: eid for eid, _, _, _, _, directory in SPECS if directory}
    candidates = []
    for folder in sorted((ROOT/'build').iterdir()):
        if not folder.is_dir() or not folder.name.startswith(('llama-context-', 'llama-native-', 'native-chat-',
                'native-template-', 'context-memory-', 'direct-messages-', 'jinja-values-', 'kv-worker-',
                'stream-', 'cost-estimator-', 'roofline-estimator-', 'roofline-diagnose-', 'cost-shapes-', 'frozen-calibration-', 'kalman-cost-', 'tool-prepare-', 'tool-timing-', 'tool-taskcost-', 'tool-isolate-', 'tool-launch-', 'resident-', 'output-', 'tool-output-', 'model-idle-', 'device-isolation-', 'restore-timing-', 'restore-balance-', 'basic-agent-live-', 'qwen-basic-')):
            continue
        files = sorted(p.name for p in folder.iterdir() if p.is_file() and
                       (p.suffix == '.jsonl' or p.name in ('manifest.json','results.json','report.json') or p.name.endswith('-summary.json')))
        if folder.name in official:
            status, reason = 'official', 'Preserved in public evidence '+official[folder.name]
        elif 'sanitized' in folder.name or '-cli' in folder.name:
            status, reason = 'validation_or_probe', 'Correctness/CLI evidence; not extra independent performance samples'
        elif folder.name.startswith(('qwen-basic-', 'basic-agent-live-')):
            status, reason = 'historical_capability_probe', 'Failure summarized in E12; private raw logs/config excluded'
        else:
            status, reason = 'development_or_superseded', 'Kept out of final aggregates; metadata inventory only'
        if folder.name.startswith(('output-metal-','tool-output-metal-','output-notify-','tool-output-notify-','output-coalesce-')) and folder.name not in official:
            status,reason='development_or_superseded','Metal observer development or qualification; not extra performance samples; build failures and successful qualification described in CHANGELOG'
        metal_failures={
            'output-metal-qualified-20261031':'ObjC runtime Method typedef collided with local alias; compilation failed',
            'output-metal-qualified2-20261031':'Alias rename also changed ObjC method lookup names; compilation failed',
            'tool-output-metal-final-20261031':'Alias rename also changed ObjC method lookup names; compilation failed',
            'output-metal-qualified5-20261031':'Weak observer linkage failed in ordinary native CLI; replaced by explicit per-engine callback',
            'tool-output-metal-final4-20261031':'Weak observer linkage failed in ordinary native CLI; no performance',
            'output-notify-qualified-20261031':'Metadata JSON string escaping compile error; no measurements',
            'tool-output-notify-final-20261031':'Metadata JSON string escaping compile error; no measurements'}
        if folder.name in metal_failures:status,reason='validation_failed',metal_failures[folder.name]
        if folder.name in ('output-metal-qualified6-20261031','output-notify-qualified2-20261031','output-coalesce-qualified-20261031'):
            status,reason='validation_or_probe','ASan/UBSan unit and48 actual CLI tasks, excluded from performance; source/archive hashes recorded separately'
        if folder.name in ('output-contention640-qualified-20261031','output-device-isolation-qualified-20261031','output-coarse640-qualified-20261031','output-coarse640-long-20261031','output-coarse640-stream-20261031'):
            status,reason='validation_or_probe','E47-E49 SAN/fresh-greedy/CLI/long/Stream acceptance, not performance samples'
        if folder.name in ('output-tailwait-qualified-20261031','output-capacity-qualified-20261031','output-capacity-long-20261031','output-capacity-stream-20261031'):
            status,reason='validation_or_probe','E45/E46 sanitizer or actual capacity/long/Stream acceptance, excluded from performance'
        if folder.name in ('output-coalesce-long-20261031','output-coalesce-stream-20261031'):
            status,reason='validation_or_probe','Final E44 long-context coalescing or strict Stream sanitizer acceptance, not performance samples'
        if folder.name in ('output-decode-qualified-20261031','tool-output-decode-final-20261031'):
            status,reason='validation_failed','Private header include path missing; compilation failed; no measurements; corrected in qualified2/final2'
        if folder.name in ('output-reap-qualified-20261031','tool-output-reap-final-20261031'):
            status,reason='validation_failed','Darwin CLOCK_MONOTONIC_RAW hidden by POSIX feature macro; compile failed; no measurements; corrected with _DARWIN_C_SOURCE'
        elif folder.name in ('output-reap-qualified2-20261031','output-event-qualified-20261031','output-race-qualified-20261031','output-contention-qualified-20261031','output-lazy-qualified2-20261031','output-decode-qualified2-20261031','output-drain-qualified-20261031','output-drain-long-20261031'):
            status,reason='validation_or_probe','ASan/UBSan unit and actual CLI policy checks; excluded from performance'
        if folder.name == 'output-qualified-20261031':
            status,reason='validation_failed','Sandbox Metal unavailable; local13 unit cases passed; initial actual CLI collector count assertion failed and first-user newline invalidated prefix; retained outside timing'
        elif folder.name == 'output-model-cli-20261031':
            status,reason='model_capability_probe','Production0.6B Native format error rc7; zero actual tools; retained separately from fixed-choice CLI execution/timing'
        elif folder.name in ('output-qualified2-20261031','output-qualified3-20261031'):
            status,reason='validation_or_probe','Output correctness/ASan, actual CLI, Native14K and Stream acceptance; excluded from performance'
        elif folder.name == 'resident-qualified-20261031':
            status,reason='validation_failed','First streaming fresh-KV oracle exceeded shared8192 slot capacity;9 sanitizer task rows completed, excluded from performance; final oracle follows normal stream close'
        elif folder.name == 'tool-timing-qualified-20261031':
            status,reason='build_failed','Collector used an unavailable common_json method; no native timing; kept private compile evidence'
        elif folder.name in ('tool-timing-qualified2-20261031','restore-balance-qualified-20261031','restore-balance-cli-20261031','tool-taskcost-qualified-20261031','tool-taskcost-cli-20261031','tool-isolate-qualified-20261031','tool-isolate-cli-20261031','tool-launch-qualified-20261031','resident-qualified2-20261031','resident-qualified3-20261031'):
            status,reason='validation_or_probe','Stage/progress/spawn sanitizer or deterministic CLI acceptance, not performance samples'
        if folder.name == 'tool-prepare-dev-20261031':
            status,reason='build_failed','Initial collector C/C++ linkage failure before native run; compile artifacts retained, no performance samples'
        elif folder.name == 'tool-prepare-dev2-20261031':
            status,reason='development_or_superseded','First real-tool acceptance; final cancellation cutoff and collector validation are in later qualification'
        elif folder.name in ('tool-prepare-qualified-20261031','tool-prepare-qualified2-20261031','tool-prepare-cli-qualified-20261031'):
            status,reason='validation_or_probe','ASan/UBSan qualification; full Native/14K/KV/Stream and final tool/CLI checks are separate, not performance'
        elif folder.name == 'tool-prepare-cli-20261031':
            status,reason='model_capability_probe','0.6B emitted bare JSON as prose; zero actual tool calls, preserved in E21 validation'
        elif folder.name == 'tool-prepare-cli-scripted-20261031':
            status,reason='validation_or_probe','Deterministic Native-parser CLI/registry/AgentStore test: actual test command executed exactly once'
        if folder.name == 'stream-final-20261030':
            reason = 'Superseded: an extra EOF interval distorted the input window; corrected in E11'
        if folder.name == 'stream-final2-20261030':
            reason = 'Incomplete compilation attempt; no formal performance samples'
        if folder.name == 'cost-estimator-final-20261030':
            reason = 'Sandbox prevented Metal initialization; no successful timing samples'
        if folder.name == 'cost-estimator-final2-20261030':
            reason = 'Superseded: wrapper postprocessing failed and a default regression overlapped; not included in E13'
        if folder.name == 'roofline-diagnose-probe-20261030':
            reason = 'Development only: nonzero n_kv_max selected sparse attention; wrong kernel for dense llama, excluded from E15'
        if folder.name == 'roofline-diagnose-probe2-20261030':
            reason = 'Development dense-layout/primitive check; no inference evaluation; excluded from formal E15 statistics'
        if folder.name == 'frozen-calibration-final-20261030':
            reason = 'Incomplete E18: cap256 completed; cap64 rejected before measurement because default prefill minimum exceeded batch limit. Full raw retained; not formal E19 results'
        if folder.name == 'cost-shapes-final-20261030':
            reason = 'Official E16; wrapper missing re import after native success; metadata recovered from unchanged raw, no inference rerun; interpolation negative result retained'
        counts = {}
        for filename in files:
            if filename.endswith('.jsonl'):
                counts[filename] = sum(bool(line.strip()) for line in (folder/filename).read_text().splitlines())
        candidates.append({'directory': 'build/'+folder.name, 'classification': status, 'reason': reason,
                           'artifact_names': files, 'jsonl_record_counts': counts})
    return {'scope': 'Local benchmark/probe directories present at consolidation; metadata only, not an exhaustive execution log',
            'private_artifacts_excluded': ['config.json', 'request/response traces', 'compiler argv', 'model paths', 'engine logs'],
            'runs': candidates}


def metal_report(e,zh):
    def tr(en,cn):return cn if zh else en
    def cell(g,f):
        xs=[r[f] for r in g];return f"{statistics.median(xs):.3f} [{min(xs):.3f},{max(xs):.3f}]"
    lines=['## '+tr('E42–E44 · GPU execution, notification and bounded batch coalescing','E42–E44 · GPU执行、通知与有限尾批合并'),'',
      tr('Three independent experiments,1539 main CLI records each. E42/E43 repeat43/53-word,0/16M-iteration workloads with device tracing, observer-off and completion-marker controls. E44 uses fresh45/51-word,0/14M workloads for bounded batch coalescing. KV stays resident; no snapshot, eviction or build optimization. All actions are scripted Native-parser choices with real CLI/tools and exact token/greedy checks.',
         '三个独立实验各1539条主CLI记录。E42/E43在43/53词、0/1600万循环同负载测设备时序、关闭observer与完成marker；E44换45/51词、0/1400万循环验证有限尾批合并。KV常驻，无快照／逐出／构建优化；动作选择脚本控制，CLI／工具执行和token／greedy校验均真实。'),'',
      tr('The old E39 worst44.039ms pair contains23.529ms extra producer wall and18.720ms extra final prefill. It has no Metal trace: new experiments cannot retrospectively assign its exact milliseconds. New waits can now be localized. GPU command-buffer intervals are unioned, not summed, and are execution spans rather than pure shader busy counters.',
         '旧E39最差44.039ms对包含producer墙钟多23.529ms、最终prefill多18.720ms，没有Metal trace，不能用新实验追溯精确分配旧44ms。新样本可以直接定位等待。多buffer的GPU区间取并集，不相加；表示命令执行跨度，不是纯shader忙时。'),'',
      '![Metal timeline](figures/metal-timeline.png)','',
      table(['E42 strict off','Iterations','GPU union ms','Sync GPU overlap ms','Pre-GPU gap ms','Post-GPU return upper lag ms','Uncovered commit-to-start ms'],[
        [w,i,*[cell([t for t in e['E42']['metal_tasks'] if (t['words'],t['producer_iterations'],t['mode'])==(w,i,'off-race')],f) for f in ['metal_gpu_union_ms','metal_sync_gpu_overlap_ms','metal_sync_preexec_gap_ms','metal_sync_return_lag_ms','metal_queue_no_own_gpu_ms']]] for w in [43,53] for i in [0,16000000]]),'',
      tr('The strict-off synchronization is predominantly observed GPU execution. Uncovered submission delay and host return tails remain separate. E42 strict-off has a14.006ms single-wait return tail; all traced modes reach57.657ms. Large commit-to-start totals mostly overlap earlier own GPU work; uncovered time still mixes CPU driver, unobserved work, device queues and system scheduling. These APIs cannot attribute exact external-GPU queue occupancy or kernel-level CPU wakeup.',
         'strict关闭组同步时间主要与实际GPU执行重叠；未解释提交延迟和host返回尾部分开。E42 strict关闭组单次wait返回尾部最高14.006ms，全部带trace模式最高57.657ms。大的commit→start累计主要与自己前序GPU工作重叠；排除后的剩余仍混合CPU driver、未捕获工作、设备排队及系统调度。这些API不能精确归属其他应用的GPU队列占用或内核级CPU唤醒。'),'',
      tr('Apple documents [GPUStartTime](https://developer.apple.com/documentation/metal/mtlcommandbuffer/gpustarttime?language=objc) as device execution timing, [kernelStartTime](https://developer.apple.com/documentation/metal/mtlcommandbuffer/kernelstarttime?language=objc) as CPU driver scheduling, and [waitUntilCompleted](https://developer.apple.com/documentation/metal/mtlcommandbuffer/waituntilcompleted%28%29?language=objc) as waiting for GPU completion and completion-handler return. E43 adds a separately controlled timestamp-only handler; neither delivery nor post-handler return is pure CPU wakeup.',
         'Apple将[GPUStartTime](https://developer.apple.com/documentation/metal/mtlcommandbuffer/gpustarttime?language=objc)定义为设备执行时间，[kernelStartTime](https://developer.apple.com/documentation/metal/mtlcommandbuffer/kernelstarttime?language=objc)为CPU驱动调度；[waitUntilCompleted](https://developer.apple.com/documentation/metal/mtlcommandbuffer/waituntilcompleted%28%29?language=objc)还等待完成handler返回。E43另设可关闭的只记时间handler；通知交付和handler后的返回都不能称纯CPU唤醒。'),'',
      '![Completion notification](figures/metal-notification.png)','',
      table(['E43 off / marker','Iterations','GPUend to marker delivery ms','Marker end to wait return ms','Covered waits / missing'],[
        [w,i,cell(g,'metal_sync_completion_delivery_ms'),cell(g,'metal_sync_after_notify_return_ms'),str(sum(t['metal_sync_notify_waits'] for t in g))+' / '+str(sum(t['metal_sync_notify_missing_waits'] for t in g))]
        for w in [43,53] for i in [0,16000000] for g in [[t for t in e['E43']['metal_tasks'] if (t['words'],t['producer_iterations'],t['mode'])==(w,i,'off-race-notify')]]]),'',
      tr('53-word burst marker tasks reach8.584ms cumulative notification-delivery delay and1.329ms cumulative post-marker return, while median GPU overlap is655.283ms. Another E43 43-word growth burst wait has a16.376ms tail:10.728ms delivery and5.648ms after-marker return. This supports separating device work from host notification/scheduling, not relabeling hundreds of milliseconds as CPU wakeup. Bare-versus-observer and marker controls have both positive and negative task differences. E42 also contains multi-second producer/host outliers, including a3,637.772ms deferred-progress regression; none were removed. The cleaner E43 repeats do not erase E42.',
         '53词突发marker任务累计通知交付最高8.584ms、marker后返回最高1.329ms，GPU重叠中位655.283ms。另一个E43 43词增长突发wait的16.376ms尾部可分为10.728ms通知交付与5.648ms marker后返回。因此应分开设备工作与host通知／调度，不能把数百毫秒叫CPU唤醒。bare／observer及marker对照均有正负任务差；E42另有数秒级producer／host异常，包含deferred-progress反慢3637.772ms，全部保留，E43较平稳的复测不抹掉E42。'),'',
      table(['Experiment','Words','Iterations','Baseline','Candidate','Task saving median [min,max] ms','Faster /7'],[
        [eid,r['words'],r['producer_iterations'],r['baseline'],r['mode'],f"{r['task_ms']['median_saving']:.3f} [{r['task_ms']['min_saving']:.3f},{r['task_ms']['max_saving']:.3f}]",r['task_ms']['positive_pairs']]
        for eid in ['E42','E43'] for r in e[eid]['metal_contrasts'] if (eid=='E42' and r['mode'] in ['off-drain','progress-drain']) or (eid=='E43' and (r['mode'].endswith('-notify') or r['baseline'].endswith('-bare')))]),'',
      tr('E42 identifies genuine tail-batch work:588 tokens split512+76, with three76-token tail buffers taking28.665/33.070/36.619ms in one53-word burst task. E44 keeps an allocated1024 capacity in every control, caps normal chunks at512, and optionally fuses a remaining513–1024-token tail only if the whole tail fits the current budget. Streaming budget512 cannot be exceeded. Both controls use strict synchronization; the default CLI batch512 and default strategy remain unchanged.',
         'E42发现真实尾批成本：588 token拆为512＋76，某53词突发任务三个76-token尾批分别占28.665／33.070／36.619ms。E44所有对照均分配1024 batch容量，普通chunk cap512，仅当剩余513–1024 token完整落在本次预算内才合并；不能超出工具流式预算512。两组均strict同步，CLI默认batch512和默认策略保持原样。'),'',
      '![Tail coalescing](figures/tail-coalescing.png)','',
      table(['E44 words','Iterations','Baseline','Candidate','Task saving median [min,max] ms','Faster /7','Final prefill saving median ms'],[
        [r['words'],r['producer_iterations'],r['baseline'],r['mode'],f"{r['task_ms']['median_saving']:.3f} [{r['task_ms']['min_saving']:.3f},{r['task_ms']['max_saving']:.3f}]",r['task_ms']['positive_pairs'],f"{r['final_prefill_ms']['median_saving']:.3f}"] for r in e['E44']['metal_contrasts']]),'',
      tr('Bare coalescing saves36.407/28.445ms on45/51-word burst tasks,7/7 faster each. The45-word paced off group regresses3.341ms median and86.283ms worst: that pair has93.325ms more final prefill (84.062ms in sync), despite20.539ms less tool wall. Bare has no device timestamps, so this particular tail cannot be assigned to GPU versus host wakeup. Profiled51-word paced coalescing saves23.360ms device span and18.263ms final prefill but loses11.240ms whole-task median, with32.426ms extra tool wall. Device improvement therefore does not imply a universal task speedup. Coalesced growth versus off retains152.803/181.509ms paced task savings,7/7 each, but burst differences are+1.718/-2.125ms.',
         'bare合并在45/51词突发任务快36.407／28.445ms，均7/7。45词持续关闭组中位反慢3.341ms、最差86.283ms：该对最终prefill多93.325ms（其中sync多84.062ms），工具墙钟反而少20.539ms；bare没有设备时间戳，不能把这个尾部精确归为GPU还是host唤醒。带probe的51词持续组GPU跨度少23.360ms、最终prefill少18.263ms，但整任务中位反慢11.240ms，工具墙钟多32.426ms。因此设备改进不保证每组整任务提速。合并后的增长流式相对关闭仍在持续任务快152.803／181.509ms，均7/7；突发仅+1.718／-2.125ms。'),'',
      table(['E44 profiled off','Iterations','GPU span saving median ms','Final prefill saving median ms','Tool wall saving median ms','Whole task saving median ms'],[[r['words'],r['producer_iterations'],*[f"{r[f]['median_saving']:.3f}" for f in ['metal_gpu_union_ms','final_prefill_ms','tool_sum_ms','task_ms']]] for r in e['E44']['metal_contrasts'] if r['baseline']=='off-split']),'',
      tr('The bare groups measure end-to-end behavior without Metal collection. Profiled split/coalesced controls isolate command-buffer spans; do not add gains across experiments. Allocating batch1024 can increase compute-buffer capacity, so this is not a memory reduction or an exact old512-capacity baseline. Metal compute reservation grows298.75→597.50MiB and CPU compute12.01→24.03MiB versus E42 batch512; both reserve896MiB KV. These are init reservations, not peak RSS. Actual allocation is recorded in E44 method metadata. All negative pairs remain; seven repeats do not establish tail guarantees.',
         'bare组衡量不采集Metal trace时的端到端行为，带observer的split／coalesce对照衡量设备跨度，不跨实验累加收益。batch1024会增加计算缓冲区容量，这不是内存优化，也不是旧512容量的精确基线；对比E42 batch512，Metal计算预留298.75→597.50MiB、CPU计算12.01→24.03MiB，KV均896MiB。这是初始化预留而非峰值RSS，实际分配见E44 method元数据。保留全部负对，7次不能保证尾延迟。'),'',
      tr('See [tasks](data/metal-tasks.csv), [all paired metrics](data/metal-pairs.csv), [command timestamps](data/metal-commands.csv), [joined wait partitions](data/metal-waits.csv) and [scope partitions](data/metal-scopes.csv). Each run has48 actual CLI sanitizer tasks; E42/E43 have30 native unit cases, E44 has47 including budget/minimum/invalid/reset checks. Final E44 source also passes14,357-token coalesced Native, strict Stream and default C/HTTP/CLI regression. No injected decode failure, Windows/CUDA, exact external-device scheduler trace or autonomous0.6B capability claim.',
         '见[任务](data/metal-tasks.csv)、[全部配对指标](data/metal-pairs.csv)、[command时间戳](data/metal-commands.csv)、[关联wait分解](data/metal-waits.csv)、[scope分解](data/metal-scopes.csv)。每轮48个真实CLI sanitizer任务；E42/E43各30原生场景，E44为47，含预算／最小批量／非法原子设置／重置。最终E44源码还通过14,357-token合并Native、strict Stream与默认C/HTTP/CLI回归。没有注入decode失败，未验收Windows/CUDA、其他设备调度精确trace或0.6B自主能力。'),'']
    aux=sum(len(e[k]['metal_commands']) for k in ['E42','E43','E44'])
    lines += [tr(f'{aux} auxiliary Metal records are stored separately from main CLI rows (7179 command,2393 wait,3 metadata). All raw traces are indexed; main counts never imply independent tasks.',f'{aux}条Metal附加记录与主CLI记录分列（7179 command、2393 wait、3 metadata），原始trace可追溯；主记录数不表示独立任务数。'),'']
    return lines


def tailwait_worst(d):
    lookup={(r['words'],r['producer_iterations'],r['mode'],r['rep']):r for r in d['metal_tasks']};out=[]
    for a,b in [('off-split-bare','off-coalesce-bare'),('off-split','off-coalesce'),('off-split-notify','off-coalesce-notify')]:
        for w in [45,51]:
            pairs=[(lookup[(w,14000000,b,j)]['task_ms']-lookup[(w,14000000,a,j)]['task_ms'],j) for j in range(7)];extra,j=max(pairs);x,y=lookup[(w,14000000,a,j)],lookup[(w,14000000,b,j)]
            out.append(dict(words=w,producer_iterations=14000000,baseline=a,mode=b,rep=j,**{'extra_'+k:y[k]-x[k] for k in x.keys()&y.keys() if k not in ['words','producer_iterations','mode','rep','metal_profiled']}))
    return out

def old86_steps(d):
    rows=[(i,r) for i,r in enumerate(d['rows']) if r.get('phase')=='evaluation' and r.get('words')==45 and r.get('producer_iterations')==14000000 and r.get('rep')==1 and r['kind']=='step'];a={r['step']:(i,r) for i,r in rows if r['mode']=='off-split-bare'};b={r['step']:(i,r) for i,r in rows if r['mode']=='off-coalesce-bare'}
    return [dict(experiment='E44',step=j,baseline_record_index=a[j][0],candidate_record_index=b[j][0],**{'extra_'+f:b[j][1][f]-a[j][1][f] for f in ['prefill_ms','decode_ms','sync_ms','decode_cpu_ms','sync_cpu_ms','prepare_thread_cpu_ms']},evaluated_tokens=a[j][1]['evaluated_tokens']) for j in a]

def capacity_worst(d):
    ts=d['metal_tasks'];lookup={(t['words'],t['producer_iterations'],t['mode'],t['rep']):t for t in ts};out=[]
    for a,b in [('off-512-notify','off-640-notify'),('off-1024-notify','off-640-notify'),('growth-512-bare','growth-640-bare'),('off-512-bare','off-640-bare')]:
        pairs=[(lookup[(w,it,b,j)]['task_ms']-t['task_ms'],t,lookup[(w,it,b,j)]) for (w,it,m,j),t in lookup.items() if m==a];extra,x,y=max(pairs,key=lambda r:r[0])
        def index(t):return next(i for i,r in enumerate(d['rows']) if r['kind']=='task_timing' and r['phase']=='evaluation' and all(r[k]==t[k] for k in ['words','producer_iterations','mode','rep']))
        out.append(dict(words=x['words'],producer_iterations=x['producer_iterations'],rep=x['rep'],baseline=a,mode=b,baseline_record_index=index(x),candidate_record_index=index(y),**{'extra_'+k:y[k]-x[k] for k in x.keys()&y.keys() if k not in ['words','producer_iterations','mode','rep','metal_profiled']}))
    return out


def tailcapacity_report(e,zh):
    def tr(a,b):return b if zh else a
    def val(r,f):
        if ('completion_delivery' in f or 'after_notify_return' in f) and not r.get('mode','').endswith('-notify'):return '—'
        return f"{r[f]:.3f}" if f in r else '—'
    def fmt(c):return f"{c['median_saving']:.3f} [{c['min_saving']:.3f},{c['max_saving']:.3f}]"
    lines=['## '+tr('E45/E46 · sustained-tail diagnosis and smaller compute reservation','E45/E46 · 持续尾延迟与更小计算预留'),'',
      tr('E45 repeats the E44-shaped workload with split/coalesced × bare/no-marker/marker controls,1539 main records. The old86.283ms bare pair still lacks GPU timestamps. Its93.325ms extra final prefill is spread across all four preparations,22.717/11.694/32.119/26.795ms; owner CPU decreases in each. Thus it was not one measured CPU wakeup or extra CPU token processing. Device progress versus host notification remains unknown for that exact historical pair.',
         'E45按E44负载重测split／coalesce×bare／无marker／marker，1539主记录。旧86.283ms bare仍没有GPU时间戳；最终prefill多93.325ms分散在四次prepare，分别22.717／11.694／32.119／26.795ms，每次owner CPU反而减少。因此它不是一次已被测到的CPU唤醒，也不是额外CPU分词处理；旧样本设备进展与host通知仍无法精确区分。'),'',
      table(['Historical E44 step','Tokens','Extra prefill ms','Extra decode ms','Extra sync ms','Extra owner CPU ms'],[[r['step'],r['evaluated_tokens'],*[val(r,'extra_'+f) for f in ['prefill_ms','decode_ms','sync_ms','prepare_thread_cpu_ms']]] for r in old86_steps(e['E44'])]),'',
      tr('The86ms regression does not reproduce in E45: paced bare worst1.908ms (tool+3.871ms, prefill−2.058ms); no-marker trace worst11.248ms (tool+14.212ms, prefill−3.054ms). Marker worst1.157ms has tool+2.069ms, GPU union+1.991ms, decode−3.797ms, and notification/return−0.180/−0.043ms. These new tails are explained by their measured stages, not evidence that the old tail was eliminated. The45-word paced device-span saving is only2.486ms median with marker, versus36.747ms burst; batch merging is workload-dependent.',
         'E45没有重现86ms：持续bare最差慢1.908ms（工具＋3.871ms、prefill－2.058ms）；无marker trace最差慢11.248ms（工具＋14.212ms、prefill－3.054ms）。marker最差慢1.157ms，工具＋2.069ms、GPU并集＋1.991ms、decode－3.797ms，通知／返回反而少0.180／0.043ms。这些新负对有对应阶段证据，不能据此宣称旧尾延迟已消除。45词持续marker的GPU跨度中位只少2.486ms，突发少36.747ms；合批收益与工作节奏有关。'),'',
      '![Sustained tail](figures/sustained-tail.png)','',
      table(['E45 paced words','Candidate','Worst extra task ms','Extra tool ms','Extra prefill ms','Extra GPU span ms','Extra notify delivery ms','Extra post-marker return ms'],[[r['words'],r['mode'],*[val(r,'extra_'+f) for f in ['task_ms','tool_sum_ms','final_prefill_ms','final_metal_gpu_union_ms','final_metal_sync_completion_delivery_ms','final_metal_sync_after_notify_return_ms']]] for r in tailwait_worst(e['E45'])]),'',
      tr('Notification columns apply only to marker mode; zero in no-marker traces means unobserved, not measured zero. GPU spans include internal memory/sync and possible device preemption. Notification and post-marker return still include driver/handler/scheduler coordination. No GPU frequency/temperature counter, external queue timeline, or kernel CPU-wakeup trace was collected. All negative pairs and non-reproduction remain in evidence.',
         '通知列只对marker模式有观测；无marker trace的0表示未观测，不是测得零延迟。GPU跨度含内部访存／同步及可能的设备抢占；通知与marker后返回仍含driver／handler／调度协调。没有采GPU频率／温度、其他队列时间线或内核CPU唤醒trace；全部负对及未复现保留。'),'',
      tr('The allocation cause is direct: cxf couples n_batch/n_ubatch to the startup batch parameter; pinned [llama.cpp sched_reserve](https://github.com/ggml-org/llama.cpp/blob/f1e44dcc11d8802d107bd7331a3d3fd3e6f57b93/src/llama-context.cpp#L596) reserves the maximum graph using n_ubatch. E46 reuses the existing parameter at640; no inference source or runtime binary change, extra context, dynamic reallocation, snapshot or eviction. Larger initial input still chunks; approximately600-token tool increments can fit in one batch.',
         '计算预留原因直接可查：cxf启动参数同时设置n_batch／n_ubatch，固定[llama.cpp sched_reserve](https://github.com/ggml-org/llama.cpp/blob/f1e44dcc11d8802d107bd7331a3d3fd3e6f57b93/src/llama-context.cpp#L596)按n_ubatch预留最大图。E46复用已有参数设640，不改推理源码或运行时二进制，不新增context／动态重分配／快照／逐出。较大初始输入继续分块，约600-token工具增量可一次处理。'),'',
      table(['Capacity','Loads','Metal compute MiB','CPU compute MiB','Physical KV MiB'],[[cap,len(g),*[f"{statistics.median(r['allocation_mib'][f] for r in g):.2f}" for f in ['metal_compute','cpu_compute','physical_kv']]] for cap in [512,640,1024] for g in [[r for r in e['E46']['capacity_runs'] if r['capacity']==cap]]]),'',
      tr('640 reserves373.44MiB Metal compute versus597.50MiB at1024:224.06MiB less (37.5%), leaving only74.69MiB above default512. CPU compute falls24.03→15.02MiB; KV remains896MiB. These are init reservations, not measured peak RSS or KV savings. The default remains512;640 is an explicit configuration, not an online capacity switch.',
         '640的Metal计算预留373.44MiB，相对1024的597.50MiB少224.06MiB（37.5%），相对默认512仅多74.69MiB；CPU计算24.03→15.02MiB，KV仍896MiB。这是初始化预留，不是峰值RSS或KV节省。默认仍512，640是显式配置，不是在任务中免费切容量。'),'',
      tr('Fresh47/54-word,0/12M workloads run in seven outer blocks, rotating512/split,640/coalesced,1024/coalesced load order and mode order. Each capacity/load has off-bare, growth-bare and off-marker modes, one warmup and one evaluation per case.21 models,252 evaluation plus252 warm tasks,4095 main rows. Load/warm/helper first execution excluded; all warm rows retained, KV resident within each task/load. Cross-capacity block contrasts reduce but do not remove temporal drift.',
         '新47/54词、0/1200万循环，7个外层block旋转512/split、640/coalesce、1024/coalesce加载与模式顺序；每容量有关闭bare、增长bare、关闭marker，每负载1warm＋1eval。21模型、252评价＋252warm任务，4095主记录。加载／warm／helper首次执行不计评价，warm原样保留，每任务／加载内KV常驻；block配对减少但不消除时间漂移。'),'',
      '![Capacity control](figures/capacity-control.png)','',
      table(['E46 words','Iterations','Baseline','Candidate','Task saving median [min,max] ms','Faster /7','Final prefill saving ms'],[[r['words'],r['producer_iterations'],r['baseline'],r['mode'],fmt(r['task_ms']),r['task_ms']['positive_pairs'],f"{r['final_prefill_ms']['median_saving']:.3f}"] for r in e['E46']['metal_contrasts'] if r['baseline'].endswith('-bare')]),'',
      table(['E46 profiled words','Iterations','Baseline','Candidate','GPU span saving median ms','Delivery saving median ms','Post-marker return saving median ms','Task saving median ms'],[[r['words'],r['producer_iterations'],r['baseline'],r['mode'],*[f"{r[f]['median_saving']:.3f}" for f in ['metal_gpu_union_ms','metal_sync_completion_delivery_ms','metal_sync_after_notify_return_ms','task_ms']]] for r in e['E46']['metal_contrasts'] if r['baseline'].endswith('-notify')]),'',
      tr('Smaller reservation is a memory/latency tradeoff, not a universal speed optimization. Against default512,640 growth paced task medians regress24.934/12.957ms. The worst growth pair regresses217.550ms: tool+190.983ms, producer wall+150.844ms versus own CPU+12.507ms, runnable-ready proxy+153.688ms, final prefill+30.848ms. The proxy has accounting lag and does not identify a competitor. The worst off-bare burst pair regresses176.227ms with162.131ms extra final prefill and no device trace.',
         '缩容是内存／速度取舍，未获得通用加速。640增长持续组相对默认512中位反慢24.934／12.957ms；最差反慢217.550ms，其中工具＋190.983ms、producer墙钟＋150.844ms、自身CPU＋12.507ms、ready代理＋153.688ms、最终prefill＋30.848ms。代理有计量滞后，不能定位竞争者。关闭bare突发最差慢176.227ms，最终prefill多162.131ms且没有设备trace。'),'',
      tr('The worst matched marker pair640 versus512 regresses136.408ms: final prefill+107.952ms, observed GPU union+106.174ms, sync GPU overlap+105.772ms, delivery−0.320ms, post-marker return−0.073ms, uncovered commit delay+0.148ms, tool+24.597ms. Thus this new prefill tail lies in device execution spans, not completion delivery/CPU return or submission queue. Evaluated token counts are equal, with fewer decode calls; spans can grow due to kernel efficiency, stalls, GPU scheduling or clock/state variation, none individually identified. Against1024 the same640 task is179.725ms slower, with144.977ms extra GPU union. Do not retrospectively assign these measurements to the old86ms pair.',
         '新marker最差640对512反慢136.408ms：最终prefill＋107.952ms、GPU并集＋106.174ms、sync与GPU重叠＋105.772ms、通知交付－0.320ms、marker后返回－0.073ms、未解释提交延迟仅＋0.148ms、工具＋24.597ms。因此这次prefill尾部落在设备执行跨度，通知／CPU返回或提交排队没有相应增长。evaluated token数量相同且decode次数更少；跨度可因kernel效率、访存停顿、GPU调度或频率／状态变化增大，尚未逐一识别。同一640任务对1024慢179.725ms，GPU并集多144.977ms；不能倒推成旧86ms的原因。'),'',
      table(['E46 worst pair','Words / iterations / block','Extra task ms','Extra tool ms','Extra final prefill ms','Extra GPU span ms','Extra delivery ms','Extra post-marker return ms'],[[r['baseline']+' → '+r['mode'],str(r['words'])+' / '+str(r['producer_iterations'])+' / '+str(r['rep']),*[val(r,'extra_'+f) for f in ['task_ms','tool_sum_ms','final_prefill_ms','final_metal_gpu_union_ms','final_metal_sync_completion_delivery_ms','final_metal_sync_after_notify_return_ms']]] for r in capacity_worst(e['E46'])]),'',
      tr('Acceptance: E45 has47 native sanitizer cases and48 actual CLI sanitizer tasks. E46 has72 actual CLI sanitizer tasks across all capacities,640-capacity14,357-token Native, strict Stream and default C/HTTP/CLI regression. Runtime and core sources match E44/E45; orchestration and the test-only long-context capacity selector are newly archived. All negative results remain; no claimed p95/p99, Windows/CUDA acceptance or autonomous-quality result.',
         '验收：E45有47原生sanitizer场景和48实际CLI sanitizer任务；E46有三容量72实际CLI sanitizer任务、640容量14,357-token Native、strict Stream及默认C/HTTP/CLI回归。runtime与核心源码沿用E44/E45，新归档实验编排和仅测试使用的长上下文容量选择。负结果全保留，不声称p95/p99、Windows/CUDA验收或自主能力成绩。'),'',
      tr('See [old86 step audit](data/tailwait-old86.csv), [new worst-pair attribution](data/tailwait-worst.csv), [all model allocations](data/capacity-runs.csv), [capacity worst pairs](data/capacity-worst.csv), [tasks/pairs](data/metal-pairs.csv) and [joined wait partitions](data/metal-waits.csv). All new device auxiliary rows are counted separately in the catalog/integrity file.',
         '见[旧86ms逐步审计](data/tailwait-old86.csv)、[新最差负对归因](data/tailwait-worst.csv)、[每次模型分配](data/capacity-runs.csv)、[容量最差配对](data/capacity-worst.csv)、[全部任务配对](data/metal-pairs.csv)、[关联wait分解](data/metal-waits.csv)。新增设备附加记录在catalog／integrity中单独计数。'),'']
    return lines


def contention_report(e,zh):
    def tr(a,b):return b if zh else a
    def fmt(v):return f"{v['median_saving']:.3f} [{v['min_saving']:.3f},{v['max_saving']:.3f}]"
    lines=['## '+tr('E47–E49 · matched tool, graph-shape and memory-pressure isolation','E47–E49 · 同负载工具、图形状与内存压力隔离'),'',
      tr('The historical217.550ms pair combines a third producer main stall68.2→217.4ms and1→2 streaming updates; correlation does not establish that the extra update caused the whole stall. E47 fixes capacity640 and final chunk policy throughout off/thread-only/token-only/growth controls;47/54 words,0/12M iterations, six modes including off/growth marker controls,1539 records. The same commands, canonical transcript and greedy tokens are required.',
         '旧217.550ms负对第三个producer main由68.2→217.4ms，流式update由1→2；相关性不能证明额外update造成全部延迟。E47固定640容量和相同最终分块，关闭／仅线程／仅分词／真实growth加关闭和growth marker共六组，47/54词、0/1200万循环，1539记录；要求同命令、canonical transcript与greedy。'),'',
      table(['E47 words','Iterations','Baseline','Candidate','Task saving median [min,max] ms','Producer wall saving median ms','Ready proxy saving median ms'],[[r['words'],r['producer_iterations'],r['baseline'],r['mode'],fmt(r['task_ms']),f"{r['producer_wall_ms']['median_saving']:.3f}",f"{r['producer_ready_proxy_ms']['median_saving']:.3f}"] for r in e['E47']['metal_contrasts']]),'',
      tr('Large tool tails occur without overlapping GPU work:54-word paced thread-only versus off worst task+1868.877ms, tool+1817.950ms, producer wall+1761.856ms, own CPU−22.205ms, runnable-ready proxy+1784.891ms. Growth worst+392.829ms has tool+522.869ms and final prefill−334.860ms. This rules out concurrent inference as a necessary condition for these tool stalls; it does not identify a competing PID or exclude every indirect effect. Negative counter proxies, all outliers and marker/bare controls remain.',
         '没有工具期间GPU预填充也出现大尾部：54词持续仅线程对关闭最差整任务多1868.877ms、工具多1817.950ms、producer墙钟多1761.856ms、自身CPU少22.205ms、ready代理多1784.891ms。growth最差多392.829ms，工具多522.869ms、最终prefill反少334.860ms。这说明并发推理不是这些工具停顿的必要条件；仍未定位竞争PID或排除所有间接影响。负代理值、全部异常和marker/bare对照保留。'),'',
      '![Matched controls](figures/contention640-controls.png)','',
      tr('E48 directly isolates device-span sensitivity. A640-capacity engine keeps a2048-token read-only root; forked logical prefixes768/2048, increments128/224/512/582, split512 or coalesced640. Idle, arithmetic and64MiB cache-line read/write workers run with identical model input per shape; one warmup+seven evaluations gives384 samples plus3 status records. Root cells remain physically resident even for logical768, so cross-prefix differences are not a production context-scaling curve. Worker setup/join and independent fresh-KV/canonical oracles are excluded from the prefill timer.',
         'E48直接隔离设备跨度敏感性：640容量引擎保留2048-token只读root，逻辑fork prefix768/2048，新增128/224/512/582，split512或coalesce640。每shape同输入，空闲／算术／64MiB缓存行读写线程，1warm＋7eval共384样本＋3状态。逻辑768仍保留2048 root物理cells，不能把跨prefix差当生产上下文缩放曲线。worker创建／join及fresh-KV／canonical oracle均不计prefill时间。'),'',
      '![Device pressure](figures/device-isolation.png)','',
      table(['Prefix','New tokens','Baseline','Candidate','Extra wall median [min,max] ms','Extra GPU median ms','Extra uncovered submit median ms'],[[r['prefix_tokens'],r['new_tokens'],r['baseline'],r['mode'],f"{r['wall_ms']['median_extra']:.3f} [{r['wall_ms']['min_extra']:.3f},{r['wall_ms']['max_extra']:.3f}]",f"{r['metal_gpu_union_ms']['median_extra']:.3f}",f"{r['metal_queue_no_own_gpu_ms']['median_extra']:.3f}"] for r in e['E48']['isolation_contrasts']]),'',
      tr('Idle582-token coalescing reduces GPU spans7.869/13.458ms at logical prefixes768/2048, with negative pairs retained.128/224/512 are same-batch controls rather than a new fusion. Memory versus arithmetic pressure adds about6.5–10.2ms median GPU span for224/582 coalesced increments, generally0.02–0.19ms extra uncovered submission interval. Arithmetic alone is sometimes faster than idle; pressure changes scheduling/power as well as traffic. Memory-path contention can lengthen device execution, but these synthetic results cannot explain the entire old217ms or106ms device tail. No direct DRAM bandwidth or pure kernel/preemption counter was collected.',
         '空闲582-token合并在逻辑prefix768/2048减少GPU跨度7.869/13.458ms，负对保留；128/224/512只是相同单批控制。内存对算术压力使224/582合并增量的GPU跨度中位多约6.5–10.2ms，未解释提交区间一般仅多0.02–0.19ms。算术压力有时反比空闲快，压力也改变调度／功耗状态。可确认内存路径争用能拉长设备执行，但不能解释旧217ms或106ms设备尾部的全部来源。没有直接DRAM带宽、纯kernel／preemption计数。'),'',
      tr('M5 reports dispatch sampling=false, stage sampling=true and only timestamp/GPUTimestamp. [Apple counter sampling](https://developer.apple.com/documentation/metal/sampling-gpu-data-into-counter-sample-buffers) requires supported boundaries. Per-batch metadata labels cxf decode position/amount and associated buffers; a batch can contain multiple buffers/kernels. Intrinsic shape effects, memory sensitivity, submission residuals and host notification are distinct measurements, not a complete hardware utilization diagnosis.',
         'M5实测dispatch sampling=false、stage=true，仅timestamp/GPUTimestamp；[Apple counter sampling](https://developer.apple.com/documentation/metal/sampling-gpu-data-into-counter-sample-buffers)要求相应采样边界受支持。逐批标签关联cxf decode位置／数量与buffer，一个batch可含多个buffer／kernel。图形状效果、内存敏感性、提交残差和host通知分开测量，不冒充完整硬件利用率诊断。'),'',
      tr('E49 freezes an explicit larger publication quantum512→1536 bytes before fresh49/55-word,0/10M evaluation. Capacity640,10ms growth gate,512-token budget,min128, BPE/final canonical guard and resident KV are unchanged. Later start may lose overlap; all task/producer/device negatives remain and defaults are unchanged.',
         'E49在新49/55词、0/1000万循环评价前冻结发布量子512→1536字节。640容量、10ms growth窗、512 token预算、min128、BPE／最终canonical复核及KV常驻保持；较晚启动可能损失重叠，完整任务／producer／设备负对均保留，默认不切换。'),'',
      '![Larger output quantum](figures/coarse640-validation.png)','',
      table(['E49 words','Iterations','Baseline','Candidate','Task saving median [min,max] ms','Faster /7','Tool wall saving median ms','Final prefill saving median ms'],[[r['words'],r['producer_iterations'],r['baseline'],r['mode'],fmt(r['task_ms']),r['task_ms']['positive_pairs'],f"{r['tool_sum_ms']['median_saving']:.3f}",f"{r['final_prefill_ms']['median_saving']:.3f}"] for r in e['E49']['metal_contrasts']]),'',
      tr('The larger-quantum attempt fails the fresh validation. Bare paced tasks regress54.667/3.009ms versus old growth (0/7 and1/7 faster); burst medians also regress3.567/5.492ms. Marker-paced runs have42 batches in both policies: evaluated stream tokens9240→11970 and cumulative GPU spans2518.927→3274.296ms. Three-call paired median join cost grows82.889/73.485ms while final prefill saves25.716/63.708ms. Group medians are not additive; actual matched tasks determine the result. Later start and more work per batch cost more than the saved final prefill. Default keeps the old gate;1536 remains an explicit experimental option, not an improvement claim.',
         '更大量子在新验证中失败：bare持续任务对旧growth反慢54.667/3.009ms，只有0/7和1/7更快；突发中位也反慢3.567/5.492ms。带marker持续样本两策略均42批，流式evaluated token由9240→11970、GPU累计2518.927→3274.296ms。每三工具任务逐对中位join多82.889/73.485ms，最终prefill虽少25.716/63.708ms，未能抵消。不同指标中位不能直接相加，以实际配对任务为准。较晚启动和每批更多工作代价超过末尾节省；默认保留旧门槛，1536仅实验选项，不称优化成功。'),'',
      tr('Functional acceptance: E47 has63 sanitizer cases/48 actual CLI tasks; E48 has96 sanitizer samples and8 fresh shapes; E49 has72 cases/48 CLI tasks plus final640-capacity14K Native, strict Stream and default C/HTTP/CLI regression. Operator-grade frequency/occupancy/bandwidth/preemption attribution and Windows/CUDA remain unvalidated. New code does not relabel old performance revisions.',
         '功能验收：E47为63个SAN场景／48实际CLI任务，E48为96个SAN样本和8个fresh shape，E49为72场景／48 CLI任务及最终640容量14K Native、strict Stream、默认C/HTTP/CLI回归。硬件频率／占用／带宽／preemption完整归因与Windows/CUDA仍未验收，不把新源码标成旧版本性能重跑。'),'',
      tr('Detailed [decode batches](data/metal-batches.csv), [fixed-shape samples](data/device-isolation-samples.csv), [pressure/shape contrasts](data/device-isolation-pairs.csv), [all CLI pairs](data/metal-pairs.csv) and [wait partitions](data/metal-waits.csv) retain warmup, raw labels, interval bounds and all negatives. Main records and command/wait/batch auxiliary rows are counted separately.',
         '明细见[decode批次](data/metal-batches.csv)、[固定shape样本](data/device-isolation-samples.csv)、[压力／形状配对](data/device-isolation-pairs.csv)、[CLI全部配对](data/metal-pairs.csv)、[wait分解](data/metal-waits.csv)，保留warm、原标签、区间边界和全部负结果。主记录与command／wait／batch附加记录分列。'),'']
    return lines


def cpp_report(e,zh):
    def tr(a,b):return b if zh else a
    a=e['E56'];b=e['E57']
    def cell(d,case,mode):return next(r for r in d['summary'] if (r['case'],r['mode'])==(case,mode))
    def paired(d,case,label):return next(r for r in d['paired_summary'] if (r['case'],r['comparison'])==(case,label))
    def fmt(v):return f"{v['median']:.3f} [{v['min']:.3f},{v['max']:.3f}]"
    lines=['## '+tr('E56/E57 — Fixed-target C++: headers, caches and host coordination','E56/E57 — 固定target的C++头文件、缓存与宿主协调'),'',
      tr('E55 had an incomplete equivalence check: bound-off defaulted to macOS26.0 and bound-on to10.4, despite identical compiler/SDK hashes and program results. Its59ms residual is retained as target-confounded history. Cross-compiling the off-preprocessed input inside the sandbox exposed availability errors. New trials explicitly use -mmacosx-version-min=26.0 in every mode and verify both driver triple and deployment macro. No sysctl or other permission is opened to fix target discovery.',
         'E55的等价性检查不完整：相同编译器／SDK与程序结果下，绑定off默认macOS26.0，绑定on却为10.4。旧59ms残差保留为带target混杂的历史读数；交叉使用预处理文件时可用性报错暴露了此问题。新试验全部显式使用-mmacosx-version-min=26.0，并核对driver triple与部署版本宏，不靠开放sysctl或其他权限修复目标探测。'),'',
      table(tr(['E56 control','Off/on inner median ms','Off/on sys CPU median ms','Paired whole API extra ms [min,max]'],['E56对照','关闭／开启内部中位ms','关闭／开启sys CPU中位ms','完整API逐对额外ms [最小,最大]']),[[case,f"{cell(a,case,'off')['inner_ms']['median']:.3f} / {cell(a,case,'on')['inner_ms']['median']:.3f}",f"{cell(a,case,'off')['sys_ms']['median']:.3f} / {cell(a,case,'on')['sys_ms']['median']:.3f}",fmt(paired(a,case,'isolation_overhead')['wall_delta_ms'])] for case in ['noop','version','preprocess','syntax','object','preprocessed','link','fresh_run','chain']]),'',
      tr('The extra sys CPU follows textual header access. The include trace has1051 entries (not syscall counts), and ordinary object/repeated builds emit no PCM. Compiling already-preprocessed input removes most of the sys difference; it is a diagnostic control whose preparation is not a free production optimization. Compiler startup/version and link stages have smaller differences. Parent setup is roughly1ms, cleanup fractions of a millisecond; EOF/reaping can add a roughly10ms poll. Fresh-executable execution also has variable non-CPU wall time. No per-syscall, code-signing, CPU-cluster or external-scheduler trace can allocate every old or new tail.',
         '额外sys CPU跟随文本头文件访问。include trace有1051条记录（不是syscall次数），普通目标文件／重复编译没有PCM；编译已预处理输入后sys差异大幅缩小，但该控制不把预处理准备当免费优化。编译器启动／版本与链接差异较小；父准备约1ms、清理通常不足1ms，EOF回收可另有约10ms轮询。新可执行文件运行还存在波动的非CPU墙钟。未采逐syscall、签名、CPU簇或外部调度trace，不能精确分完所有旧／新尾部。'),'',
      table(tr(['E56 on cache control','Complete API median ms','First/second stage median ms','PCM files','Temp logical file bytes'],['E56开启缓存对照','完整API中位ms','第一／第二阶段中位ms','PCM文件','临时文件逻辑字节']),[[case,f"{cell(a,case,'on')['wall_ms']['median']:.3f}",f"{cell(a,case,'on')['stage0_wall_ms']['median']:.3f} / "+(f"{cell(a,case,'on')['stage1_wall_ms']['median']:.3f}" if 'stage1_wall_ms' in cell(a,case,'on') else '—'),cell(a,case,'on')['pcm_files']['median'],cell(a,case,'on')['file_bytes']['median']] for case in ['object','reuse','modules','pch','stat','plain3','pch3']]),'',
      tr('270 serial unsanitized rows,2 warmup+7 evaluation blocks. Modules create26 PCM files and speed the second compile but have a high cold cost. Fresh SDK stat-cache generation is also a negative result. Private PCH helps repeated headers only after charging its cold build. Clang documents [module activation](https://clang.llvm.org/docs/Modules.html), [PCH](https://clang.llvm.org/docs/PCHInternals.html) and the [stat-cache design](https://reviews.llvm.org/D136651); measured Apple-Clang behavior, not upstream documentation alone, decides these results.',
         '270条串行无SAN记录，2预热＋7评价block。模块模式生成26个PCM、第二次编译更快，但冷成本高；全新SDK stat-cache生成也是负结果。PCH对重复头文件的作用要扣除冷生成成本。Clang的[模块启用](https://clang.llvm.org/docs/Modules.html)、[PCH](https://clang.llvm.org/docs/PCHInternals.html)和[stat-cache设计](https://reviews.llvm.org/D136651)提供依据，结果仍以本机Apple-Clang实测为准。'),'',
      '![Fixed-target C++ results](figures/sandbox-cpp.png)','',
      table(tr(['E57 new task','TUs','Plain on / PCH on median ms','Paired PCH saving ms [min,max]','Faster','PCH on−off ms [min,max]'],['E57新任务','翻译单元','原开启／PCH开启中位ms','逐对PCH节省ms [最小,最大]','更快','PCH开启−关闭ms [最小,最大]']),[[case,next(r['units'] for r in b['rows'] if r['case']==case),f"{cell(b,case,'on')['wall_ms']['median']:.3f} / {cell(b,case,'pch_on')['wall_ms']['median']:.3f}",fmt(paired(b,case,'pch_saving')['wall_delta_ms']),f"{paired(b,case,'pch_saving')['positive']}/7",fmt(paired(b,case,'pch_isolation_overhead')['wall_delta_ms'])] for case in ['single_map','single_regex','batch3','batch5']]),'',
      tr('180 actual CLI tasks,4 new cases×5 modes×(2 warmup+7 evaluation blocks), exact program-result checks, identical explicit target/compiler/SDK/C++17/O2, rotating/reversed order. Every PCH is built fresh inside the task and deleted: generation, validation, all TUs, link, program execution, observers and cleanup are charged. Single-file PCH regresses158.765/118.314ms median;3/5 TUs save330.300/793.776ms, each7/7. PCH adds14.56–15.51MB of transient logical file payload; process RSS and physical KV were not measured. Bound/PCH on−off still has large positive and negative pairs; no claim that isolation is free.',
         '180个真实CLI任务，4新任务×5模式×（2预热＋7评价block），核对程序结果，显式target／编译器／SDK／C++17／O2一致，顺序轮转／倒转。每任务新建并删除PCH，生成、验证、全部翻译单元、链接、程序、观察器和清理全部计入。单文件PCH中位反慢158.765／118.314ms，3／5翻译单元节省330.300／793.776ms，均7/7。PCH增加14.56–15.51MB短期逻辑文件载荷，未测进程RSS或物理KV；开启−关闭仍有宽幅正负对，不称隔离免费。'),'',
      table(tr(['Experimental grouped deny rules','Paired saving ms [min,max]','Faster'],['实验性deny分组','逐对节省ms [最小,最大]','更快']),[[case,fmt(paired(b,case,'group_saving')['wall_delta_ms']),f"{paired(b,case,'group_saving')['positive']}/7"] for case in ['single_map','single_regex','batch3','batch5']]),'',
      tr('The candidate factors72 original patterns into6 bounded equivalent groups (max511 bytes), passes1765-path differential checks and15 OS suites, but fails performance acceptance. The worst batch3 pair adds1299.522ms: object stages add1291.321ms (user+370.221ms, sys+106.348ms), while outside-inner drops3.256ms. This locates the regression inside compiler execution, not parent preparation; it does not prove regex grouping itself caused all the variation. Keep the original default rules and denial logging. Do not add candidate and PCH savings together.',
         '候选把72条原模式因式分解为6个等价有界组（最长511字节），通过1765路径差分和15组OS测试，但性能验收失败。batch3最差多1299.522ms：目标文件阶段多1291.321ms（user＋370.221、sys＋106.348），外层反少3.256ms；位置在编译执行，非父准备，仍不能证明所有波动都由分组算法造成。默认保留原逐条规则与拒绝日志，不把候选和PCH收益相加。'),'',
      tr('The implemented deliverable is the optional tools/run_cpp_batch.sh plain|pch TARGET HEADER OUTPUT SOURCE... recipe; it adds no runtime cache/grant or automatic compiler-flag change.6 recipe suites pass on normal and ASan/UBSan CLI, including failure cleanup, changed headers, protected config and real local-network refusal with explicit-off controls. Full default561 C/15 OS/12 policy/8 foundation regression passes. Inference/native sources are byte-identical to E55; its Native14K/Stream/output acceptance remains historical for that same runtime, not a new model test.',
         '交付为可选tools/run_cpp_batch.sh plain|pch TARGET HEADER OUTPUT SOURCE...配方，不增加运行时缓存／授权或自动改编译参数。6组配方测试在普通和ASan/UBSan CLI通过，包括失败清理、头文件更新、配置保护及真实本机网络拒绝／显式off对照；默认561项C／15组OS／12策略／8基础回归通过。推理／Native源码与E55逐字节相同，其14K／Stream／输出验收仍引用相同运行时的历史版本，本轮未重跑模型。'),'',
      tr('Two earlier E57 attempts are retained: a parser import mistake and a warmup timeout with divergent clocks (~274s CLI versus0.8s active Python time). The cause of the pause/clock gap is unresolved. Formal timing used only a process-lifetime idle-sleep assertion and dual clocks; max absolute gap was0.029ms, with all pairs retained. No system power setting or agent timeout policy changed. Small-n ranges are not p95/p99 or confidence intervals.',
         '两次先前E57尝试保留：解析脚本漏导入与预热期时钟差异超时（CLI约274秒、Python活跃计时约0.8秒），暂停／时钟差异原因未定。正式计时仅使用进程存活期的防空闲休眠断言与双时钟，最大差异0.029ms，全部正负配对保留；不改系统电源设置或agent超时。小样本范围不是p95／p99或置信区间。'),'',
      tr('[E56 evidence](../evidence/sandbox-cpp-diagnostic-20261102.json), [E57 evidence](../evidence/sandbox-cpp-tasks-20261102.json), [stage samples](data/sandbox-cpp-diagnostic-samples.csv), [new CLI samples](data/sandbox-cpp-tasks-samples.csv), [all paired summaries](data/sandbox-cpp-tasks-paired-summary.csv), [aborted warmups](data/sandbox-cpp-aborted.csv) and [contract](../design/test-sandbox-v1.md). Prior56 evidence files and40 figure pairs remain byte-identical; this is build-command work, not inference acceleration.',
         '[E56证据](../evidence/sandbox-cpp-diagnostic-20261102.json)、[E57证据](../evidence/sandbox-cpp-tasks-20261102.json)、[阶段样本](data/sandbox-cpp-diagnostic-samples.csv)、[新CLI样本](data/sandbox-cpp-tasks-samples.csv)、[全部配对汇总](data/sandbox-cpp-tasks-paired-summary.csv)、[中止预热](data/sandbox-cpp-aborted.csv)和[契约](../design/test-sandbox-v1.md)。旧56份证据与40组图不改，这是构建命令工作，不计为推理加速。'),'']
    return lines


def cc_report(e,zh):
    def tr(a,b):return b if zh else a
    a=e['E54'];b=e['E55']
    def cell(d,case,mode):return next(r for r in d['summary'] if r['case']==case and r['mode']==mode)
    def fmt(v):return f"{v['median']:.3f} [{v['min']:.3f},{v['max']:.3f}]"
    lines=['## '+tr('E54/E55 — Xcode cache diagnosis and explicit same-toolchain binding','E54/E55 — Xcode缓存归因与显式同工具链绑定'),'',
        tr('Later E56 audit: the implicit deployment target differed (bound-off26.0,bound-on10.4). These historical compiler/SDK-matched measurements did not ensure equal target semantics; use E56/E57 explicit-target comparisons for new claims. The old evidence and plots are unchanged.', 'E56事后核对发现绑定控制组隐含部署target不同（off26.0，on10.4）。以下历史测量只核对了编译器／SDK，未保证目标语义等价；新结论采用E56/E57显式同target比较，旧证据与图不改。'),'',
        tr('The major trigger is a fresh private xcrun lookup cache: the /usr/bin shim invokes xcodebuild for platform/tool discovery. Reusing that lookup cache inside the same sandbox makes the second compile fast; deleting only this task-owned xcrun_db makes it slow again. A fresh off-mode HOME/TMPDIR control is much faster than restricted cold discovery, so this is not merely the cost of an empty cache. Restricted framework initialization and rejected-service fallback also matter. Actual Clang compilation with the same SDK is fast. Apple documents the [system tool shims](https://developer.apple.com/library/archive/technotes/tn2339/_index.html); exact discovery is measured locally, not assumed from documentation.',
           '主要触发条件是每次私有TMPDIR中的xcrun查找缓存为空：/usr/bin的shim会调用xcodebuild探测平台／工具。同一沙箱内复用缓存后第二次编译很快，只删除本任务的xcrun_db又恢复秒级耗时。off也使用全新HOME／TMPDIR时仍远快于受限冷发现，因此不能只归为空缓存；受限框架初始化和服务拒绝后的回退也有成本。同一Clang加同一SDK的实际编译很快。Apple说明了[系统工具shim](https://developer.apple.com/library/archive/technotes/tn2339/_index.html)，具体发现行为以本机实测为准。'),'',
        table(tr(['E54 API case','Mode','N','Complete ms','First inner ms','Second inner ms'],['E54 API负载','模式','样本','完整中位ms','首次内部ms','第二次内部ms']),[[case,mode,cell(a,case,mode)['n'],f"{cell(a,case,mode)['wall_ms']['median']:.3f}",f"{cell(a,case,mode)['phase_first_ms']['median']:.3f}",f"{cell(a,case,mode)['phase_second_ms']['median']:.3f}" if 'phase_second_ms' in cell(a,case,mode) else '—'] for case,mode in [('noop','off'),('noop','on'),('compile','off'),('compile','on'),('cold_env','off'),('cold_env','on'),('reuse','on'),('invalidate','on'),('sdk_only','on'),('pinned','on'),('platform','off'),('platform','on')]]),'',
        tr('126 serial unsanitized main records,2 warmup+5 evaluation blocks. No-log is a negative control: it does not remove the seconds-scale cost and has slower pairs; it is not shipped. SDKROOT alone also fails to avoid discovery. A direct compiler without its SDK fails to link. The attempted stack sample exited successfully but produced no call graph; a direct xcodebuild -find control emitted plugin failures and exceeded retained output. All development controls remain separate from formal timing.',
           '126条串行无SAN主记录，2预热＋5评价block。no-log是负结果：没有消除秒级成本且有更慢配对，不发布该策略；只设SDKROOT也没有避开发现。只用直接编译器但缺SDK会链接失败。栈采样虽退出成功却无调用栈；直接xcodebuild -find对照出现插件错误并超过保留输出，开发探针不混入正式计时。'),'',
        tr('The exact synthetic xcodebuild target produced7305 denial log entries, including2511 directory-service lookups; duplicate summaries are not exact syscall counts. Seventy separate direct-API records (1000 calls each) show quick rejected Mach lookups and accumulated costs in cache-directory/UID queries. These probes do not allocate the full xcodebuild delay to individual functions, and no rejected IPC was granted.',
           '精确过滤的合成xcodebuild目标有7305条拒绝日志，包括2511条目录服务查找；重复汇总不等于精确系统调用次数。另有70条直接API记录（各1000次调用），显示Mach拒绝通常快速返回，缓存目录／UID查询有累计成本。这些探针不能把完整xcodebuild延迟逐函数分完，也没有通过开放被拒IPC做优化。'),'',
        table(tr(['Direct API,1000 calls','Off median ms','On median ms','On worst single ms'],['直接API，每行1000次','关闭中位ms','开启中位ms','开启最慢单次ms']),[[case,*[f"{statistics.median(r['total_ms'] for r in a['ipc_rows'] if r['phase']=='evaluation' and r['case']==case and r['mode']==mode):.3f}" for mode in ['off','on']],f"{max(r['max_ms'] for r in a['ipc_rows'] if r['phase']=='evaluation' and r['case']==case and r['mode']=='on'):.3f}"] for case in sorted({r['case'] for r in a['ipc_rows']})]),'',
        tr('The implemented option is explicit --test-toolchain BIN_DIR --test-sdk SDK_DIR. It sets a pinned PATH and SDKROOT and grants no directories, IPC or network. Both paths are required and validated; no outside-sandbox workspace execution, shared lookup cache, global xcode-select change or implicit CLT substitution. Absolute /usr/bin/cc and explicit xcrun calls are not rewritten. Zero-config commands retain their original behavior and regression. This is a build-command environment optimization, separate from KV/prefill/inference work.',
           '实现的是显式--test-toolchain BIN_DIR --test-sdk SDK_DIR：绑定PATH与SDKROOT，不授权目录、IPC或网络。两路径必须同时有效；无沙箱外工作区执行、共享查找缓存、全局xcode-select修改或隐式CLT替换。不改写绝对/usr/bin/cc或显式xcrun调用。零配置命令仍保留原行为及回退。这是构建命令环境优化，与KV／预填充／推理优化分开。'),'',
        table(tr(['E55 new CLI task','Original on / bound on median ms','Paired saving ms [min,max]','Faster','Bound on−off ms [min,max]'],['E55新CLI任务','原开启／绑定开启中位ms','逐对节省ms [最小,最大]','更快','绑定开启−关闭ms [最小,最大]']),[[case,f"{cell(b,case,'on')['wall_ms']['median']:.3f} / {cell(b,case,'pinned_on')['wall_ms']['median']:.3f}",fmt(next(r for r in b['paired_summary'] if r['case']==case and r['comparison']=='binding_saving')['wall_delta_ms']),f"{next(r for r in b['paired_summary'] if r['case']==case and r['comparison']=='binding_saving')['positive']}/7",fmt(next(r for r in b['paired_summary'] if r['case']==case and r['comparison']=='bound_isolation_overhead')['wall_delta_ms'])] for case in ['checksum','archive','templates']]),'',
        '![Same-Xcode cache and new task controls](figures/sandbox-cc.png)','',
        tr('108 actual CLI tasks:3 new workloads×4 modes×(2 warmup+7 evaluation blocks), rotating/reversed order, exact program-result checks and identical selected compiler/SDK hashes. Every task has a fresh private HOME/TMPDIR; off creates/removes its directory inside the timed command, unlike E53 usual warm host cache. CLI time includes initialization, tool execution, cleanup and two scripted HTTP choices, with no model inference. C++ retains wide negative/positive residual pairs. Small-n ranges are not confidence intervals or proof of stable tail latency.',
           '108个真实CLI任务：3新负载×4模式×（2预热＋7评价block），旋转／倒序、核对程序结果和相同编译器／SDK哈希。每任务都有全新私有HOME／TMPDIR；off在计时命令中创建／清理目录，与E53通常已热的宿主缓存基线不同。CLI时间包含初始化、工具执行、清理及两次固定HTTP选择，无模型推理。C++仍有宽幅正负残差，小样本范围不是置信区间或稳定尾延迟证明。'),'',
        tr('Acceptance:561 C checks,15 OS suites, prior12 policy/8 foundation suites under sanitizers, Native14,357-token/16 Stream/72 output checks, and full default regression. Permission-binding tests cover normal compilation, outside-file refusal, secrets/config, network, parent environment and invalid paths. Two initial test-oracle mistakes are retained. Six existing native compiler warnings and uninstrumented upstream libraries remain.',
           '验收：561项C、15组OS、原12策略／8基础SAN套件、Native14,357-token／16 Stream／72输出及完整默认回归通过。绑定测试覆盖正常编译、外部文件拒绝、敏感文件／配置、网络、父环境和无效路径；两处初始测试预期错误保留。原6条Native编译警告和上游库未插桩边界不变。'),'',
        tr('[E54 evidence](../evidence/sandbox-cc-diagnostic-20261101.json), [E55 evidence](../evidence/sandbox-cc-tasks-20261101.json), [diagnostic samples](data/sandbox-cc-diagnostic-samples.csv), [direct API probes](data/sandbox-cc-ipc.csv), [new CLI samples](data/sandbox-cc-tasks-samples.csv), [all pairs](data/sandbox-cc-tasks-pairs.csv), [paired summaries](data/sandbox-cc-tasks-paired-summary.csv) and [contract](../design/test-sandbox-v1.md). Prior54 evidence files and39 figure pairs remain unchanged. No inference performance or autonomous quality claim.',
           '[E54证据](../evidence/sandbox-cc-diagnostic-20261101.json)、[E55证据](../evidence/sandbox-cc-tasks-20261101.json)、[诊断样本](data/sandbox-cc-diagnostic-samples.csv)、[直接API探针](data/sandbox-cc-ipc.csv)、[新CLI样本](data/sandbox-cc-tasks-samples.csv)、[全部配对](data/sandbox-cc-tasks-pairs.csv)、[配对汇总](data/sandbox-cc-tasks-paired-summary.csv)和[契约](../design/test-sandbox-v1.md)。旧54份证据与39组图不改，不新增推理性能或自主质量结论。'),'']
    return lines


def report(e, catalog, zh=False):
    def tr(en, cn): return cn if zh else en
    def stat(eid, key, **f): return stats(e, eid, key, **f)
    def cell(eid, key, scale=1, precision=1, **f): return fmt(stat(eid, key, **f), scale, precision)
    def kresult(batch,base,mode,key):
        return next(r[key] for r in e['E20']['prediction_summary'] if r['batch_tokens']==batch and r['base']==base and r['mode']==mode)
    total=sum(c['raw_records'] for c in catalog);raw_files=sum(bool(c['raw_records']) for c in catalog)
    lines = ['# '+tr('cxf × llama.cpp — benchmark results', 'cxf × llama.cpp — 测试成果汇总'), '',
        tr('[中文](README.zh-CN.md) · [Adaptation inventory](../adaptation-status.md) · [Methodology](METHODOLOGY.md) · [All metrics](data/metrics.csv) · [Raw records](data/raw-records.jsonl) · [Acceptance matrix](data/validation.csv)',
           '[English](README.md) · [当前适配](../adaptation-status.md) · [测试方法](METHODOLOGY.md) · [全部指标](data/metrics.csv) · [原始记录](data/raw-records.jsonl) · [验收矩阵](data/validation.csv)'), '',
        tr('Snapshot: 2026-11-02. E58 adds functional read-only self-prediction and a target-machine handoff, with no new performance samples. E56/E57 audit fixed-target C++ header/cache/host costs; E53 is the host-protection baseline. Earlier performance remains measured on historical revisions. Qwen3 0.6B is the compatibility test vehicle.',
           '归档日期：2026-11-02。E58新增只读自预测功能准备和异机交接，无新性能样本。E56/E57拆分固定同target的C++头文件／缓存／宿主成本，E53是宿主保护基线；此前性能仍对应历史源码版本，Qwen3 0.6B用于兼容验证。'), '',
        tr('The observed gains are in avoiding repeated frontend work, sharing canonical prefixes, and overlapping local work with input arrival. They are workload-dependent. Whole-process memory, autonomous coding quality, and Windows/CUDA performance have not been shown to improve.',
           '已经观测到的收益集中在减少前端重复工作、共享规范前缀，以及把本地工作与输入到达重叠执行。收益依赖场景；尚未证明整体进程内存、自主编程能力或 Windows/CUDA 性能改善。'), '',
        '## '+tr('E58 — Read-only self-prediction: functional preparation','E58 — 只读自预测：功能准备与异机交接'),'',
        tr('An experimental host now forks the actual evaluated prefix in the same KV pool, generates one bounded tool-call probe with the same model, starts only fs.read early, then accepts only an exact official call with matching epoch and a fresh policy/file-version check. Otherwise it discards the candidate and performs the official read. All engine operations remain on one owner; the probe pauses main decode. Default agentctl is unchanged.',
           '实验宿主已复用同KV池的实际前缀分叉，用同模型生成有界工具探针，只提前执行fs.read；正式调用、任务epoch和当前权限／文件版本均一致才采纳，否则丢弃并正常读取。所有引擎调用仍由一个owner串行驱动，probe会暂停主解码；默认agentctl未接入。'),'',
        tr('24 model-free security/state cases and8 Native cases (six KV/template and two injected flows) pass under ASan/UBSan; the existing14,357-token Native regression also passes. Initial JSON-stream compilation, thinking-header guard and outer-sandbox Metal/loopback failures are retained separately. No new performance rows or charts: larger-model hit rate, complete-task gains and WSL2/CUDA acceptance await the target machine. Existing58 evidence files and41 figure pairs are unchanged.',
           '24项无模型状态／安全测试、8项Native验收（6项KV／模板及2项注入流程）及原14,357-token Native回归通过ASan/UBSan。开发期JSON流编译、thinking尾部假设与外层沙箱Metal／回环失败单独保留。没有新增性能记录或图表；更大模型命中率、完整任务收益与WSL2/CUDA留待目标机器验收。旧58份证据和41组图不改。'),'',
        tr('[Contract](../design/self-speculation-v1.md), [target-machine handoff](../self-speculation-handoff.md) and [functional evidence](../evidence/self-speculation-20261102.json). The runner preserves all warmups, failed tasks, output-mismatch pairs and regressions, reports explicit rate denominators, probe cost and all-task versus complete/equal-task timings separately.0.6B compatibility does not establish self-prediction quality.',
           '[契约](../design/self-speculation-v1.md)、[异机交接](../self-speculation-handoff.md)和[功能证据](../evidence/self-speculation-20261102.json)。准备好的runner保留全部预热、失败、输出不一致和回退配对，明确命中率分母，并分别展示probe成本、全部任务与完整且输出一致任务的耗时。0.6B兼容验收不代表自预测质量。'),'',
        *cpp_report(e,zh),
        *cc_report(e,zh),
        '## '+tr('E53 — Host-protection baseline and original overhead', 'E53 — 宿主保护基线与原开销'), '',
        tr('Additional review found that both broad and narrowed Seatbelt sysctl rules still exposed an unrestricted parent startup environment. E53 sets a sticky CS_RESTRICT flag on cxf itself, checks the SIP gate, and verifies effective environment omission with a trusted child before enabling restricted commands. CLI does this before configuration/model initialization; direct hosts can initialize early too. No system setting or other application is changed. Only the probe read has a1000ms poll bound; fork/reaping are not a hard deadline. Explicit off in a fresh process is the leaking positive control.',
           '额外审查发现宽泛及收窄的Seatbelt sysctl规则都仍可读取未保护父环境。E53只为cxf自身设置粘性CS_RESTRICT、检查SIP门，并用可信子进程验证环境确实被省略后再启用命令。CLI在读配置／加载模型前完成，直接宿主也可提前初始化。不改系统设置或其他应用；1000ms只是探针读回poll上限，不是fork／回收硬deadline。新进程显式off保留为可见合成标记的正对照。'), '',
        table(tr(['Complete CLI workload','Off / on median ms','Paired extra ms [min,max]','On slower'],['完整CLI负载','关闭／开启中位ms','逐对额外ms [最小,最大]','开启更慢']),[[r['case'],f"{r['off_wall_median_ms']:.3f} / {r['on_wall_median_ms']:.3f}",f"{r['wall_ms_overhead']['median']:.3f} [{r['wall_ms_overhead']['min']:.3f},{r['wall_ms_overhead']['max']:.3f}]",f"{r['on_slower']}/{r['pairs']}"] for r in e['E53']['summary'] if r['kind']=='cli_timing']), '',
        '![Final test.run isolation cost](figures/test-sandbox-final.svg)', '',
        tr('Same workload, new independent experiment:230 rows,44 warmups and93 evaluation pairs. CLI runs in a fresh process and includes host protection; API evaluation is steady state after warmups, where the sticky host flag remains common to later off/on calls. API cold warmup rows are retained. Standard cc regresses2467.548ms paired median,2534.841ms worst; paired inner-command growth median2453.219ms and outside-inner growth median21.003ms are not additive. CLT cost median3.565ms has only4/7 slower pairs, so it is not a guaranteed fixed cost or a proven improvement over E52. Exact cache/IPC effects and cold initialization after a model is already loaded are not isolated.',
           '同工作量、独立新实验：230条记录、44条预热、93组评价配对。CLI每任务新进程，包含宿主保护；API评价是预热后稳态，粘性宿主标记对随后off/on都保持，冷启动预热行仍保留。标准cc逐对中位多2467.548ms、最差2534.841ms；命令内部差值中位2453.219ms、外层差值中位21.003ms，不可相加。CLT中位多3.565ms，只有4/7更慢，不能称固定成本或证明比E52更快。缓存／IPC以及模型已加载后才初始化的冷开销未独立测量。'), '',
        tr('Final acceptance:542 C checks,12 OS-security suites (including effective parent-environment tests), prior12 policy and8 foundation suites under sanitizers,14,357-token Native,16 Stream and72 output cases. The73,880-byte job capacity remains common to both modes; host verification additionally uses at most256KiB transient heap and a trusted fork once per process, not a measured RSS/KV figure. Other same-UID processes are not protected by this flag, argv remains visible, and this is not a VM or universal DLP. Keys must not be supplied as command-line literals. Linux/Windows remain unsupported with default refusal. Previous53 evidence files and38 figure pairs stay unchanged.',
           '最终验收：542项C检查、12组OS安全（含有效父环境对照）、原12组策略／8组基础SAN套件，以及14,357-token Native、16项Stream、72项输出通过。73,880字节任务容量仍为两模式共有；宿主验证另用最多256KiB临时堆和每进程一次可信fork，不是实测RSS/KV。该标记不保护其他同UID进程，argv仍可见，不能把key写进命令行，也不属于VM或通用DLP。Linux/Windows仍无后端、默认拒绝。旧53份证据与38组图保持原字节。'), '',
        tr('All final [samples](data/test-sandbox-host-samples.csv), [pairs](data/test-sandbox-host-pairs.csv), [summary](data/test-sandbox-host-summary.csv), [contract](../design/test-sandbox-v1.md) and [E53 evidence](../evidence/test-sandbox-host-20261101.json). E52 remains below as pre-fix evidence; do not attribute cross-stage timing differences to the security fix.',
           '最终[样本](data/test-sandbox-host-samples.csv)、[配对](data/test-sandbox-host-pairs.csv)、[汇总](data/test-sandbox-host-summary.csv)、[契约](../design/test-sandbox-v1.md)和[E53证据](../evidence/test-sandbox-host-20261101.json)。以下E52是修复前记录，不按跨阶段差值推导修复的性能收益。'), '',
        '## '+tr('E52 — Pre-fix OS isolation and overhead (superseded by E53)', 'E52 — 修复前OS隔离与开销（已由E53取代）'), '',
        tr('Default: workspace content read-only, fresh private temporary writes, and network denied for the command and descendants. Explicit read/write directory and full-network grants are independent; protected names/config/trace remain blocked. Existing file descriptors are closed. Unsupported platforms or preflight/setup failures do not fall back to unrestricted execution. Current backend is macOS Seatbelt; Linux/Windows are pending. Explicit isolation off is the unrestricted compatibility control.',
           '默认工作区正文只读、独立私有临时目录可写、命令及后代禁网。读／写目录和完整联网由启动者独立授权，敏感名称／配置／trace仍拒绝；关闭已有fd。后端不支持或准备／初始化失败不降级到无限制。当前仅macOS Seatbelt，Linux/Windows待做；显式off是取消OS隔离的兼容对照。'), '',
        table(tr(['Complete CLI workload','Off / on median ms','Paired extra ms [min,max]','On slower'],['完整CLI负载','关闭／开启中位ms','逐对额外ms [最小,最大]','开启更慢']),[[r['case'],f"{r['off_wall_median_ms']:.3f} / {r['on_wall_median_ms']:.3f}",f"{r['wall_ms_overhead']['median']:.3f} [{r['wall_ms_overhead']['min']:.3f},{r['wall_ms_overhead']['max']:.3f}]",f"{r['on_slower']}/{r['pairs']}"] for r in e['E52']['summary'] if r['kind']=='cli_timing']), '',
        '![test.run isolation overhead](figures/test-sandbox-overhead.svg)', '',
        tr('Timing is serial and unsanitized:176 command-API rows (8/256 extra fixture files) and54 actual CLI rows, including44 warmups and93 evaluation pairs. CLI uses two scripted HTTP model responses; no inference or real cloud. Positive overhead means isolation is slower. The standard Xcode cc CLI regresses2591.134ms in paired median and5524.787ms worst; paired inner-command growth median2561.604ms, outside-inner growth median17.541ms. These medians are not additive. Warnings show restricted cache/service behavior, but cache versus IPC causes are not isolated. The separate explicit CLT command is measured as a different workload, not silently substituted in production.',
           '计时独占、无SAN：176条命令API记录（8／256个额外语料文件）及54条真实CLI记录，含44条预热和93组评价配对。CLI用两次脚本HTTP模型响应，无推理或真实云端。额外开销为开启−关闭，正值表示更慢。标准Xcode cc完整CLI逐对中位多2591.134ms、最差5524.787ms；命令内部增加的逐对中位2561.604ms，外层差值中位17.541ms，不可直接相加。受限缓存／服务诊断可见，但缓存和IPC成因未独立证明。显式CLT命令作为另一负载实测，没有在生产中偷偷替换编译器。'), '',
        tr('The C job object has a fixed73,880-byte stack capacity in both current off/on paths; this is not added RSS, child peak memory or physical KV. Scanned entries and populated profile bytes are in raw records; files=8/256 counts extra fixtures, not total workspace entries. Time outside the inner command includes setup, sandbox process/application, polling and cleanup; it is not pure Seatbelt compilation time. All API cells and negative pairs are in [samples](data/test-sandbox-samples.csv), [pairs](data/test-sandbox-pairs.csv) and [summary](data/test-sandbox-summary.csv).',
           'C任务结构固定栈容量73,880字节，当前off/on都分配，不能称新增RSS、子进程峰值或物理KV。实际扫描entries及profile实填字节在原始记录；files=8/256是额外语料数，不是工作区全部项。命令外差值包含准备、隔离进程／规则加载、轮询与清理，不等同纯Seatbelt编译。全部API分组与负对见[样本](data/test-sandbox-samples.csv)、[配对](data/test-sandbox-pairs.csv)和[汇总](data/test-sandbox-summary.csv)。'), '',
        tr('Acceptance:542 C checks,11 OS-security suites, prior12 data-policy and8 foundation suites under ASan/UBSan; Native14,357-token,16 Stream and72 output checks. The localhost-only network candidate admitted a non-loopback local interface and was removed: only deny/allow ship. Initial Stream equality failed on timestamped Xcode output; the deterministic fixture explicitly uses CLT while retaining token/fresh-greedy checks. Both failures are retained. Runtime/benchmark code matches the timing archive; the later Stream test-command change is separately identified. Six prior native warnings remain, upstream libraries are not instrumented. This is not a VM or arbitrary-secret DLP; explicit off/inherited secrets/full network grant still require trusted deployment. See [D-EXEC](../design/test-sandbox-v1.md) and [E52 evidence](../evidence/test-sandbox-20261101.json).',
           '验收：542项C检查、11组OS安全套件、原12组数据策略与8组基础套件在ASan/UBSan下通过；Native14,357-token、16项Stream、72项输出通过。候选localhost网络规则放行本机非回环接口，已撤下，只发布deny/allow。首次Stream全文比较受Xcode时间戳输出影响失败，确定性夹具显式用CLT并保留token／fresh-greedy检查；两次失败均保留。运行时／计时程序与归档源码一致，之后的Stream测试命令变化单列。原有6条Native警告保留，上游库未插桩。这不是VM或任意秘密DLP，显式off／凭据继承／完整网络授权仍须可信部署。见[D-EXEC](../design/test-sandbox-v1.md)和[E52证据](../evidence/test-sandbox-20261101.json)。'), '',
        '## '+tr('E51 — Protected files and cloud egress: functional acceptance', 'E51 — 敏感文件与云端外发：功能验收'), '',
        tr('All eight file tools share protected-name, deployed config/trace, descriptor, symlink and hardlink checks. HTTP providers default to unknown scope and cloud egress is denied before connection. Operators explicitly select local/cloud and task/workspace egress; task mode cannot dispatch tools. Tool arguments/results, HTTP requests/responses and decoded model output reject known credentials and common secret patterns. Trace admission/capacity failures preserve existing files. Native prefill has the same content checks.',
           '8个文件工具共用敏感名称、实际配置／trace、目录fd与链接检查。HTTP默认UNKNOWN、云外发在连接前拒绝；启动者显式声明local/cloud及task/workspace，task不派发工具。工具参数／结果、HTTP请求／响应和解码动作拦截已知凭据与常见秘密模式；trace准入／容量失败保留原文件。Native预填充也检查内容。'), '',
        tr('445 policy checks and12 real HTTP/files/process suites pass under ASan/UBSan; the prior eight foundation suites, full default regression,14,357-token Native,16 Stream and72 output checks pass. Normal cloud-authorized read/edit/test and expected access/leak refusals are separate from model quality or performance. The initial added Native test used a stale checkpoint in an assertion; its failed log hash is retained and the corrected test passes. Six existing native harness compiler warnings remain; fixed upstream libraries are not instrumented.',
           '445项策略检查和12组真实HTTP／文件／进程套件在ASan/UBSan下通过；原8组基础套件、完整默认回归、14,357-token Native、16项Stream与72项输出检查通过。正常授权读—改—测与预期拒绝不计模型能力或性能。首次新增Native测试引用旧检查点导致断言失败，保留失败日志哈希，修正测试后通过；6条已有Native harness编译警告保留，固定上游库未插桩。'), '',
        tr('This is bounded admission and common-pattern detection, not universal DLP or an OS sandbox. test.run still has launcher file/network rights; unknown secrets and arbitrary encodings can escape detection. No real cloud calls, direct HTTPS, Windows/CUDA or new speed/memory result. Previous51 evidence files and37 figure pairs are byte-for-byte unchanged. See the [single policy contract](../design/data-policy-v1.md) and [E51 evidence](../evidence/agent-data-policy-20261101.json).',
           '这是有界准入与常见模式检查，不是通用DLP或OS沙箱；test.run仍有启动者文件／网络权限，任意未知秘密或编码可能漏检。未调用真实云端，直接HTTPS、Windows/CUDA与新速度／内存结果均未验收。旧51份证据与37组图逐字节保持。见[唯一策略契约](../design/data-policy-v1.md)和[E51证据](../evidence/agent-data-policy-20261101.json)。'), '',
        '## '+tr('E50 — Agent foundation: functional acceptance', 'E50 — Agent基础能力：功能验收'), '',
        tr('Added an opt-in guarded single-line editor, minimal child-process environment by default in the CLI, and a loopback-only llm.local text helper. Eight scripted two-provider HTTP suites pass under ASan/UBSan, alongside the default regression, edit/process sanitizers,14,357-token Native,16 Stream checks and72 tool-output cases. Helper responses are data, not actions; credentials and task quotas are separate. No new performance or real cloud/model-quality result.',
           '新增仅随写权限挂载的单行匹配编辑、CLI默认最小子进程环境，以及仅回环的llm.local文本辅助。8组双端点HTTP桩验收在ASan/UBSan下通过，默认回归、编辑／进程sanitizer、14,357-token Native、16项Stream和72项工具输出检查通过。辅助结果是数据，不是动作；认证和任务配额独立。没有新增性能或真实端云／模型质量结论。'), '',
        tr('Historical E50 limits (file/egress admission is added in E51): no OS sandbox, universal sensitive-file policy, atomic file CAS, direct HTTPS, automatic routing, cloud SSE or shared Native KV for the HTTP helper. Existing writer paths have not all migrated to descriptor-based traversal. Six pre-existing compiler warnings remain in the native harness; no sanitizer errors were observed. See the [tool contract](../basic-agent.md), [hybrid contract](../design/hybrid-v1.md) and [functional evidence](../evidence/agent-foundation-20261101.json).',
           'E50历史边界（文件／外发准入见E51）：当时尚无OS沙箱、统一敏感文件策略、文件原子CAS、直接HTTPS、自动路由、云SSE或HTTP辅助共享Native KV；旧写入路径尚未全部迁移为目录fd遍历。Native harness保留6条已有编译警告，未发现sanitizer错误。见[工具契约](../basic-agent.md)、[协同契约](../design/hybrid-v1.md)和[功能证据](../evidence/agent-foundation-20261101.json)。'), '',
        '## '+tr('Results at a glance', '关键结果'), '',
        table(tr(['Experiment','Matched comparison','Observed result','Scope'], ['实验','同组对照','观测结果','范围']), [
            ['E49', 'off → existing growth; paced tools', '104.276 / 67.290 ms paired task savings', tr('Seven pairs per fixture, all faster; scripted model actions, resident KV.', '两负载各7对且全快；脚本选动作，KV常驻。')],
            ['E49', 'growth → 1536-byte coarse trial', '54.667 / 3.009 ms paired task regressions', tr('Failed optimization; defaults unchanged. Full burst/marker negatives below.', '失败优化，默认不变；下文保留突发和marker负结果。')],
            ['E47', 'off → thread-only control', '1868.877 ms worst task regression', tr('No tool-window GPU prefill; tool/system variation remains unresolved.', '工具窗没有GPU预填充，工具／系统波动仍未解决。')],
            ['E09', 'normalized_json → jinja_values', '511.7 → 141.3 KiB temporary heap', tr('14,504 logical tokens; n=3. About 370 KiB absolute; RSS unchanged.', '14504 个逻辑 token，n=3；绝对量约370 KiB，RSS未下降。')],
            ['E07–09', '8 branches × 6,000-token prefix', '48,000 logical → 6,000 stored IDs', tr('Shared token pool; physical KV is a separate metric.', '共享token块池，物理KV另计。')],
            ['E46', 'batch capacity 1024 → 640', '597.50 → 373.44 MiB Metal compute reservation', tr('Default512 is298.75MiB; KV remains896MiB; speed can regress.', '默认512为298.75MiB；KV仍896MiB，速度可能回退。')],
        ]), '',
        tr('Paired task savings/regressions, worst pairs and allocation measurements are labeled separately above. Detailed tables retain every comparison and observed range. Whiskers are ranges, not confidence intervals. Gains from different experiments must not be added or multiplied.',
           '上表分别标明任务配对收益／回退、最差负对与分配测量；下文保留全部对照和观测范围。图中须线是范围，不是置信区间；不同阶段收益不能相加或连乘。'), '',
        '## '+tr('Environment and evidence coverage', '环境与证据范围'), '',
        table(tr(['Component','Recorded configuration'], ['组件','已记录配置']), [
            ['Host', 'Apple M5 · 16 GiB unified memory · macOS arm64 · Metal'],
            ['llama.cpp', '`b10938` · `f1e44dcc11d8802d107bd7331a3d3fd3e6f57b93`'],
            ['Model', '`Qwen3-0.6B-Q8_0.gguf` · 654,302,880 file bytes'],
            ['Model SHA-256', '`710d7b1aa4e2ae03238a9e65e82f48cfa70edf501becae8979c35acf5830e1ed`'],
            ['Template SHA-256', '`57f1fd00f0013a2be96aa79b857391f27e23df5b5f847072b524c897e24d0361`'],
            ['Execution', '4 CPU threads · batch 512 · FP16 KV · warm runs'],
            ['KV allocation', 'E01–02: 3328 tokens / 364 MiB; E05–06, E10–11, E13–19: 8192 / 896 MiB; E07–09: 16384 / 1792 MiB'],
        ]), '',
        tr(f'{len(catalog)} evidence files are indexed below. {raw_files} contain {total} raw records, including metadata, correctness, file timelines and cost-estimation samples; this is **not {total} independent latency samples**. Functional, sanitized-summary and reported-only evidence remain separate. Source hashes identify the measured revisions.',
           f'下表索引 {len(catalog)} 份证据。其中 {raw_files} 份共含 {total} 条原始记录，包含元数据、正确性、文件时间线和耗时估算样本，**不等于 {total} 个独立延迟样本**。功能验收、脱敏摘要与仅文档读数分开。源码哈希对应各自实测版本。'), '',
        table(['ID', tr('Experiment','实验'), tr('Evidence class','证据等级'), tr('Records','记录数')],
              [[c['id'], f"[{c['title']}](../../{c['file'].removeprefix('docs/')})".replace('../../evidence/', '../evidence/'), c['evidence_level'], c['raw_records']] for c in catalog]), '',
        '## '+tr('E01–02 · direct API feasibility', 'E01–02 · 直接 API 可行性'), '',
        tr('E01 starts from the same 2,048-token warm prefix and appends 128 tokens six times. The fair comparison is against existing prefix reuse (`cached_scan`). The cold run deliberately recomputes all history and is an upper-bound control.',
           'E01 从相同的 2048-token 暖前缀出发，六次各追加 128 token。主要基线是已有前缀复用的 `cached_scan`。`cold` 故意重复计算全部历史，只是上界对照。'), '',
        table(['Mode', 'Frontend ms', 'Total ms', 'Tokenized', 'Evaluated', 'n'], [
            [mode, cell('E01','frontend_ms',kind='prefix',mode=mode), cell('E01','wall_ms',kind='prefix',mode=mode),
             int(stat('E01','tokenized_tokens',kind='prefix',mode=mode)['median']), int(stat('E01','evaluated_tokens',kind='prefix',mode=mode)['median']), 5]
            for mode in ['cold','cached_scan','explicit_delta']]), '',
        tr('A standalone scan of 2,688 token IDs took 0.649 µs per scan in a 20,000-iteration microbenchmark. Avoiding retokenization and materialization matters more than removing this scan at this scale. Max logit difference was zero in the recorded prefix/stream/fork/state controls; a changed-token negative control changed logits by 15.410506, and explicit rollback again matched a fresh run.',
           '独立微测中扫描 2688 个 token ID，20000 次循环的每次耗时为 0.649 µs。在这个尺度，避免重复分词与物化比省去扫描更重要。前缀／流式／分支／状态对照的最大 logit 差为 0；修改 token 的负对照产生 15.410506 差值，显式回退后重新匹配全新计算。'), '',
        table(['ID / gap', 'Mode', 'Local compute ms', 'After-input ms', 'Total ms', 'n'], [
            [f'{eid} / {gap} ms',mode,cell(eid,'compute_ms',kind='stream',gap_ms=gap,mode=mode),
             cell(eid,'post_input_ms',kind='stream',gap_ms=gap,mode=mode),cell(eid,'wall_ms',kind='stream',gap_ms=gap,mode=mode),5 if eid=='E01' else 3]
            for eid,gap in [('E01',0),('E01',20),('E02',200)] for mode in ['wait_all','coalesced_128','eager_32']]), '',
        tr('These runs replay already-tokenized arrivals: 512 new tokens in 32-token chunks. They do not include cloud transport, chat rendering, or generation. E02 has raw samples but no recorded benchmark source hash/snapshot. With no arrival gap, eager 32-token decoding is 2.24× slower than waiting. A longer source window can hide preparation, but cannot save more absolute work than the remaining critical path.',
           '这里回放的是已经分词的输入：512 个新 token，每段 32 个。不含云端传输、聊天模板或生成。E02 保留原始样本，但没有当时的源码哈希／快照。无输入间隔时，32-token 急切解码比等待慢 2.24 倍。较长输入窗口有利于隐藏准备工作，但绝对节省量仍受关键路径限制。'), '',
        table(['Operation', 'Time ms / bytes', 'n'], [
            ['Share 2048-token prefix (`seq_cp`)', cell('E01','fork_ms',precision=3,kind='branch'), 5],
            ['Recompute same prefix', cell('E01','recompute_prefix_ms',kind='branch'), 5],
            ['Shared prefix + 128-token suffix', cell('E01','fork_then_suffix_ms',kind='branch'), 5],
            ['Recompute full branch', cell('E01','recompute_full_ms',kind='branch'), 5],
            ['Export in-memory state', cell('E01','export_ms',kind='state'), 5],
            ['Restore in-memory state', cell('E01','restore_ms',kind='state'), 5],
            ['Serialized state size', '234,906,296 bytes (224.024 MiB)', 5],
        ]), '',
        tr('Prefix sharing uses llama.cpp’s existing same-pool sequence-copy API. In-memory state restore is a different experiment from E10 file restore. Removing a sequence did not shrink the preallocated 364 MiB KV buffer or measured RSS.',
           '前缀共享使用 llama.cpp 已有的同池序列复制 API。内存状态恢复与 E10 文件恢复是不同实验。删除序列没有缩小预分配的 364 MiB KV buffer，实测 RSS 也未下降。'), '',
        '## '+tr('E05–06 · chat frontend', 'E05–06 · 聊天前端'), '',
        tr('Each sample totals six tool-result appends. E05 conservatively replays three boundary tokens per round: 606 versus 588 evaluated tokens. Template rendering still sees the full transcript. E06 adds cached parsing/preparation of the verified template; it is a separate same-run comparison, not a cumulative speedup against E01.',
           '每个样本统计六轮工具结果追加。E05 每轮保守重算 3 个边界 token，所以 evaluated 为 606 对 588；模板仍完整渲染历史。E06 进一步缓存已验收模板的解析准备，它是独立的同组对照，不能和 E01 叠加计算。'), '',
        table(['ID', 'Prefix tokens', 'Mode', 'Frontend ms', 'Total ms', 'Tokenized / evaluated', 'n'], [
            [eid,int(stat(eid,'prefix_tokens',prefix_lines=prefix,mode=mode)['median']),mode,
             cell(eid,'front_ms',prefix_lines=prefix,mode=mode),cell(eid,'wall_ms',prefix_lines=prefix,mode=mode),
             f"{int(stat(eid,'tokenized',prefix_lines=prefix,mode=mode)['median'])} / {int(stat(eid,'evaluated',prefix_lines=prefix,mode=mode)['median'])}",5]
            for eid,modes in [('E05',['full_cached','native_checkpoint']),('E06',['full_cached_control_0','full_cached_control_1','native_checkpoint','native_prepare_cache'])]
            for prefix in [16,96] for mode in modes]), '',
        tr('E05 does not establish a stable overall speedup. E06’s 1,225-token case reduced frontend median by 80.7%, while total median fell 7.8% (376.843 → 347.468 ms) with overlapping observed ranges. This is warm preparation, not complete generation TTFT.',
           'E05 尚不能证明稳定的整体提速。E06 的 1225-token 档前端中位数下降 80.7%，总耗时中位数下降 7.8%（376.843→347.468 ms），观测范围仍重叠。这是暖态准备耗时，不是完整生成 TTFT。'), '',
        '## '+tr('E07–09 · context storage and temporary memory', 'E07–09 · 上下文存储与临时内存'), '',
        tr('Canonical token history is stored in one cxf block pool; branches reference shared prefixes. Raw source text, token IDs, and KV are distinct required representations. The whole pipeline is not a single physical copy. At 14,504 logical tokens, 14,511 IDs are physically retained (seven boundary-tail IDs), with 59,392 bytes of token capacity and 65,260 bytes of cxf allocations.',
           '规范 token 历史存于一个 cxf 块池，分支引用共享前缀。原始文本、token ID 与 KV 是不同的必要表示，不能宣称整条链路只有一份物理数据。14504 个逻辑 token 对应实际保存 14511 个 ID（含 7 个边界尾部 ID）、59392 字节 token 容量与 65260 字节 cxf 分配。'), '',
        table(['State', 'Logical token references', 'Stored IDs', 'Token capacity bytes', 'cxf bytes', 'New decode tokens'], [
            ['8 shared branches',48000,6000,24576,30444,0],
            ['Each appends 32 tokens',48256,6256,31744,37612,256],
            ['All sessions closed',0,0,0,0,'—'],
        ]), '',
        tr('The 48,000 references fit in a shared 16K pool; this is not a measured comparison against eight independent KV allocations. That 16K FP16 KV pool remains 1,792 MiB (112 KiB per configured token for this model). Token storage is small compared with KV.',
           '48000 个引用可放在共享的 16K 池中；没有实测八套独立 KV 分配作为对照。16K FP16 KV 池仍为 1792 MiB（该模型每个配置 token 为 112 KiB）。token 存储远小于 KV。'), '',
        '![Temporary heap and preparation trade-offs](figures/frontend-memory.svg)', '',
        tr('Temporary heap = max(0, sampled process malloc peak − heap before the prepare call). macOS 200 µs sampling plus stage probes can miss short peaks; n=3, warm repeated prepares. It excludes existing model/KV and is neither total request peak nor VRAM. Memory and timing trials are separate.',
           '临时堆 = max(0，prepare 调用中的采样进程 malloc 峰值 − 调用前堆)。macOS 以 200 µs 采样并插入阶段探针，可能漏掉短峰值；n=3、暖态重复准备。不含已存在的模型／KV，不是完整请求峰值或显存。内存与计时分开采样。'), '']
    for eid,modes,desc in [
        ('E07',['old_lifetimes','early_release'],tr('Release request/DOM/message temporaries earlier.', '提前释放请求／DOM／消息临时对象。')),
        ('E08',['request_json','direct_messages'],tr('Borrowed messages; baseline already has early release.', '借用消息；基线已经提前释放临时对象。')),
        ('E09',['normalized_json','jinja_values'],tr('Combined direct Jinja values, direct output collection, and bounded tokenizer scratch.', '直接 Jinja 值、直接输出收集、有界分词缓冲的组合改动。'))]:
        lines += ['### '+eid, '', desc, '',
            table(['Shape / size', 'Result', 'Mode', 'Extra heap KiB', 'Prepare wall ms', 'n'], [
                [f'{shape} / {size}', 'OK' if stat(eid,'rc',kind='memory',shape=shape,size=size,mode=mode)['median']==0 else 'FULL (expected rejection)',mode,
                 cell(eid,'heap_extra_bytes',scale=1024,kind='memory',shape=shape,size=size,mode=mode),
                 cell(eid,'wall_ms',kind='timing',shape=shape,size=size,mode=mode) if samples(e,eid,kind='timing',shape=shape,size=size,mode=mode) else 'not timed',3]
                for shape,size in sorted({(r['shape'],r['size']) for r in samples(e,eid,kind='memory')}) for mode in modes]), '']
    lines += [tr('Sizes for single_text are fixture word counts, not actual model tokens; tool_turns sizes count tool records. FULL rows are rejected inputs, never successful long-context acceptance. E08’s long-text heap worsened (304.6 → 321.9 KiB); E09’s 7.3K-token heap also worsened (67.1 → 84.0 KiB), and its 64K-word rejection uses 64 KiB more scratch. E09’s long-text prepare time was 34.581 → 35.085 ms, with no stable overall speedup. Whole RSS stayed around 2480 MiB: the 72.4% reduction concerns about 370 KiB of temporary heap only.',
        'single_text 的 size 是夹具单词数，不是实际模型 token 数；tool_turns 是工具记录条数。FULL 行是被拒绝输入，不能算成功长上下文验收。E08 长文本临时堆反而增加（304.6→321.9 KiB）；E09 的 7.3K-token 档也增加（67.1→84.0 KiB），64K-word 拒绝档多占 64 KiB 缓冲。E09 长文本准备耗时 34.581→35.085 ms，没有稳定的整体提速。整体 RSS 约 2480 MiB，72.4% 降幅仅涉及约 370 KiB 的临时堆。'), '',
        '## '+tr('E10 · earlier KV restore', 'E10 · 提前恢复 KV'), '',
        tr('Restore the same evicted prefix from a private sequence file either after assembly (`late_hint`) or concurrently with assembly / a controlled wait (`early_hint`). n=7 per mode. Same live engine, session, revision, and canonical token pool; snapshot token_count=0. OS file cache is warm; storage is not a cold-SSD benchmark.',
           '同一前缀被逻辑逐出后，从私有序列文件恢复。`late_hint` 在上下文装配后恢复；`early_hint` 与装配／受控等待重叠。每种模式 n=7；同一存活引擎、会话、修订号与 token 池，快照 token_count=0。OS 文件缓存暖态，不是冷 SSD 基准。'), '',
        table(['Words / new tools / history / window', 'Mode', 'Assembly ms', 'Actual overlap ms', 'After-submit prepare ms', 'After-submit first token ms', 'Total first token ms', 'n'], [
            [f'{w} / {r} / {h} / {gap} ms',mode,
             *[cell('E10',field,kind='timing',words=w,records=r,history_records=h,window_ms=gap,mode=mode)
               for field in ['assembly_ms','overlap_ms','post_submit_prepare_ms','post_submit_first_token_ms','total_first_token_ms']],7]
            for w,r,h,gap in [(4096,1,0,0),(1024,1,128,0),(1024,128,0,0),(4096,1,0,25),(4096,1,0,100),(4096,1,0,250)]
            for mode in ['late_hint','early_hint']]), '',
        tr('Short plain assembly (~0.26 ms) did not improve total latency (80.635 → 81.361 ms). Existing dense history improved median (135.265 → 125.488 ms), but only 5/7 pairs were faster and early max was 286.886 ms. Heavy new prefill worsened (2757.792 → 2858.619 ms). At the synthetic 100 ms window, actual overlap was 51.206 ms [45.734–58.911]. Total median differences also include variable restore durations; they are not pure overlap savings.',
           '普通装配约 0.26 ms，总延迟未改善（80.635→81.361 ms）。已有密集历史的中位数改善（135.265→125.488 ms），但只有 5/7 对更快，提前组最大值达 286.886 ms。大量新 prefill 的一组反而变慢（2757.792→2858.619 ms）。100 ms 人工窗口中，实际重叠为 51.206 ms [45.734–58.911]。两组恢复耗时本身有波动，总中位数差不能全部归因于重叠。'), '',
        tr('The 4,257-token snapshot is 465.66 MiB; observed saves cost 59.6–157.6 ms, excluded from restore-stage comparisons. No fsync, durable recovery, external snapshot import, physical KV-buffer shrinking, or automatic eviction prediction is claimed. Queue/ticket/snapshot limits and cancellation/stale-revision guards are tested.',
           '4257-token 快照为 465.66 MiB，观测保存成本为 59.6–157.6 ms，恢复阶段对照不含这部分。不宣称 fsync 持久化、崩溃恢复、外部快照导入、物理 KV buffer 缩容或自动逐出预测。队列／票据／快照上限和取消／过期修订保护已测试。'), '',
        '## '+tr('E11 · streaming input, prefill and file tools', 'E11 · 流式输入、预填充与文件工具'), '',
        '![Streaming preparation results](figures/stream-latency.svg)', '',
        tr('Six controlled cases × three modes × seven repeats = 126 timing samples, plus 63 file timelines and two status records. Source: 16 immutable semantic units + an end control, wire chunks ≤7 bytes. End arrives immediately after the last payload; there is no extra EOF gap. Initial 538-token context/model load are excluded. Each payload is a complete local user-message unit, not an arbitrary append into one message.',
           '6 个受控场景 × 3 种模式 × 7 次重复 = 126 个计时样本，另有 63 条文件时间线与 2 条状态记录。输入为 16 个不可变语义单元加结束帧，线缆分块不超过 7 字节。结束帧紧跟最后一段，无额外 EOF 间隔。模型加载与初始 538-token 上下文不计入。每段是独立、完整的本地 user-message 单元，不是随意追加进同一消息。'), '',
        tr('`wait_all`: wait for complete source/file callbacks, then prepare. `stream_coalesced`: prepare complete units with a 128-token minimum batch. `stream_eager`: flush smaller updates. All modes hold final event boundaries and input token counts equal. File case: three C-source chunks, file_end at sequence 4, then 12 text units; the deployment-fixed test command compiles and runs the complete file.',
           '`wait_all`：等完整输入／文件处理后再准备。`stream_coalesced`：完整单元按最小 128-token 批量预填充。`stream_eager`：更小更新即刷新。三种模式的最终事件边界与输入 token 数相同。文件场景先传三段 C 源码、序号 4 的 file_end，再传 12 段文本；部署时固定的测试命令编译并运行完整文件。'), '',
        table(['Scenario / gap', 'Mode', 'Total ready ms', 'After-input ready ms', 'Local prefill ms', 'Input decode calls', 'n'], [
            [f'{scenario} / {gap} ms',mode,
             cell('E11','total_ready_ms',kind='timing',scenario=scenario,gap_ms=gap,mode=mode),
             cell('E11','post_input_ready_ms',kind='timing',scenario=scenario,gap_ms=gap,mode=mode),
             cell('E11','prefill_ms',kind='timing',scenario=scenario,gap_ms=gap,mode=mode),
             cell('E11','input_decode_calls',kind='timing',scenario=scenario,gap_ms=gap,mode=mode),7]
            for scenario in ['text_prefill','complete_file_test'] for gap in [0,20,100]
            for mode in ['wait_all','stream_coalesced','stream_eager']]), '',
        tr('With a 100 ms gap, the file tool finished before input end in all seven streaming runs, and in none of the wait-all runs. At 20 ms it did not finish before end, but some execution was hidden. Coalesced mode was faster than wait-all in all seven pairs for text/file cases at 20/100 ms; seven pairs do not establish p95/p99 or operational stability. No-gap text did not improve. No-gap file timing is noisy, including a coalesced maximum of 1,300.267 ms.',
           '100 ms 间隔时，两种流式模式的七次文件工具均在输入结束前完成，wait_all 均未提前完成。20 ms 时工具尚未提前完成，但执行已部分重叠。文本／文件在 20／100 ms 档，合批模式均为 7/7 对更快；七对不能证明 p95/p99 或运行稳定性。无间隔纯文本未改善；无间隔文件波动很大，合批组最大值达 1300.267 ms。'), '',
        '![Latency versus execution trade-off](figures/stream-tradeoff.svg)', '',
        tr('For pure text at 100 ms, all modes evaluate 701 input tokens. Input decode calls rise 2 → 6 → 16; cumulative local prefill execution rises 158.956 → 259.494 → 396.539 ms while post-input wait falls 161.341 → 35.166 → 28.473 ms. The latency gain buys overlap with more small-batch execution; there is no FLOP/energy measurement. Some file/render wall times are large and variable; contention has not been profiled.',
           '纯文本 100 ms 档三种模式均计算 701 个输入 token。输入 decode 调用由 2→6→16，累计本地 prefill 执行由 158.956→259.494→396.539 ms，同时输入后等待降为 161.341→35.166→28.473 ms。延迟改善伴随更多小批次执行；未测 FLOP／能耗。部分文件／渲染墙钟时间很大且波动，尚未剖析争用原因。'), '',
        tr('“Ready” means full source + valid current-version receipts + assistant header have KV, before the final callback. The callback performs a test-only full-tokenization oracle and at most 16 preview tokens; first-token/output fields include that oracle overhead and are not summary-quality or full-generation benchmarks. All such fields remain available in the raw data and metrics CSV.',
           '“Ready” 指完整输入、当前有效版本收据和 assistant header 都已形成 KV，尚未进入最终回调。回调包含测试用全量分词 oracle 和最多 16-token 预览；first-token／output 字段含 oracle 开销，不是总结质量或完整生成基准。所有字段仍保留在原始数据与指标 CSV 中。'), '',
        '## '+tr('Correctness, capability and untested scope', '正确性、能力与未验证项'), '',
        table(['Category', 'Result', 'Evidence / scope'], [
            ['Native context/chat', 'PASS', 'E03–04, E07–11; full-token and greedy oracles, rollback, budgets, failure recovery'],
            ['Scripted tool executor', 'PASS', 'E04; read → compile FAIL → write → compile PASS → read; actions supplied by test'],
            ['KV worker / stream', 'PASS', 'E10–11; cancellation, stale versions, bounded queues, cleanup, file feedback before end'],
            ['CLI and default regression', 'PASS', 'E11; replay 6 cases, native 5 cases, make test; expected nonzero exits are intentional'],
            ['ASan / UBSan', 'PASS (partial instrumentation)', 'cxf/adapters instrumented; prebuilt llama/common/Metal libraries excluded'],
            ['Real model: one file read', 'PASS, n=1', 'E04; two calls, receipt-based answer'],
            ['Real model: instructed C repair', 'FAIL, n=1', 'E04; FORMAT at first action, zero tools dispatched'],
            ['Historical HTTP C repair', 'FAIL, two individual probes', 'E12; sanitized summaries; earlier protocol, no independently passing final program'],
            ['Windows/CUDA, live cloud SSE, automatic agentctl integration', 'NOT VALIDATED', 'Host/replay interfaces only; second machine acceptance pending'],
            ['Physical KV release, cold SSD, tail stability, general coding/summarization', 'NOT VALIDATED', 'Do not extrapolate the latency/temporary-heap results'],
        ]), '',
        tr('See [validation.csv](data/validation.csv) for individual gates. No global “pass percentage” combines model capability, expected rejection tests, and implementation correctness.',
           '逐项结果见 [validation.csv](data/validation.csv)。不把模型能力、预期拒绝测试和实现正确性合成一个“总通过率”。'), '',
        '## '+tr('E13 · calibrated service-time estimation','E13 · 本机耗时测算'), '',
        tr('A standalone 3,120-byte C estimator predicts warm-prefill service time using batch-call count, new tokens and attention-pair work proxies. The machine/runtime/model fingerprint chooses a profile; hardware names do not supply measured throughput. No inference or scheduling behavior changes.',
           '独立 C 估算器固定占 3120 字节，用批次调用数、新 token 和 attention-pair 工作量特征估计暖态 prefill。机型／运行时／模型指纹用于隔离 profile，不凭机型填写吞吐；未改变推理或调度行为。'), '',
        tr('63 calibration samples: prefixes 0/2048/6144 × new tokens 32/128/512 × 7. Evaluation uses 84 samples on different shapes: prefixes 0/1024/4096 × 16/64/256/768 × 7. The fixed token/s baseline uses all calibration samples. Frozen uses no evaluation labels; online predicts each row before observing it. Each tested shape is warmed outside timing; model load and prefix setup are excluded.',
           '63 条标定：前缀 0/2048/6144 × 新 token 32/128/512 × 7。84 条评价使用不同形状：前缀 0/1024/4096 × 16/64/256/768 × 7。固定 token/s 基线也使用全部标定数据；冻结模型不用评价标签；在线每条先预测再观察。各形状预热、模型加载和前缀建立不计入服务时间。'), '',
        '![Prediction error and online correction](figures/cost-estimation.svg)', '',
        table(['Mode','Group','n','MAE ms','WAPE %','MAPE %','Max abs error ms','Signed bias ms'],[
            [r['mode'],r['group'],r['samples'],f"{r['mae_ms']:.3f}",f"{r['wape_percent']:.2f}",f"{r['mape_percent']:.2f}",f"{r['max_abs_error_ms']:.3f}",f"{r['mean_signed_error_ms']:.3f}"]
            for r in e['E13']['prediction_summary']]), '',
        tr('MAE is mean absolute error; WAPE = sum absolute errors / sum actual times; MAPE averages per-sample relative errors. Positive signed bias means overestimated duration, which would make a deadline policy start earlier. Online correction is prequential learning, not an entirely untouched holdout. “Inside domain” is only a range check, not proof of accuracy; half the evaluation rows are outside the frozen token-size range.',
           'MAE 为平均绝对误差；WAPE=绝对误差总和/实际耗时总和；MAPE 为逐样本相对误差的平均。正 bias 表示耗时高估，用于提前量会更早启动。在线纠正会学习先前评价样本，属于先预测后学习；不是全程不接触评价数据。域内只按范围判断，不保证准确；评价中一半超出冻结模型的新 token 范围。'), '',
        table(['Prefix','New tokens','Actual ms median [min–max]','Fixed / frozen / online MAE ms','n'],[
            [p,n,cell('E13','wall_ms',scenario='evaluation',prefix_tokens=p,new_tokens=n),
             ' / '.join(f"{statistics.mean(r[m+'_abs_error_ms'] for r in samples(e,'E13',scenario='evaluation',prefix_tokens=p,new_tokens=n)):.2f}" for m in ['fixed','frozen','online']),7]
            for p in [0,1024,4096] for n in [16,64,256,768]]), '',
        tr('Estimator overhead, measured separately (10 batches; timings per call):','估算器开销单独测量（10 组，折算每次调用）：'), '',
        table(['Operation','µs median [min–max]'],[
            ['predict',cell('E13','predict_us',precision=4,kind='estimator_overhead')],
            ['observe/refit',cell('E13','observe_us',precision=4,kind='estimator_overhead')]]), '',
        tr('The model-structure formula gives 114,688 bytes (112 KiB) of FP16 KV payload per token: 28 layers × 8 KV heads × (128 K + 128 V) × 2 bytes. This is model-specific payload, not whole-process memory. The rolling window holds 64 numeric observations and does no heap allocation. Remaining maximum errors require explicit scheduling margins; lower prediction error is not measured inference speedup. Cold start, restore I/O, queue time, agent ETA, arbitrary-program complexity and Windows/CUDA remain unmeasured.',
           '模型结构公式给出每 token 114688 字节（112 KiB）的 FP16 KV payload：28 层×8 KV heads×(128 K+128 V)×2 字节。这是该模型的 payload，不是进程内存。窗口最多 64 条数值观测，无堆分配。仍有较大的最大误差，后续调度必须留余量；误差降低不等于已实测推理加速。冷启动、恢复 I/O、排队、Agent ETA、一般程序复杂度和 Windows/CUDA 均未测算。'), '',
        tr('Independent C unit tests, ASan/UBSan and default regression passed. Real-model checks cover canonical tokens, exact new-token/batch counts and 21 fresh-KV greedy shape comparisons. See [D-COST](../design/cost-estimator-v1.md); raw predictions and failures are preserved in E13.',
           '独立 C 单测、ASan/UBSan 与默认回归通过；真实模型核对 canonical token、准确新增 token／batch 数，以及 21 种形状的从头 KV greedy 结果。见 [D-COST](../design/cost-estimator-v1.md)，逐条预测与反例保留在 E13。'), '',
        '## '+tr('E14 · hardware Roofline estimation','E14 · 硬件 Roofline 公式对照'), '',
        tr('The standard GEMM 2MNK count and per-operation max(F/P, B/BW) Roofline are now implemented as an optional C estimator. The Qwen3 GGUF supplies actual matrix dimensions and Q8_0 bytes. New measurements use the same 63/84 calibration/evaluation grids as E13, with five predictions per measured job. E13 remains immutable; comparisons below are within E14 only.',
           '新增可选纯 C 估算器，采用标准 GEMM 的 2MNK 和逐算子 max(F/P, B/BW) Roofline；Qwen3 GGUF 提供实际矩阵尺寸及 Q8_0 字节。新测量沿用 E13 的 63/84 标定／评价网格，每个实测任务同时比较五种预测。E13 保持原样；下面只在 E14 内作比较。'), '',
        table(['Same GGML/Metal primitive','Effective rate','Samples'],[
            ['Q8_0 GEMM K1024 / N3072 / M512',f"{stat('E14','linear_flops_s',kind='hardware_profile')['median']/1e12:.3f} TFLOP/s",7],
            ['F16 GEMM K128 / N4096 / M512',f"{stat('E14','attention_flops_s',kind='hardware_profile')['median']/1e12:.3f} TFLOP/s",7],
            ['64 MiB copy (read + write)',f"{stat('E14','bandwidth_bytes_s',kind='hardware_profile')['median']/1e9:.3f} GB/s",7]]), '',
        tr('These are synchronized wall-clock microprobe rates after two warmups, not vendor peak specifications. GEMM FLOPs omit quantization instruction work. F16 GEMM is only an attention-throughput proxy; actual fused attention has different utilization and IO. Traffic assumes one KV pass per chunk; normalization, RoPE and activation work are omitted. This is not a verified physical latency lower bound. Sources: [GEMM](https://docs.nvidia.com/deeplearning/performance/dl-performance-matrix-multiplication/index.html), [Roofline](https://docs.nvidia.com/nsight-compute/ProfilingGuide/index.html#roofline-charts), [FlashAttention IO](https://arxiv.org/abs/2205.14135).',
           '有效速率来自两轮预热后的同步墙钟微测，不是厂商峰值。GEMM FLOPs 未计量化指令；F16 GEMM 仅代理 attention 吞吐，实际融合 kernel 的利用率和 IO 不同。访存假设每 chunk 一次 KV 读取，省略 norm、RoPE 和激活，因此不是已验证的物理耗时下界。来源：[GEMM](https://docs.nvidia.com/deeplearning/performance/dl-performance-matrix-multiplication/index.html)、[Roofline](https://docs.nvidia.com/nsight-compute/ProfilingGuide/index.html#roofline-charts)、[FlashAttention IO](https://arxiv.org/abs/2205.14135)。'), '',
        '![Raw and calibrated Roofline errors](figures/roofline-estimation.svg)', '',
        table(['Mode','Group','n','MAE ms','WAPE %','MAPE %','Max abs error ms','Signed bias ms'],[
            [r['mode'],r['group'],r['samples'],f"{r['mae_ms']:.3f}",f"{r['wape_percent']:.2f}",f"{r['mape_percent']:.2f}",f"{r['max_abs_error_ms']:.3f}",f"{r['mean_signed_error_ms']:.3f}"]
            for r in e['E14']['prediction_summary']]), '',
        tr('Raw Roofline is substantially worse: the current combination of rate and traffic/operator proxies does not transfer directly to inference; individual error sources have not been isolated. Calibration uses three nonnegative coefficients for linear time, attention time and decode calls, trained only on the same 63 labels as the original frozen three-parameter model. MAE 5.281 vs 5.526 ms is a small single-run difference, not evidence of a stable advantage. Original online prediction remains better at 3.931 ms, but learns prior evaluation labels. The raw negative result is retained; no held-out tuning or replacement run was used.',
           '原始 Roofline 明显更差，表明当前速率与流量／算子近似组合不能直接预测，尚未拆分各误差来源。校准用 linear 耗时、attention 耗时、decode 调用数三个非负系数，只使用与原三参数冻结模型相同的 63 条标签。MAE 5.281 对 5.526 ms 只是单次运行的小差异，不能说明有稳定优势。原在线预测仍更好，为 3.931 ms，但会学习先前评价标签。保留原公式的负结果，没有按评价数据调参或换轮次挑成绩。'), '',
        tr('One-chunk raw estimation overhead: ','单 chunk 原公式估算开销：')+cell('E14','estimate_us',precision=4,kind='roofline_overhead')+' µs; '+tr('10 batches of 5,000 calls. Calibrated prediction/refit overhead was not separately timed. Calibration state is 3,120 bytes per model; no heap or context copy. All 21 fresh-KV greedy shape checks, canonical tokens, exact token/batch counts, unit tests, ASan/UBSan and default regression passed. Scheduling is unchanged; lower prediction error is not inference speedup.',
           '10 组，每组 5000 次。校准预测／refit 的开销本轮未单独计时；每份校准状态 3120 字节，无堆分配或上下文副本。21 形状从头 KV greedy、规范 token、准确 token／batch 计数、单测、ASan/UBSan 和默认回归通过。未改变调度；预测误差降低不等于推理提速。'), '',
        '## '+tr('E15 · diagnosing and correcting the raw formula','E15 · 原公式偏差排查与修正'), '',
        tr('An independent Python recomputation matches all 147 E14 FLOP/byte/time records: no 2MNK arithmetic or unit-conversion defect was found. Source inspection of pinned Qwen3 shows a real work-model omission: the final layer selects requested output rows before its FFN. The new optional last_layer_ffn_tokens field uses 1 for this native path; zero retains legacy behavior and requires callers to recompile. No inference code or scheduling was changed.',
           '独立 Python 复算与 E14 的147条 FLOP／字节／时间记录一致，未发现 2MNK 算术或单位换算错误。固定版本 Qwen3 源码确认实际算量描述有缺口：最后一层在 FFN 前选择输出行。新增可选 last_layer_ffn_tokens，当前原生路径显式传1；零值保留旧口径，调用者需重编译。没有修改推理执行或调度。'), '',
        tr('Rate diagnosis uses dependent graphs of depths 1 and 16, distinct weight/KV buffers per stage, two warmups and seven retained synchronized wall times. Rate = extra work / extra median duration; overlapping depth ranges reject the probe. Q8/F16 matmuls alternate transposed dimensions. Attention now uses dense ggml_flash_attn_ext with actual head-interleaved strides, F32 Q/accumulation, FP16 KV, Hq16/Hkv8, dk=dv128, prefix2048/new512 and n_kv_max=0. A development probe incorrectly set a sparse hint; it was corrected before the formal run and remains excluded in the inventory.',
           '速率诊断采用深度1／16的依赖图，每级独立 weight／KV buffer，预热2次后保留7次同步墙钟时间。速率=增加工作量/中位耗时差，深度观测范围相交则拒绝探针。Q8／F16 矩阵乘法交替转置尺寸；attention 改用实际 head-interleaved stride、F32 Q／累积、FP16 KV、Hq16／Hkv8、dk=dv128、prefix2048／new512、n_kv_max=0 的 dense ggml_flash_attn_ext。首个开发探针误设 sparse 提示，正式运行前已修正，开发记录保留并排除。'), '',
        table(['Rate (decimal units)','Initial isolated wall rate','Repeated after graph probes','Dependent-graph slope'],[
            ['Q8 GEMM TFLOP/s',f"{stat('E15','linear_flops_s',kind='hardware_profile')['median']/1e12:.3f}",f"{stat('E15','linear_flops_s',kind='warm_hardware_profile')['median']/1e12:.3f}",f"{stat('E15','linear_flops_s',kind='graph_profile')['median']/1e12:.3f}"],
            ['F16 GEMM TFLOP/s',f"{stat('E15','attention_flops_s',kind='hardware_profile')['median']/1e12:.3f}",f"{stat('E15','attention_flops_s',kind='warm_hardware_profile')['median']/1e12:.3f}",f"{stat('E15','effective_rate',kind='graph_rate',mode='f16_chain')['median']/1e12:.3f}"],
            ['Copy read+write GB/s',f"{stat('E15','bandwidth_bytes_s',kind='hardware_profile')['median']/1e9:.3f}",f"{stat('E15','bandwidth_bytes_s',kind='warm_hardware_profile')['median']/1e9:.3f}",f"{stat('E15','bandwidth_bytes_s',kind='graph_profile')['median']/1e9:.3f}"]]), '',
        tr('Dense FlashAttention slope: ','Dense FlashAttention 斜率：')+f"{stat('E15','attention_flops_s',kind='graph_profile')['median']/1e12:.3f} TFLOP/s; "+tr('logical causal FLOPs per second, not physical peak throughput. Q8 depth1/depth16 medians are 0.514/5.203 ms: the slope implies 0.3126 ms/op and a 0.2014 ms graph intercept. Later repetition of the exact old probe is also much faster. These establish graph-size and order effects, but without GPU clock/temperature telemetry they do not establish throttling or any single frequency cause. The F16 and FlashAttention rates represent different kernels and cannot be interchanged.',
           '按逻辑 causal FLOPs 归一化，不是物理峰值。Q8深度1／16中位0.514／5.203ms，斜率给出0.3126ms/算子、截距0.2014ms/图；稍后重复完全相同的旧探针也明显更快。这证明图规模与测量顺序有影响，但没有GPU频率／温度遥测，不能指认为降频或某个单一频率原因。F16与FlashAttention是不同kernel，速率不可互换。'), '',
        '![Roofline diagnostic ablations](figures/roofline-diagnosis.svg)', '',
        table(['E15 mode (same 84 jobs)','MAE ms','WAPE %','Max abs error ms','Signed bias ms'],[
            [r['mode'],f"{r['mae_ms']:.3f}",f"{r['wape_percent']:.2f}",f"{r['max_abs_error_ms']:.3f}",f"{r['mean_signed_error_ms']:.3f}"]
            for r in e['E15']['prediction_summary'] if r['group']=='all']), '',
        tr('legacy remeasures the E14-style isolated rates; math_only changes final FFN work; bandwidth_only/linear_only/attention_only each replace just that rate; rate_only replaces all rates while retaining old work; corrected combines all changes. All variants share each measured job. The large error is mainly the compute-rate/operator mismatch, not bandwidth. Rate-only MAE is 13.845 ms; adding the semantically correct FFN count slightly worsens it to 14.220 ms, revealing compensating errors. Calibrated corrected uses only 63 calibration labels and the same three coefficients as before: 9.133 ms vs original frozen 10.117 ms, while original prequential online remains better at 6.434 ms. No held-out tuning or run selection; these are single-run differences, not stable speedups.',
           'legacy在本轮重测E14式孤立速率；math_only只修最后FFN算量；bandwidth_only／linear_only／attention_only各只替换一项速率；rate_only换全部速率但保留旧算量；corrected组合修正。全部共享每个实测任务。大偏差主要来自算力速率／算子错配，带宽不是主因。仅速率修正MAE13.845ms，加入语义正确的FFN算量后略变差至14.220ms，表明存在误差补偿。校准修正版仅用63条标定及原有三个系数，MAE9.133ms，原frozen为10.117ms，原先预测后学习online仍更好，为6.434ms。没有按评价集调参或挑运行；单轮差异不是稳定提速证明。'), '',
        table(['Corrected estimator overhead (per call)','µs median [min–max]'],[
            ['raw estimate',cell('E15','estimate_us',precision=4,kind='roofline_overhead')],
            ['calibrated predict',cell('E15','calibrated_predict_us',precision=4,kind='roofline_overhead')],
            ['calibrated observe/refit',cell('E15','calibrated_observe_us',precision=4,kind='roofline_overhead')]]), '',
        tr('C units, ASan/UBSan and default regression passed before timing; 21 fresh-KV greedy shape oracles, canonical tokens and exact token/batch checks passed. E15 contains 278 records, including 56 depth probes and 42 isolated probes. Real DRAM traffic, pure GPU timing and frequency are unmeasured; synthetic payloads, a single rate shape, small-batch vec kernels, padding, cache and omitted work remain limitations. Corrected raw max error is still 54.856 ms. E13/E14 are preserved at their measured source revisions; current hashes are E15. Lower prediction error is not faster model inference.',
           'C单测、ASan/UBSan和默认回归在计时前通过；21形状从头KV greedy、规范token和精确token／batch核对通过。E15共278条，含56条深度探针、42条孤立探针。真实DRAM流量、纯GPU时间和频率仍未测；合成数据、单一速率形状、小batch vec kernel、padding、缓存和省略算子仍是限制。修正原公式最大误差仍54.856ms。E13／E14保持当时版本，当前源码哈希看E15；预测误差降低不是模型推理提速。'), '',
        '## '+tr('E16–17 · batch/context residuals and fair online comparison','E16–17 · batch／上下文拆分与公平在线对照'), '',
        tr('The earlier frozen Roofline versus online empirical comparison used different information. Post-hoc numeric replay of E15 gives Roofline online MAE 5.641 ms versus frozen 9.133 and empirical online 6.434; this is historical replay, not new inference. E16/E17 now compare empirical, single-shape Roofline and shape-profile predictors under both frozen and identical prequential updates. Every online model gets the same previous labels and 64-slot window; each predicts before observing the current job.',
           '先纠正训练信息口径：此前冻结Roofline与原online使用的信息不同。E15事后数值回放中，Roofline online MAE为5.641ms，冻结为9.133、原online为6.434；这是历史回放，不是新推理。E16／E17现将经验、单形状Roofline和形状表均分冻结／公平在线版本；所有在线模型使用相同先前标签与64槽窗口，每条先预测再观察。'), '',
        tr('Each run measures batch limits 512 then 128 independently, 63 calibration and 147 evaluation jobs per limit (n=7 per cell), with 60 fresh-KV shape oracles. Batch here means tokens per native decode call, not concurrent requests. E16 evaluates prefixes 0/1024/4096 and new tokens 8/16/19/20/64/256/768. E17 uses new prefixes 512/3072/5120 and 12/18/24/48/192/384/640 after the E16 interpolation diagnosis. Both retain calibration prefixes 0/2048/6144 and sizes 32/128/512; full-job evaluation shapes do not overlap calibration.',
           '每轮独立测batch上限512后128，各63条标定、147条评价，每格7次，共60个从头KV形状oracle。这里batch指一次原生decode的token宽度，不是并发请求数。E16评价prefix0/1024/4096、新token8/16/19/20/64/256/768；E17在E16诊断后换用prefix512/3072/5120、新token12/18/24/48/192/384/640。两轮均保留标定prefix0/2048/6144及新token32/128/512，完整评价工作形状与标定不重叠。'), '',
        tr('Evidence for a kernel boundary: pinned Metal uses the FA vec path below 20 query tokens. E16 independent per-layer FA slopes at prefix2048 drop from 0.4678 ms (19 tokens) to 0.1978 ms (20). Full-model observations show the same discontinuity; a smooth token/pair formula cannot represent it. Route labels are source-inferred, not GPU counters. Q8 kernels also have a small-width path through 8 tokens.',
           'kernel边界有两组证据：固定Metal在query少于20时走FA vec路径；E16独立单层FA探针在prefix2048由19-token的0.4678ms降到20-token的0.1978ms。完整模型也出现同方向突变，平滑token／pair公式无法描述。路由标记来自源码推定，不是GPU计数器；Q8在宽度不超过8时也有小batch路径。'), '',
        table(['E16 batch cap','Prefix','19-token actual ms [min–max]','20-token actual ms [min–max]'],[
            [b,p,cell('E16','wall_ms',scenario='evaluation',batch_tokens=b,prefix_tokens=p,new_tokens=19,precision=3),cell('E16','wall_ms',scenario='evaluation',batch_tokens=b,prefix_tokens=p,new_tokens=20,precision=3)]
            for b in [512,128] for p in [0,1024,4096]]), '',
        '![Batch boundary and interpolation regression](figures/cost-shape-boundary.svg)', '',
        tr('The optional 960-byte table profiles six widths (8/16/19/20/128/512) and three contexts (0/2048/6144) before model labels, with 336 depth probes and 24 rate rows per run. It interpolates within kernel regimes and advances context per chunk. E16 interpolated inverse linear rate, then multiplied by token work: this artificially curves the time between knots. It helped narrow queries but badly overpredicted width64; the negative result is preserved. E17 instead interpolates normalized operator time width/rate, then converts back to rate. FA already interpolated time. New shape tests were fixed before the E17 run.',
           '可选表固定960字节，模型标签到来前测6个宽度（8/16/19/20/128/512）和3个context（0/2048/6144），每轮336条深度探针、24条速率记录；按kernel区间插值并逐chunk推进context。E16先插值linear倒速率，再乘token工作量，会人为弯曲节点间时间；小宽度改善，但width64明显高估，负结果保留。E17改为先插值归一化算子时间width/rate，再换回速率；FA原本就插值时间。新评价形状在E17运行前冻结。'), '',
        table(['Run','Batch cap','Mode','n','MAE ms','WAPE %','Max abs error ms','Signed bias ms'],[
            [eid,r['batch_tokens'],r['mode'],r['samples'],f"{r['mae_ms']:.3f}",f"{r['wape_percent']:.2f}",f"{r['max_abs_error_ms']:.3f}",f"{r['signed_bias_ms']:.3f}"]
            for eid in ['E16','E17'] for r in e[eid]['prediction_summary']]), '',
        '![New-shape validation](figures/cost-shape-v2.svg)', '',
        tr('E17 online shape MAE is 25.6% lower than empirical online at cap512 and 42.2% lower at cap128, within the same jobs and observation policy. Single-shape Roofline online also beats empirical online in this run. This is not a universal advantage: E16 shape online regressed, and E17 frozen shape calibration is worse than raw shape and single-shape frozen in both caps. Extra profiling and calibration transfer remain costs; the table alone does not solve runtime drift.',
           'E17在相同任务及观察口径下，形状online的MAE相对原online在cap512降低25.6%、cap128降低42.2%；本轮单形状Roofline online也优于原online。这不是普遍优势：E16形状online回退；E17冻结形状校准在两个cap都差于形状raw及单形状冻结。额外探测和校准迁移仍有成本，表本身不能解决运行漂移。'), '',
        tr('All 42 prefix × new-token × cap cells per run, including signed bias and actual median/min/max/MAD, are in [cost-shape-errors.csv](data/cost-shape-errors.csv). Whiskers/MAD describe variation, not confidence intervals. Context sensitivity, width-dependent kernel changes, chunk-count overhead, and frozen/online information explain different parts of the residual; resource contention is not identified without telemetry. Neither E17 gain nor cross-run differences are inference speedups.',
           '每轮全部42个prefix×new-token×cap单元的方向误差及实际中位／最小／最大／MAD见 [cost-shape-errors.csv](data/cost-shape-errors.csv)。须线／MAD描述波动，不是置信区间。上下文敏感性、宽度kernel切换、chunk次数开销及冻结／在线信息差异分别影响残差；缺少遥测仍不能指认争用。E17改善或跨轮差异均不是推理提速。'), '',
        table(['E17 cost','Recorded value'],[
            ['Shape-table construction',cell('E17','build_ms',kind='shape_table',precision=3)+' ms; '+tr('before inference labels','在推理标签之前')],
            ['Table + one numeric calibration', '960 + 3120 = 4080 bytes; '+tr('no text/token/KV copy','不复制原文／token／KV')],
            ['Shape predict µs / cap512',cell('E17','predict_us',kind='shape_overhead',batch_tokens=512,precision=4)],
            ['Shape observe/refit µs / cap512',cell('E17','observe_us',kind='shape_overhead',batch_tokens=512,precision=4)]]), '',
        tr('Related primary research: [Vidur](https://www.microsoft.com/en-us/research/wp-content/uploads/2024/05/vidur_mlsys24.pdf) separates token-level and context-sensitive operators and uses profiled shape models; [FlashAttention-2](https://arxiv.org/abs/2307.08691) explains how work partitioning and occupancy affect effective throughput; [IBM Roofline + regression](https://research.ibm.com/publications/predicting-llm-inference-latency-a-roofline-driven-ml-method) combines structural estimates with learned runtime overhead. These motivate the method; CUDA/cloud results do not establish Metal accuracy. No random-forest implementation was imported.',
           '相关一手研究：[Vidur](https://www.microsoft.com/en-us/research/wp-content/uploads/2024/05/vidur_mlsys24.pdf)分开token算子与上下文敏感算子并使用形状实测模型；[FlashAttention-2](https://arxiv.org/abs/2307.08691)解释工作划分／并行度如何影响有效吞吐；[IBM Roofline＋回归](https://research.ibm.com/publications/predicting-llm-inference-latency-a-roofline-driven-ml-method)组合结构估算与运行开销校准。这些支持方法选择，CUDA／云端结果不能证明Metal准确率；未引入随机森林实现。'), '',
        tr('Both 916-record runs retain all failures/regressions and passed C units, ASan/UBSan, default regression before timing and 60 shape oracles each. E16 wrapper lacked the re import after native success; metadata was recovered from unchanged raw records without rerunning inference, and measurement/processor hashes are separate. E17 completed directly. Current estimator hashes are E17; E16 source hashes describe its superseded interpolation. Default agent scheduling, Windows/CUDA, real cloud streaming and pure GPU/DRAM/clock/temperature profiling remain unvalidated.',
           '两轮各916条，保留失败／回退；C单测、ASan/UBSan、默认回归先于性能通过，各60形状oracle通过。E16 wrapper在native成功后缺re导入，从未改的raw恢复元数据，没有重跑推理，测量／processor哈希分列；E17直接完成。当前估算器哈希看E17，E16哈希对应已被修正的插值。默认Agent调度、Windows/CUDA、真实云流以及纯GPU／DRAM／频率／温度分析仍未验收。'), '',
        '## '+tr('E18–19 · why frozen calibration regresses','E18–19 · 冻结校准回退排查'), '',
        tr('The E17 development audit rules out insufficient optimizer iterations: exhaustive active-set NNLS and the recorded 128-pass fit differ by at most 6.6e-9 in coefficients, with matching training SSE. Before fitting, 512-token jobs account for 92.4% / 94.6% of raw squared residuals at caps512/128. Same-label weighted NNLS helps on E17, and a new three-feature residual basis helps more; these are development observations, not the new validation.',
           'E17开发审计先排除优化器迭代不足：穷举active-set NNLS与原128轮拟合的系数最大差不超过6.6e-9，训练SSE一致。拟合前raw残差的平方量中，512-token任务在cap512／128分别占92.4%／94.6%。E17同标签加权NNLS有所改善，新的三特征残差basis改善更多；这些只作开发分析，不能代替本轮新验证。'), '',
        tr('E19 fixes the policies before measuring unseen caps256/64, prefixes1280/3328/5376 and new tokens10/15/21/72/176/352/672. Both sparse and diverse coverage use exactly63 labels per cap and interleave in each prefix. Sparse is the old 3×3 grid; diverse uses nine points covering small/vec/dense paths. The shape table adds context7168 so all calibration chunks are covered. Clipped-control predictions use the same first three knots; evaluation chunks are all within6144, so raw evaluation features are exactly equal and only training coverage differs.',
           'E19在策略冻结后测试未测过的cap256／64、prefix1280/3328/5376、新token10/15/21/72/176/352/672。sparse／diverse各用63条标签，在每个prefix内交错采样；sparse保留旧3×3网格，diverse九个点覆盖small／vec／dense路径。形状表新增context7168，覆盖所有标定chunk；旧端点对照保留前三个节点。评价chunk均在6144以内，两种表的评价raw特征完全相同，差异仅来自标定表覆盖。'), '',
        tr('The new optional C residual model keeps the measured raw prior and fits calls, new_tokens/512 and raw_attention_ms/100. It uses signed coefficients, weight1/max(actual_ms,20), weighted-RMS feature normalization, ridge0.01 around zero residual, and clamps final duration to zero. Like the old model it has three parameters and64 numeric observations. The same-basis weighted NNLS controls are computed offline from training labels only, stored separately with raw-record references; they are not extra inference samples.',
           '新增可选C残差模型保留raw先验，拟合calls、new_tokens/512、raw_attention_ms/100；使用有符号系数、权重1/max(actual_ms,20)、加权RMS归一化、围绕零残差的ridge0.01，最终耗时截0。与旧模型同为3参数、64条数值观测。同basis的加权NNLS对照仅从标定标签离线计算，单列原始记录引用，不增加推理样本。'), '',
        table(['E19 batch cap','Mode','n','MAE ms','WAPE %','Max abs error ms','Signed bias ms'],[
            [r['batch_tokens'],r['mode'],r['samples'],f"{r['mae_ms']:.3f}",f"{r['wape_percent']:.2f}",f"{r['max_abs_error_ms']:.3f}",f"{r['signed_bias_ms']:.3f}"]
            for r in e['E19']['prediction_summary']]), '',
        '![Coverage and model controls](figures/frozen-calibration.svg)', '',
        tr('The cause is mixed. At cap256, broader equal-label coverage reduces legacy MAE5.331→4.486 ms; changing to the residual model with that coverage gives3.452 ms. Weighting alone does not consistently help. Table extension helps cap64 (4.728→2.678) but regresses at cap256 (4.399→5.331), revealing compensating errors. At cap64, raw2.183 ms remains better than every frozen calibration, including new residual/diverse2.645 ms. More coverage does not establish that three global correction coefficients generalize.',
           '原因是混合的。cap256下，等量扩覆盖使旧模型MAE5.331→4.486ms，在该覆盖上换残差模型为3.452ms；仅改权重没有一致收益。补表覆盖在cap64改善4.728→2.678，但cap256反而4.399→5.331，说明存在误差补偿。cap64的raw2.183ms仍优于全部冻结校准，包括新residual/diverse的2.645ms；补覆盖并不能保证三个全局修正系数泛化。'), '',
        tr('Training error also separates fitting from generalization: at cap64/sparse, raw training MAE1.792 ms drops to1.045 with residual fitting, yet held-out error grows from raw2.183 to2.664 ms. All training metrics are in [frozen-calibration-training.csv](data/frozen-calibration-training.csv), and all42 validation cells ×10 modes in [frozen-calibration-errors.csv](data/frozen-calibration-errors.csv). Frozen variants never use evaluation labels; online baselines do, after each prediction.',
           '训练误差进一步区分了拟合与泛化：cap64／sparse的raw训练MAE1.792ms，残差拟合降到1.045；但新形状误差从raw2.183升到2.664ms。全部训练指标见 [frozen-calibration-training.csv](data/frozen-calibration-training.csv)，42个验证单元×10种方案见 [frozen-calibration-errors.csv](data/frozen-calibration-errors.csv)。冻结方案不使用评价标签；online基线仅在每次预测之后学习。'), '',
        table(['Cap','Anchor prefix/new','Before calibration ms','After calibration ms','After evaluation ms'],[
            [b,f'{p}/{n}',cell('E19','wall_ms',scenario='anchor',batch_tokens=b,prefix_tokens=p,new_tokens=n,anchor_stage='before_calibration',precision=3),
             cell('E19','wall_ms',scenario='anchor',batch_tokens=b,prefix_tokens=p,new_tokens=n,anchor_stage='after_calibration',precision=3),
             cell('E19','wall_ms',scenario='anchor',batch_tokens=b,prefix_tokens=p,new_tokens=n,anchor_stage='after_evaluation',precision=3)]
            for b in [256,64] for p,n in [(0,32),(2048,17),(6144,512)]]), '',
        tr('Anchors are three repetitions per stage and excluded from all fitting. At cap256 the long anchor rises363.472→377.057 ms, while the narrow anchor is nearly unchanged; cap64 long-anchor change is smaller. Phase variation therefore exists and is workload-dependent, but without clock/temperature/DRAM/GPU timestamps it cannot identify a hardware cause. Coverage and model effects are not additive, and cross-cap runs are sequential.',
           '锚点每阶段3次，全部排除在拟合外。cap256长锚点363.472→377.057ms，而窄锚点基本不变；cap64长锚点变化较小。阶段波动确实存在且依赖工作量，但没有频率／温度／DRAM／GPU时间遥测，不能指认硬件原因。覆盖与模型效果不可相加，两种cap也是顺序运行。'), '',
        table(['E19 numeric cost','Value'],[
            ['Residual state + table','3632 + 960 = 4592 bytes; no heap/context copy'],
            ['Extended-table construction',cell('E19','build_ms',kind='shape_table',precision=3)+' ms'],
            ['Predict including shape lookup / cap256 µs',cell('E19','predict_us',kind='residual_overhead',batch_tokens=256,precision=4)],
            ['Residual observe/refit / cap256 µs',cell('E19','observe_us',kind='residual_overhead',batch_tokens=256,precision=4)]]), '',
        tr('Residual ridge uses the standard [L2-regularized least-squares structure](https://scikit-learn.org/stable/modules/generated/sklearn.linear_model.Ridge.html), implemented here in standalone C with a direct3×3 Cholesky solve, without scikit-learn. Analytic collinear/zero-column, signed correction, window/drift and invalid-state tests, ASan/UBSan and default regression passed before GPU measurement;78 fresh-KV workload shapes passed token/greedy/batch checks. Current source hashes are E19. E18 preserves all849 rows of the incomplete first run: cap256 finished, then cap64 was rejected because default prefill minimum128 exceeded batch64. E19 corrects only that test setting to min(128,batch); calibration strategy was not retuned or selected by accuracy. E19 has1162 native records plus294 separately referenced offline controls. No model-inference or default scheduler behavior changed.',
           '残差ridge采用标准 [L2正则最小二乘结构](https://scikit-learn.org/stable/modules/generated/sklearn.linear_model.Ridge.html)，以独立C的3×3 Cholesky直接求解，未引入scikit-learn。共线／零列手算、有符号修正、窗口／漂移与失败原子性单测、ASan/UBSan、默认回归在GPU前通过；78种fresh-KV工作形状的token／greedy／batch核对通过。当前源码哈希看E19。E18保留首次未完成运行的全部849条：cap256完成后，cap64因默认prefill阈值128超过batch64被拒绝；E19仅修测试设置为min(128,batch)，没有重调校准策略或按准确率选运行。E19为1162条native记录＋294条独立引用的离线对照；未改变模型推理或默认调度。'), '',
        '## '+tr('E20 · optional residual Kalman', 'E20 · 可选残差 Kalman'), '',
        tr('A new C layer tracks scalar residual bias with 40 bytes and no heap/context. Read-only prediction does not advance Q; only successful nonzero observation updates state, using the original unfiltered base. Profile changes require a separate/reset state. Fixed Q/R from steady-state initialization is mathematically identical to EWMA; both are checked rather than advertised as distinct accuracy gains.',
           '新C层以40B状态跟踪标量残差偏移，不存heap／上下文。predict只读，不推进Q；仅成功非零任务的observe更新，且必须使用原始未过滤base。profile变化必须独立／重置状态。固定Q/R、稳态初始化的Kalman与EWMA数学等价，本轮验证等价，不把它们宣传为两种独立增益。'), '',
        tr('Fresh native measurements use cap256/64, prefixes1792/3840/5504 and new tokens11/18/26/88/208/400/688. The existing sparse/diverse calibration and expanded microtable are retained. Noise parameters come only from nine diverse training groups: constant R is median within-group variance with a .25 ms² floor; scaled R is a nonnegative intercept + squared-duration fit. Q=.01R and the initialization were fixed before evaluation. All layers start with zero bias at evaluation, predict before observing each label, and use the same chronological past jobs within each base. This is numeric C replay of new measurements, not live-agent scheduling or extra independent inference samples.',
           '新测量用cap256/64、prefix1792/3840/5504及new11/18/26/88/208/400/688，沿用sparse/diverse标定和扩展微表。噪声参数仅由diverse九组标定估计：constant R为组内方差中位数、下限.25ms²；scaled R对截距＋耗时平方做非负拟合。Q=.01R和初值先于评价固定。各层从评价bias0开始，严格先预测后观测，在同一base内使用相同过去任务。这里是新实测的数值C回放，未接真实Agent调度，也不增加独立推理样本。'), '',
        table(['Cap','Base','Base MAE ms','EWMA = fixed MAE ms','Scaled R MAE ms','Base RMSE ms','Scaled RMSE ms','Base max ms','Scaled max ms'],[
            [b,base,*[f'{kresult(b,base,mode,key):.3f}' for mode,key in [('base','mae_ms'),('fixed','mae_ms'),('scaled','mae_ms'),('base','rmse_ms'),('scaled','rmse_ms'),('base','max_abs_error_ms'),('scaled','max_abs_error_ms')]]]
            for b in [256,64] for base in ['raw','residual_diverse','shape_online']]), '',
        '![Residual Kalman comparison](figures/kalman-cost.svg)', '',
        tr('On the current shape-online base, fixed/EWMA MAE improves3.173→3.073 ms at cap256 (3.2%) and1.975→1.922 ms at cap64 (2.7%): a small observed correction, not a broad accuracy breakthrough. Fixed/EWMA beats scaled R in five of six base/cap comparisons. At cap256/shape-online, scaled R slightly worsens maximum error9.582→9.778 ms; even fixed improves aggregate MAE while worsening10 of21 cell means at each cap. The workload order repeats each shape seven times, so these gains do not establish the effect on a real heterogeneous Agent sequence.',
           '在当前shape-online基线上，fixed／EWMA的MAE为cap256的3.173→3.073ms（3.2%）和cap64的1.975→1.922ms（2.7%），属于本轮小幅修正。fixed／EWMA在六组base/cap中的五组优于scaled R。cap256/shape-online的scaled R最大误差略恶化9.582→9.778ms；即便fixed平均改善，每个cap仍有10/21格的平均误差变差。测试按形状各连续重复七次，不能据此保证真实异构Agent任务序列同样收益。'), '',

        tr('All 24 comparisons, signed bias and cold first14 errors are in [kalman-summary.csv](data/kalman-summary.csv); all504 per-cell/mode rows are in [kalman-errors.csv](data/kalman-errors.csv). The882 indexed layer records in [kalman-replay.csv](data/kalman-replay.csv) reference294 original evaluation jobs. They are excluded from the1162 native-record count and from native-record totals. No Q/R was retuned on evaluation and negative results remain.',
           '全部24项对照、方向偏差和冷状态首14项误差见 [kalman-summary.csv](data/kalman-summary.csv)，504项逐格／方案误差见 [kalman-errors.csv](data/kalman-errors.csv)。[kalman-replay.csv](data/kalman-replay.csv) 的882条层记录索引引用294个原评价任务，不算进1162条native或原始记录总数。未用评价重调Q/R，负结果保留。'), '',
        tr('A Gaussian noise model does not itself guarantee smaller MAE. Standard Kalman optimality relies on the model/noise assumptions and concerns squared error; see [MathWorks estimation documentation](https://www.mathworks.com/help/fusion/ug/introduction-to-estimation-filters.html). The following normalized innovation diagnostics are descriptive. The nominal ±1.96sqrt(Pprior+R) interval is not a validated 95% guarantee; correlated workload residuals and model bias can invalidate it.',
           '引入Gaussian噪声模型本身不保证MAE降低。Kalman最优性依赖模型／噪声假设，针对平方误差，参见 [MathWorks估计文档](https://www.mathworks.com/help/fusion/ug/introduction-to-estimation-filters.html)。以下标准化innovation为描述性诊断；名义±1.96sqrt(Pprior+R)区间不代表已验证的95%保证，工作量残差相关和模型偏差都会破坏它。'), '',
        table(['Cap','Base','Filter','Coverage %','z SD','Skew','Excess kurtosis','Lag1'],[
            [r['batch_tokens'],r['base'],r['mode'],*[f'{r[key]:.3f}' for key in ['interval_coverage_percent','normalized_sd','skew','excess_kurtosis','lag1']]] for r in e['E20']['diagnostics']]), '',
        tr('Observed nominal coverage is only15.65–70.07%. Calibration group jitter omits cross-shape structural error, and normalized innovation SD is2.64–11.85 rather than1. Both constant-R estimates hit the .25ms² floor. The scaled fit is [1.9193,0] at cap256 (it becomes another constant-noise filter) and [0,5.5676] at cap64. This supports underestimated/misspecified covariance; low coverage alone does not prove non-Gaussian shape. Do not use this uncertainty as a readiness guarantee. Keep the layer optional and validate on real sequences before default integration.',
           '名义区间实测覆盖仅15.65–70.07%。标定组内抖动未覆盖跨形状的结构误差，标准化innovation SD为2.64–11.85而非1。两组constant R都触及.25ms²下限；scaled拟合cap256为[1.9193,0]，实际退化成另一固定噪声滤波，cap64为[0,5.5676]。这支持协方差低估／模型失配，覆盖率低本身不能证明分布形状非正态。不能把当前不确定度用于就绪保证；保持可选，默认接入前先验收真实任务序列。'), '',
        table(['Layer-only cost','Median [min–max]'],[
            ['C state bytes',40],['Predict µs',cell('E20','predict_us',kind='filter_overhead',precision=4)],
            ['Observe µs',cell('E20','observe_us',kind='filter_overhead',precision=4)]]), '',
        tr('Analytic Joseph/gain, EWMA equivalence, step, single-spike propagation and failure-atomicity tests, ASan/UBSan and full default regression passed before timing. All78 fresh-KV native shapes passed canonical token/greedy/batch oracles; C layer predictions/covariance/gain and calibration-only noise are independently replay-checked in Python. Current measured source hashes are E20; prior evidence remains immutable. Only optional estimator behavior changed, with no inference-kernel, default-scheduler or Windows/CUDA speed claim.',
           '手算Joseph/gain、EWMA等价、step、单尖峰传播及失败原子性测试、ASan/UBSan和完整默认回归先于计时通过。78种fresh-KV形状的规范token／greedy／batch oracle通过；C层预测／协方差／gain及仅标定噪声由Python独立回放核对。当前实测源码哈希看E20；旧证据保持不变。本轮只增加可选估算层，没有推理内核、默认调度或Windows/CUDA提速结论。'), '',
        '## '+tr('E21 · actual tool-window preparation', 'E21 · 真实工具窗口准备'), '',
        tr('Simple EWMA now controls an opt-in native CLI test.run wrapper and the same callable host module. Native borrows the worker-owned engine; every engine operation stays on one owner thread. The tool executes once with its original authorization, args and receipt. A checkpoint retains the verified pre-header canonical prefix. No second model/context or persistent transcript is created. This is a serial tool-window slice, not the full P0–P4 coordinator.',
           '简单EWMA已接到可选原生CLI的test.run包装和同一可调用宿主模块。Native借用worker拥有的engine，所有引擎操作仍在同一owner线程。工具按原权限／参数执行一次，使用真实收据；checkpoint保留已证明的generation-header之前规范前缀。不新建第二个模型／context或持久对话副本。这是串行工具窗口切片，不是完整P0–P4协调器。'), '',
        tr('The benchmark runs real fs.read and deployed C compiler/test processes. Each task has three measured calls; compilation tests fail, apply a real full-file correction, then pass twice. The same real receipts drive independent full-template/token and fresh-KV greedy references. Actions are deterministic to isolate scheduling from 0.6B decision quality. Native CLI smoke-probe results are separated from timing.',
           '性能组实际执行fs.read和部署的C编译／测试进程。每任务三次计时调用；编译测试先失败，实际写入完整修正后再通过两次。同一真实收据通过独立完整模板／token和fresh-KV greedy参考核对。动作确定性选择，用于隔离调度与0.6B决策能力；Native CLI的单次模型探测与计时分列。'), '',
        tr('Resident KV is the ordinary warm baseline. Recompute explicitly evicts KV but retains canonical tokens; restore actually saves a same-live-session private file, then evicts it. Both nonresident states compare after-tool, immediate and EWMA-timed preparation. Timed delay=max(0, tool_hat − prefix_tokens×prepare_ms_per_token_hat −5ms); alpha=.0951249. Initial estimates use the same three real calibration samples per operation/case, then each mode learns only its own past labels. Replicates rotate mode order. No artificial sleeps create performance windows.',
           'resident保留KV，是普通暖态基线；recompute显式逐出KV、保留规范token；restore实际保存同一存活会话的私有文件再逐出。两种非驻留状态分别比较工具后、立即、EWMA定时准备。定时延迟=max(0,工具预计耗时−prefix_tokens×准备ms/token估计−5ms)，alpha=.0951249。初值使用相同的三次真实标定样本，之后各方案只学习自己的过去标签；重复轮转方案顺序。性能窗口没有用sleep填充。'), '',
        table(['Case / history words','Mode','n tasks','Task ms [min–max]','After-tool wait sum ms','Tool sum ms','Save sum ms','Early-hold sum ms'],[
            [f"{scenario} / {words}",mode,7,*[cell('E21',field,kind='task_timing',phase='evaluation',scenario=scenario,words=words,mode=mode,precision=2) for field in ['task_ms','post_tool_sum_ms','tool_sum_ms','save_sum_ms','early_hold_sum_ms']]]
            for scenario in ['read','compile_test'] for words in [128,2048] for mode in ['resident','prefill_late','prefill_immediate','prefill_timed','restore_late','restore_immediate','restore_timed']]), '',
        '![Real tool window comparison](figures/tool-prepare.svg)', '',
        tr('Task time includes checkpoint/save/eviction, real tools, correction write, final rendering/suffix prefill and first greedy. Only model load, initial history preparation, calibration and independent oracle work are excluded; initial-history and oracle costs remain recorded. After-tool wait is summed over three calls; it is not the whole task. Full pairwise differences are in [tool-prepare-pairs.csv](data/tool-prepare-pairs.csv), complete tasks in [tool-prepare-tasks.csv](data/tool-prepare-tasks.csv), and the compact overview in [tool-prepare-summary.csv](data/tool-prepare-summary.csv). All931 native records include144 calibration records and784 evaluation records plus metadata/acceptance/closure; multiple metrics do not multiply tasks.',
           '任务耗时包含checkpoint／保存／逐出、真实工具、修正写入、最终渲染／后缀预填充及首greedy；仅排除模型加载、初始历史准备、标定及独立oracle工作，后两类成本仍记录。工具后等待为三次调用之和，不是完整任务耗时。全部逐对差异见 [tool-prepare-pairs.csv](data/tool-prepare-pairs.csv)，完整任务见 [tool-prepare-tasks.csv](data/tool-prepare-tasks.csv)，概览见 [tool-prepare-summary.csv](data/tool-prepare-summary.csv)。931条native含144条标定、784条评价及元数据／验收／关闭；多项指标不是额外独立任务。'), '',
        table(['Case / words','Compared with','Mode','Task saving median ms','Positive task pairs /7','Wait-sum saving median ms','Positive wait pairs /7'],[
            [f"{r['scenario']} / {r['words']}",r['baseline'],r['mode'],f"{r['task_ms']['median_saving_ms']:.3f}",r['task_ms']['positive_pairs'],f"{r['post_tool_sum_ms']['median_saving_ms']:.3f}",r['post_tool_sum_ms']['positive_pairs']]
            for r in e['E21']['paired_summary'] if r['mode'].endswith('_timed')]), '',
        tr('For long-history compiler tests, three-call after-tool wait medians fall from1759.905 to631.885ms with timed recomputation (64.1%) and242.040 to150.610ms with timed restore (37.8%). Complete task medians are3242.604→2091.362ms and2058.327→1878.756ms respectively; the always-resident baseline remains1582.892ms. Timed long restore has94.834ms median paired wait saving (7/7 positive), with214.356ms median paired task saving (7/7). Paired medians differ from subtracting unpaired medians.',
           '长历史编译测试中，三次调用的工具后等待中位合计：定时重算1759.905→631.885ms（64.1%），定时恢复242.040→150.610ms（37.8%）。完整任务分别3242.604→2091.362ms与2058.327→1878.756ms；一直驻留基线仍为1582.892ms。长历史定时恢复的逐对等待节省中位94.834ms（7/7为正）、任务节省214.356ms（7/7为正）；逐对中位数不等于两个独立中位数相减。'), '',
        tr('Timing is a latency/holding tradeoff: long-history immediate restore leaves119.848ms wait versus150.610ms timed, while timed cuts ready-prefix early holding from1077.405 to18.483ms across three calls. This measures completed-prefix idle time, not the full KV-cell occupancy integral or physical memory release. The short-history recomputation comparison also worsens complete task1555.155→1609.734ms when using timed instead of immediate. Fast reads have less than1ms total tool time and no material hidden wait; their complete-time differences are not evidence of useful overlap.',
           '定时存在等待／占位权衡：长历史立即恢复的剩余等待119.848ms，定时为150.610ms；但定时把三次PREFIX_READY后的提前空等从1077.405降至18.483ms。这只是完成前缀的空等时间，不是完整KV占用积分或物理内存释放。短历史重算从立即改定时还使完整任务1555.155→1609.734ms变慢。快速读取的工具总时不足1ms，没有实质隐藏窗口，其任务总时波动不能当作重叠收益。'), '',
        table(['Compiler history words','Restore mode','Total preparation sum ms','Native restore sum ms','Outside-native sum ms'],[
            [r['words'],r['mode'],*[f"{r[key+'_median']:.3f}" for key in ['prepare_sum_ms','native_restore_sum_ms','outside_native_sum_ms']]] for r in tool_restore_components(e['E21'])]), '',
        tr('Preparation includes owner queue/verification/finish/host coordination, not just native transfer+sync. Immediate restore has282–293ms outside the native restore call across three tools, versus under1ms for timed. Long-history native restore also grows124.116→299.346ms when immediate. The trial does not separately timestamp queue, fork suspension or wakeup costs, nor measure bandwidth/frequency, so this residual is unattributed coordination delay, not proof of slower disk or bus bandwidth. See [tool-prepare-restore-components.csv](data/tool-prepare-restore-components.csv).',
           'prepare包含owner排队／校验／finish／宿主协调，不只是native恢复和设备同步。立即恢复在native恢复调用之外的三轮合计约282–293ms，定时不足1ms；长历史native恢复本身也由124.116增至299.346ms。该轮未独立记录队列、fork暂停或唤醒成本，未测带宽／频率，因此这部分只能记为未归因协调等待，不能据此声称磁盘或总线带宽下降。详见 [tool-prepare-restore-components.csv](data/tool-prepare-restore-components.csv)。'), '',
        tr('The production0.6B CLI smoke probe exited normally but emitted bare tool JSON as prose: zero registered tool calls/process executions. This negative model result is retained. A separate compile-time deterministic Native-parser action harness traversed the actual CLI parser/registry/AgentStore and executed the authorized compiler command exactly once under ASan/UBSan; it validates routing without claiming autonomous model success.',
           '实际0.6B CLI探测虽正常退出，却把工具裸JSON当正文输出，注册工具与真实进程执行均为0；该负结果保留。另一编译期确定性Native动作测试经过实际CLI参数／注册表／AgentStore，在ASan/UBSan下使授权编译命令恰好执行一次，用于验证接线，不当作模型自主成功。'), '',
        tr('Fast reads, already resident KV, saving costs, contention and negative paired differences are retained. A timed policy can reduce early holding without improving latency. No claim of smaller physical KV buffers, cold SSD, PCIe speedup, Windows/CUDA, model cold start or general cloud integration follows. Tool results/suffix work remain after completion. Current measured source hashes are E21; historical performance remains tied to its original revision.',
           '快速读取、已驻留KV、保存成本、争用及逐对回退均保留。定时可能减少提前占位而不改善延迟；不据此宣称物理KV缩容、冷SSD／PCIe、Windows/CUDA、模型冷启动或通用云端接入。真实工具结果与后缀仍需在返回后处理。当前实测源码哈希看E21，旧性能仍对应各自历史版本。'), '',
        tr('Qualification covers borrowed-engine lifetime, timer/in-flight/ready cancellation, exact native tokens/greedy, environment-change rebuild, bounded keys, tool failure/capacity behavior, Native/14K/KV/Stream regression, ASan/UBSan and default tests. The native CLI exposes --tool-prepare late|immediate|timed and --tool-kv resident|recompute|restore with an explicit deployed test command. Unknown estimates start immediately; early tool completion wakes the timer. This opt-in does not replace the default HTTP path.',
           '验收覆盖借用engine生命周期、等待／运行中／就绪后取消、精确Native token／greedy、环境变化重建、key上限、工具失败／容量行为、Native／14K／KV／Stream回归、ASan/UBSan和默认测试。Native CLI通过--tool-prepare late|immediate|timed与--tool-kv resident|recompute|restore显式启用，并要求部署test command；未知估计立即启动，工具提前结束唤醒定时。此选项不替换默认HTTP路径。'), '',
        '## '+tr('E22 · restore timing attribution', 'E22 · 定时恢复等待归因'), '',
        tr('E22 adds observation only and keeps the old scheduling algorithm, margin5ms and alpha. Four read/compiler × short/long cases compare resident and restore late/immediate/timed, with the same calibration/past-label discipline. Parent fork start/end, preparer timer, worker admission/queue/verification/restore/finish/return, cleanup and final frontend/prefill now share the same Darwin raw monotonic clock. Tool trace does not call observers from the fork child or repeat commands.',
           'E22只增加观测，保留旧算法、5ms余量及alpha；read／compile×两档历史比较resident及restore late／立即／定时，标定与过去标签规则不变。父进程fork起止、准备线程timer、worker入队／校验／恢复／finish／返回、清理和最终前端／预填充统一到Darwin raw单调时钟。工具trace不在fork子进程回调，也不重跑命令。'), '',
        table(['E22 compiler words','Mode','n calls','Prefix wait mean ms','Post-prefix mean ms','Spawn mean ms','Before native mean ms','Native mean ms','Queue mean ms','Finish return mean ms'],[
            [r['words'],r['mode'],r['samples'],*[f"{r[k]['mean']:.3f}" for k in ['prefix_wait_ms','post_prefix_ms','spawn_ms','before_native_ms','native_ms','queue_ms','finish_return_ms']]]
            for r in e['E22']['stage_summary'] if r['scenario']=='compile_test']), '',
        tr('The immediate path spends about111/148ms before native restore at the two history sizes, while fork itself averages117/162ms; the intervals overlap. Queue and finish return are much smaller. This localizes the missing time to verification overlapping process creation, not purely disk transfer or finish notification. It does not identify an internal libc lock or prove an OS-wide mechanism. E21 did not record these boundaries, so its historical residual is not retrospectively relabelled.',
           '两档历史的立即恢复在native之前平均耗时约111／148ms，fork自身约117／162ms，区间重叠；queue和finish返回很小。缺失成本被定位到与进程创建重叠的校验阶段，不是纯读盘传输或完成通知。该观测没有直接识别libc内部锁或OS机制；E21未记录这些边界，不能事后把旧残差逐条改名归因。'), '',
        tr('For timed restore, mean prefix waiting is4.978/5.202ms per call. Old deadline anchoring averages only.006/.007ms off tool start, so a simple origin fix is not the principal mean cause in E22. The signed identity is ready−tool_end = (ETA_hat−actual_tool) + (actual_prepare−prepare_hat) − margin + dispatch_offset + clamp. Positive wait clips this value at zero; early and late errors can cancel in the mean. Moving the deadline earlier alone trades waiting for early holding.',
           '定时恢复的正prefix等待均值为每调用4.978／5.202ms；旧deadline锚点相对工具开始平均仅偏.006／.007ms，简单修正时间原点不是E22中的主要均值原因。逐条恒等式为ready−tool_end=(ETA估计−真实工具时长)+(真实准备−准备估计)−余量+派发偏移+clamp。正等待对这个值取max(0)，早／晚误差会在均值中互相抵消；仅整体提前会用更多提前占位交换等待。'), '',
        tr('All adjacent preparation stages sum exactly to total preparation. The old E21 audit is retrospective and references immutable original records; no extra inference samples. [restore-estimate-audit.csv](data/restore-estimate-audit.csv), [restore-stages.csv](data/restore-stages.csv) and [restore-stage-summary.csv](data/restore-stage-summary.csv) preserve signed errors, full stage partitions and mean/median/ranges. E22 has547 native records, not a revised E21 run.',
           '所有相邻准备阶段之和与总prepare逐条相等。旧E21审计为事后数值复算，引用不可变原记录，不增加推理样本。[restore-estimate-audit.csv](data/restore-estimate-audit.csv)、[restore-stages.csv](data/restore-stages.csv)及[restore-stage-summary.csv](data/restore-stage-summary.csv)保留signed误差、阶段分解和均值／中位／范围。E22是新547条native记录，不是重写E21。'), '',
        '## '+tr('E23 · spawn and parent-progress ETA', 'E23 · 进程启动与进度条件ETA'), '',
        tr('Two changes are tested independently on new320/1792-word histories and25,000,000-iteration real compiler/test fixtures. macOS26+ uses posix_spawn with the same deployed shell command, cwd, environment, stdio redirection and process-group timeout. Explicit fork remains a benchmark/deployment control; older macOS/Linux retain fork pending acceptance. The implementation follows the [Apple spawn manual](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/posix_spawn.2.html) and [public header](https://github.com/apple-oss-distributions/xnu/blob/main/libsyscall/wrappers/spawn/spawn.h). A failed launch does not retry via another backend.',
           '两项改动在新320／1792-word历史与25000000次循环的真实编译／测试上独立对照。macOS26+使用posix_spawn，保持部署shell命令、cwd、环境、标准IO重定向和进程组超时规则；显式fork保留作部署／测试对照，更旧macOS／Linux暂保留fork待验收。实现参考[Apple spawn手册](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/posix_spawn.2.html)及[公开头](https://github.com/apple-oss-distributions/xnu/blob/main/libsyscall/wrappers/spawn/spawn.h)。启动失败不切backend重跑。'), '',
        tr('The new progress policy observes only the parent spawn-return event, bound to the current execute epoch. It estimates end=observed_spawn_return + prior EWMA(after-spawn duration), and dispatches at max(spawn_return, estimated_end−prior_prepare−5ms). This conditions on an event already observed, not a future label. Alpha/margin remain fixed; same three complete calibration labels seed every policy of one backend. Unknown/no progress falls back to preparation after parent progress/tool return. Explicit --tool-prepare progress is available in the same Native CLI tool wrapper.',
           '新progress策略只使用父进程spawn返回事件，绑定当前execute epoch；预计结束=已观测spawn_return＋过去EWMA后段耗时，派发=max(spawn_return,预计结束−过去准备估计−5ms)。特征是已经发生的事件，不是未来标签；alpha／余量不变，同backend各策略共享相同三次完整标定标签。未知／无进度回退到收到父进度／工具返回后准备。原Native工具包装可显式--tool-prepare progress。'), '',
        table(['E23 case / words','Mode','n tasks','After-tool wait sum ms','Early-hold sum ms','Complete task ms'],[
            [f"{r['scenario']} / {r['words']}",r['mode'],r['samples'],*[f"{r[k]['median']:.3f}" for k in ['post_tool_sum_ms','early_hold_sum_ms','task_ms']]]
            for r in e['E23']['summary'] if r['kind']=='task_timing']), '',
        '![Restore diagnosis and tradeoffs](figures/restore-balance.svg)', '',
        table(['E23 compiler words','Baseline','Candidate','Wait saving median ms','Hold saving median ms','Both lower pairs /7','Task saving median ms'],[
            [r['words'],r['baseline'],r['mode'],f"{r['post_tool_sum_ms']['median_saving_ms']:.3f}",f"{r['early_hold_sum_ms']['median_saving_ms']:.3f}",r['both_wait_and_hold_lower_pairs'],f"{r['task_ms']['median_saving_ms']:.3f}"]
            for r in e['E23']['paired_summary'] if r['scenario']=='compile_test' and (r['baseline'],r['mode']) in [('restore_fork_timed','restore_fork_progress'),('restore_spawn_timed','restore_spawn_progress'),('restore_fork_timed','restore_spawn_progress'),('restore_spawn_immediate','restore_spawn_progress')]]), '',
        tr('The robust improvement is process launch/verification: compiler fork controls average175–388ms per launch across the measured policy/history cells, while spawn restore cells average0.6–0.9ms. In the long immediate comparison, before-native time drops260.299→0.227ms; old fork overlap is removed without changing the KV format or llama kernels. This does not turn every task-time fluctuation into a causal scheduler gain.',
           '明确改善在进程启动／校验干扰：编译工具各策略／历史单元的fork平均启动175–388ms，spawn恢复单元约0.6–0.9ms。长历史立即恢复对照的before-native由260.299降到0.227ms；在不改KV格式或llama内核的情况下消除了这类fork重叠停顿。不能因此把完整任务的所有波动都算作调度收益。'), '',
        tr('Progress ETA is not a stable joint win. Against spawn/timed, short/long history paired median wait savings are3.334/8.243ms over three calls; paired hold savings are−2.180/+0.089ms, and only2/7 and4/7 pairs lower both. Long-history paired task saving is−15.058ms: a regression. Unpaired wait medians there are155.645→146.870ms while hold medians grow9.481→23.805ms. These different medians are all retained; no aggregate is substituted to claim a Pareto improvement. Keep progress opt-in and the original timed default.',
           'progress ETA尚未稳定双赢：相对spawn/timed，短／长历史三轮等待的逐对节省中位3.334／8.243ms，提前空等逐对节省−2.180／+0.089ms，仅2/7与4/7对同时下降。长历史完整任务逐对节省−15.058ms，反而变慢；该组独立等待中位155.645→146.870ms，但空等中位9.481→23.805ms。不同统计口径全部保留，不替换成有利的聚合去宣称Pareto改进。progress保持显式可选，原timed默认保留。'), '',
        tr('Why the residual tradeoff remains: after-spawn tool duration still drifts. Mean final suffix prefill is roughly28–38ms per call in spawn compiler groups, and prefix waiting is clipped/variable; a better phase ETA does not eliminate these costs. Changing only a fixed safety margin would move the waiting/holding tradeoff. No E23 labels were used to retune the frozen5ms/alpha or choose a replacement default.',
           '剩余权衡来自spawn之后工具时长仍会漂移。spawn编译组的最终后缀prefill均值约28–38ms/调用，prefix等待又是截断且有波动的量，阶段ETA变好无法消除这些成本。只改固定余量会移动等待／占位权衡；本轮没有用E23标签重调冻结5ms／alpha或挑一个新默认策略。'), '',
        tr('Every negative pair is retained in [restore-control-pairs.csv](data/restore-control-pairs.csv). [restore-task-summary.csv](data/restore-task-summary.csv) lists complete task ranges; component and final frontend/prefill traces remain separate. Compare within this run only: different histories/workloads and machine drift prevent subtracting E21/E22 from E23 as an algorithm speedup. Neither ready-prefix idle time nor fewer file stalls implies physical KV allocation release or a Gaussian timing guarantee.',
           '全部负的逐对差异保留在[restore-control-pairs.csv](data/restore-control-pairs.csv)，[restore-task-summary.csv](data/restore-task-summary.csv)列完整任务范围，阶段及最终前端／预填充单列。只比较同轮对照：不同历史／工作量和机上漂移使E21／E22→E23跨轮差值不能称算法提速。就绪前缀空等变少或文件校验停顿减少，都不等于物理KV缩容或Gaussian准时保证。'), '',
        tr('Current progress/spawn code passed epoch/duplicate/time-bound/cancel tests, Native/14K/KV/Stream, default tools including process-group timeouts, ASan/UBSan, exact canonical/fresh-greedy oracles and deterministic actual CLI routing. E23 records979 native rows (192 calibration,784 evaluation,3 status); three measured tool calls per task plus a real correction write in compiler tasks. Actions remain scripted for timing, not a new0.6B autonomous capability claim. Historical evidence stays immutable; measured source hashes for this historical run are E23; E24 records the later implementation.',
           '当前progress／spawn代码通过epoch／重复／越界／取消、Native／14K／KV／Stream、含进程组超时的默认工具、ASan/UBSan、精确canonical／fresh-greedy和实际CLI确定性路由验收。E23为979条native（192标定、784评价、3状态），每任务三次计时工具调用，编译任务另有计入总时的真实修正写入。计时动作仍为脚本选择，不是新的0.6B自主能力结论；旧证据不变，本阶段实测源码哈希看E23，后续实现看E24。'), '',
        '## '+tr('E24 · complete task accounting and early cleanup', 'E24 · 完整任务归因与提前清理'), '',
        tr('The E23 long-history paired median regression of15.058ms belongs to rep1. Its additive decomposition is tool+8.354, pre-tool+8.207, post-tool−3.517, actual repair+.874 and bookkeeping+1.141ms. Save alone contributes+8.267ms within pre-tool. Components of this same pair add up; medians of separate components do not. All14 compiler pairs, source task/call indices and the immutable E23 hash are in [tool-taskcost-e23-audit.csv](data/tool-taskcost-e23-audit.csv). Both E23 controls already called the same progress API: a15ms direct callback penalty was not established.',
           'E23长历史逐对中位退化15.058ms对应rep1。可加拆分：工具+8.354、工具前+8.207、工具后−3.517、真实修正+.874、记账+1.141ms；其中保存+8.267ms属于工具前。必须在同一对上加总，不能加各项独立中位数。全部14对编译任务、原task/call索引与不变E23哈希见[tool-taskcost-e23-audit.csv](data/tool-taskcost-e23-audit.csv)。E23两组本来都调用同一progress API，尚无证据支持“回调直接花15ms”。'), '',
        tr('Frozen new validation:512/2304 history words, affine arithmetic repair at two inputs,18M rotate/xor iterations, actual compiler/tests and fast-read controls. Five modes rotate by replicate, seven three-call tasks each, identical three-task calibration per state, independent past-only EWMA. Alpha and5ms margin stay fixed. Callback entry/exit and thread creation/function entry are observed separately. Saving remains charged before every restore task call; its variability is not erased by subtracting it from the headline.',
           '冻结新验证：512/2304历史词、两个输入的乘法修复、1800万rotate/xor循环、实际编译测试及快速读控制。五策略按rep轮转，每组七个三次调用任务，同状态共用三任务标定、独立仅过去标签的EWMA；alpha／5ms不调。单列回调起止和线程创建／tool函数入口。每次恢复前的保存仍计完整成本，其波动不能从成绩中扣掉。'), '',
        table(['E24 case / words','Mode','n tasks','After-tool sum ms','Ready-prefix hold ms','Complete task ms'],[
            [f"{r['scenario']} / {r['words']}",r['mode'],r['samples'],f"{r['post_tool_sum_ms']['median']:.3f}",f"{r['early_hold_sum_ms']['median']:.3f}",f"{r['task_ms']['median']:.3f}"]
            for r in e['E24']['summary'] if r['kind']=='task_timing']), '',
        '![Task and callback cost controls](figures/tool-taskcost.svg)', '',
        table(['Compiler words','Baseline','Candidate','Task saving median [min,max] ms','Faster /7','Wait saving median ms','Both wait/hold lower /7'],[
            [r['words'],r['baseline'],r['mode'],f"{r['task_ms']['median_saving_ms']:.3f} [{r['task_ms']['min_saving_ms']:.3f}, {r['task_ms']['max_saving_ms']:.3f}]",r['task_ms']['positive_pairs'],f"{r['post_tool_sum_ms']['median_saving_ms']:.3f}",r['both_wait_and_hold_lower_pairs']]
            for r in e['E24']['paired_summary'] if r['scenario']=='compile_test']), '',
        table(['Compiler words','Mode','Callback mean / max ms','Thread creation mean ms','Drop exposed after tool mean ms'],[
            [r['words'],r['mode'],f"{r['callback_ms']['mean']:.6f} / {r['callback_ms']['max']:.6f}",f"{r['thread_create_ms']['mean']:.6f}",f"{r['exposed_drop_ms']['mean']:.3f}"]
            for r in e['E24']['stage_summary'] if r['scenario']=='compile_test']), '',
        tr('Direct restore-mode callbacks average2.0–2.8 microseconds, max6.458 microseconds; thread creation averages21–29 microseconds. These observed direct costs cannot explain15ms. Timed event/no-event task differences still span both signs despite identical scheduling, so serial wall-clock differences are not callback CPU cost. Early deletion reduces mean exposed cleanup2.308→.648ms (short) and4.004→3.346ms (long), but complete progress tasks regress by paired median313.434ms (short, faster1/7) and4.653ms (long, faster3/7). Long wait improves20.711ms with5/7 positive pairs; short wait worsens14.894ms. The short task median-regression pair contains350.203ms slower tools, partly offset by42.725ms less pre-tool work. Resource interference versus OS/cache/frequency drift is not fully identified; a2ms cleanup interval cannot establish the cause of a350ms tool change. The optimization is not promoted to default.',
           '恢复组直接回调均值2.0–2.8微秒、最大6.458微秒；线程创建均值21–29微秒，解释不了15ms。相同调度的timed有／无事件任务差值仍有正有负，串行墙钟差不是回调CPU成本。提前删除使暴露清理均值短2.308→.648ms、长4.004→3.346ms，但相对原progress完整任务逐对中位分别回退313.434ms（仅1/7更快）和4.653ms（3/7更快）；长等待改善20.711ms、5/7为正，短等待恶化14.894ms。短任务中位回退那一对包含工具慢350.203ms、工具前少42.725ms的部分抵消；间接争用与OS／缓存／频率漂移尚未完全分开，不能从2ms清理区间断言350ms工具变化的原因。优化不升为默认。'), '',
        tr('The optional early-release path deletes the private snapshot only after ticket finish has drained; prefix-ready remains the device-ready boundary. Cleanup joins before return, preserves cancellation rollback and tool-once semantics, and falls back to return cleanup on failure. It adds no extra thread. Select with --tool-snapshot-release early; default remains return, and default ETA remains timed. Early deletion can overlap the remaining tool, but cannot remove pre-tool saving or all tool-duration variation. The direct callback duration excludes any indirect resource contention; these seven pairs are not a causal decomposition of OS load or a tail guarantee.',
           '可选提前释放只在ticket finish排空后删除私有快照，prefix_ready仍表示设备就绪。返回前join清理，保留取消回滚、工具仅执行一次及失败时返回清理兜底，不新增线程。用--tool-snapshot-release early选择，默认仍return／timed。删除可与剩余工具重叠，但消除不了工具前保存或工具时长波动；直接回调耗时也不包含间接争用。七对不构成OS负载的完整因果拆分或尾部保证。'), '',
        tr('All new task additive identities, calibration and evaluation rows are in [tool-taskcost-accounting.csv](data/tool-taskcost-accounting.csv); every indexed task pair, including negatives, is in [tool-taskcost-pairs.csv](data/tool-taskcost-pairs.csv), with aggregate ranges in [restore-control-pairs.csv](data/restore-control-pairs.csv). E24 adds659 native records:72 calibration calls+24 tasks,420 evaluation calls+140 tasks,3 status rows. Twelve independent fresh-KV references check492 call rows. Sanitizers, cancellation before/during/after restore, tool exception, Native/14K/KV/Stream, default regression and actual CLI tool-once passed before timing. Exact current measured sources are archived privately; public evidence carries hashes. No cross-run speedup, autonomous-model or physical-memory-release claim.',
           '全部新任务恒等式、标定及评价见[tool-taskcost-accounting.csv](data/tool-taskcost-accounting.csv)，包括负结果的每对任务及源索引见[tool-taskcost-pairs.csv](data/tool-taskcost-pairs.csv)，汇总范围在[restore-control-pairs.csv](data/restore-control-pairs.csv)。E24新增659条native：72标定调用＋24任务、420评价调用＋140任务、3状态；12个独立fresh-KV参考核对492调用。计时前通过sanitizer、恢复前／中／后取消、工具异常、Native/14K/KV/Stream、默认回归及实际CLI工具一次执行。当前实测源码私有归档，公开仅哈希；不跨轮相减、不宣称模型能力或物理显存释放。'), '',
        '## '+tr('E25 · snapshot deletion and resource isolation', 'E25 · 快照删除与资源隔离'), '',
        tr('Snapshots are optional capacity-policy state. The normal resident path does no save/evict/restore; recompute can retain only canonical tokens. Current restore snapshots require the same live engine/session/revision and do not implement cold restart or shrinking of the preallocated KV buffer. Saving before every tool in the experimental restore path is charged; it is not universal Agent overhead. Do not evict usable resident KV merely to create a prewarming opportunity.',
           '快照属于可选容量策略。正常resident不做save/evict/restore；也可只保留canonical token后重算。当前恢复文件绑定同一live engine/session/revision，不支持冷重启，不缩小预分配KV buffer。实验restore每次工具前保存的成本全部收费，不是普通Agent的必需开销；不应主动逐出可用KV来制造预热机会。'), '',
        tr('E25 repeats the short512-word/18M repair workload with a trusted C driver splitting actual compiler and check execution. Five controls use ten complete tasks each in forward/reverse cyclic Latin order: each mode occupies every order position twice. Same calibration labels, independent past-only EWMA, fixed5ms/alpha. The unlink-hold control removes the directory entry but retains an owner-only fd and snapshot slot/budget until tool-end last close. [Apple unlink](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/unlink.2.html) defines this separation; it does not predict the size of the resulting performance effect.',
           'E25用可信C驱动器将短512词／1800万修复任务拆成真实编译与实际check运行。五控制各10完整任务，正反Latin轮换，每策略每个位置恰好2次；同标定、独立仅过去标签的EWMA，5ms／alpha不改。unlink-hold移除目录项但保留owner私有fd及原槽位／预算，工具结束才最后close。[Apple unlink](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/unlink.2.html)定义该隔离语义，不预先保证收益大小。'), '',
        table(['Mode','Task median ms','After-tool sum median ms','Compiler wall / CPU mean ms per call','Run wall / CPU mean ms per call'],[
            [r['mode'],f"{r['task_ms']['median']:.3f}",f"{r['post_tool_sum_ms']['median']:.3f}",
             f"{next(g for g in e['E25']['stage_summary'] if g['mode']==r['mode'])['compile_wall_ms']['mean']:.3f} / {next(g for g in e['E25']['stage_summary'] if g['mode']==r['mode'])['compile_cpu_ms']['mean']:.3f}",
             f"{next(g for g in e['E25']['stage_summary'] if g['mode']==r['mode'])['run_wall_ms']['mean']:.3f} / {next(g for g in e['E25']['stage_summary'] if g['mode']==r['mode'])['run_cpu_ms']['mean']:.3f}"]
            for r in e['E25']['summary'] if r['kind']=='task_timing']), '',
        '![Snapshot deletion isolation](figures/tool-isolate.svg)', '',
        table(['Baseline','Candidate','Task saving median [min,max] ms','Faster /10','Compiler wall saving median ms','Run wall saving median ms'],[
            [r['baseline'],r['mode'],f"{r['task_ms']['median_saving']:.3f} [{r['task_ms']['min_saving']:.3f}, {r['task_ms']['max_saving']:.3f}]",r['task_ms']['positive_pairs'],f"{r['compile_wall_ms_sum']['median_saving']:.3f}",f"{r['run_wall_ms_sum']['median_saving']:.3f}"] for r in e['E25']['paired_summary']]), '',
        tr('E25 does not reproduce a stable313ms early-delete penalty: early versus return has paired median task saving14.258ms, only5/10 faster, range−130.629 to176.366ms. Moving last close out of the tool (early→hold) gives only1.146ms median saving,5/10 faster. Full early deletion averages4.064ms wall and2.166ms parent-interval CPU; unlink while holding fd averages1.253ms wall/.117ms parent CPU, with1.795ms last-close wall after the tool. These are small direct differences, not a demonstrated hundred-ms mechanism. All modes, including resident, show check-launch/reaping wall284–313ms but child CPU10.6–11.1ms. Compression/swapin also occurs in resident; global pressure is not unique evidence against unlink. This motivates the separate main-entry trace in E26.',
           'E25未复现稳定313ms提前删除惩罚：early相对return任务逐对中位节省14.258ms、仅5/10更快，范围−130.629至176.366ms；将最后close移出工具（early→hold）仅中位节省1.146ms、5/10更快。完整提前删除均值4.064ms墙钟、父进程区间CPU2.166ms；保留fd的unlink为1.253ms／.117ms，工具后最后close墙钟1.795ms。直接差异较小，未证实数百ms机制。所有模式含resident均有check启动／回收墙钟284–313ms、child CPU10.6–11.1ms；驻留也出现全机压缩／swapin，不能专归因unlink，因此另做E26 main入口时间线。'), '',
        tr('Detailed resource samples, all negative task pairs and source indices are in [tool-isolate-resources.csv](data/tool-isolate-resources.csv), [tool-isolate-tasks.csv](data/tool-isolate-tasks.csv), [tool-isolate-pairs.csv](data/tool-isolate-pairs.csv) and [tool-isolate-pair-summary.csv](data/tool-isolate-pair-summary.csv). [wait4](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/wait4.2.html) includes waited child descendants. CPU may exceed wall for parallel compiler work; wall minus CPU is not IO time. [getrusage](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/getrusage.2.html) IO operation counts exclude cache-served activity; zeros cannot exclude filesystem CPU or cache contention. Mach global VM counters include other processes, and footprint gauges can decrease. Resource sampling and the driver are common diagnostic overhead, retained in task time, so no E24→E25 cross-run speedup is claimed.',
           '资源样本、全部负任务对及源索引见[tool-isolate-resources.csv](data/tool-isolate-resources.csv)、[tool-isolate-tasks.csv](data/tool-isolate-tasks.csv)、[tool-isolate-pairs.csv](data/tool-isolate-pairs.csv)及[tool-isolate-pair-summary.csv](data/tool-isolate-pair-summary.csv)。[wait4](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/wait4.2.html)包含已等待后代的资源；并发编译CPU可超过墙钟，墙钟减CPU不能当IO时间。[getrusage](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/getrusage.2.html)的IO操作数不包含缓存命中，0不能排除文件系统CPU／缓存争用。Mach全机VM计数含其他进程，footprint可下降。资源采样及驱动器属于各组共用诊断成本，计入任务，不宣称E24→E25跨轮提速。'), '',
        tr('E25 records227 native rows:18 calibration calls+6 tasks,150 evaluation calls+50 tasks,3 status. Three fresh-KV reference prompts validate168 calls. ASan/UBSan, retained-slot accounting, unlink-in-use/duplicate/stale restore refusal, waiting/inflight/ready cancellation, Native14K/KV/Stream, default regression and real CLI once pass before timing. Default resident capacity policy, timed ETA and return cleanup remain unchanged; hold-fd remains an explicit diagnostic. Exact measured source snapshot is private, hashes public. Old313.434ms result is retained, not replaced by this run.',
           'E25为227条native：18标定调用＋6任务、150评价调用＋50任务、3状态；3个fresh-KV参考核对168调用。计时前通过ASan/UBSan、保留槽位记账、占用／重复unlink／失效恢复拒绝、等待／运行／ready取消、Native14K/KV/Stream、默认回归及实际CLI一次验收。默认resident容量策略、timed ETA及返回清理不变，hold-fd保持显式诊断；精确实测源码私有归档、公开哈希。原313.434ms负结果保留，不被本轮替换。'), '',
        '## '+tr('E26 · executable first launch and incremental build', 'E26 · 程序首次启动与增量构建'), '',
        tr('E25 locates a large wall interval around check launch/reaping even in the resident control. E26 separately freezes new768-word/12M tasks, changes the repair to calc(x)=3*x−2 at two inputs, and records spawn return plus child main/compute-end. Each real test tool now runs the same binary twice with matching exit status; the repeated run is charged and its stdout suppressed. Initial test binary is removed per task. The incremental deployment recipe rebuilds after the actual repair and reuses only the unchanged third-call artifact; it never skips running tests or caches their result.',
           'E25在驻留控制也定位到check启动／回收的大段墙钟区间。E26另冻新768词／1200万任务，修复改为两个输入的calc(x)=3*x−2，记录spawn返回及child main／compute-end。每次真实工具都将同binary运行两次并验证退出一致，第二次stdout丢弃、成本照收；每任务开头删除旧binary。显式增量部署命令在实际修正后重建，只在第三次源未变化时复用artifact，从不跳过测试或缓存测试结果。'), '',
        table(['Rebuilt this call','Execution','n calls','Wall median ms','Spawn call median ms','Pre-main median ms','Compute median ms'],[
            [r['compiled_this_call'],r['execution'],r['samples'],f"{r[r['execution']+'_wall_ms']['median']:.3f}",f"{r[r['execution']+'_spawn_ms']['median']:.3f}",f"{r[r['execution']+'_pre_main_ms']['median']:.3f}",f"{r[r['execution']+'_compute_ms']['median']:.3f}"] for r in e['E26']['launch_summary']]), '',
        '![Executable launch and incremental build](figures/tool-launch.svg)', '',
        table(['Mode','Complete task median [min,max] ms','After-tool wait sum median ms'],[
            [r['mode'],f"{r['task_ms']['median']:.3f} [{r['task_ms']['min']:.3f}, {r['task_ms']['max']:.3f}]",f"{r['post_tool_sum_ms']['median']:.3f}"] for r in e['E26']['summary'] if r['kind']=='task_timing']), '',
        table(['Baseline','Candidate','Task saving median [min,max] ms','Faster /7','Compiler wall saving median ms','Run wall saving median ms'],[
            [r['baseline'],r['mode'],f"{r['task_ms']['median_saving']:.3f} [{r['task_ms']['min_saving']:.3f}, {r['task_ms']['max_saving']:.3f}]",r['task_ms']['positive_pairs'],f"{r['compile_wall_ms_sum']['median_saving']:.3f}",f"{r['run_wall_ms_sum']['median_saving']:.3f}"] for r in e['E26']['paired_summary']]), '',
        tr('New-artifact first-run pre-main median is278.015ms (spawn call.179ms); immediate same-artifact rerun pre-main is1.788ms. First-run wall287.626ms versus rerun10.066ms, with compute7.978/6.561ms. Reused third-call artifacts have pre-main1.149ms. A large common delay is therefore after process creation and before main on first execution, not inside the arithmetic or specific to KV cleanup. The explicit incremental recipe reduces paired complete task median327.722ms, range283.323–395.514ms,7/7 faster; all tests still run. This is a tool-workflow optimization, not llama speedup. Early cleanup still has negative pairs: E26 early versus return ranges−135.317 to366.685ms; hold versus early−210.271 to168.660ms. Keep runtime defaults unchanged.',
           '新artifact首次运行pre-main中位278.015ms（spawn调用.179ms），同artifact立即再运行pre-main为1.788ms；总运行墙钟287.626→10.066ms，计算7.978／6.561ms。复用的第三轮artifact pre-main为1.149ms。大段共同延迟因此定位在首次执行的进程创建后、main前，不在算术中，也不专属于KV清理。显式增量命令使完整任务逐对中位减少327.722ms，范围283.323–395.514ms、7/7更快，测试照常执行；这是工具流程优化，不是llama提速。清理仍有负对：E26 early相对return范围−135.317至366.685ms，hold相对early−210.271至168.660ms，默认策略保持不变。'), '',
        tr('Pre-main contains process creation and loader/platform work; spawn-call time is a contained observation and must not be added to it. Parent scheduling can resume after child main, so no universal spawn-return-before-main ordering is assumed. The new same-artifact paired trace can locate first-execution delay but does not prove Gatekeeper/AMFI/SSD or any specific kernel cause. E24 had no such child-entry timestamps, so its313ms pair remains unassigned at this finer boundary. Global resource noise observed in E25 is retained, not retrospectively filtered out.',
           'pre-main包含进程创建及loader／平台工作；spawn调用是其中的观测，不可再相加。父进程可能在child已进main后恢复调度，不假设所有样本spawn返回都早于main。同artifact配对可定位首次执行延迟，不能据此指定Gatekeeper／AMFI／SSD或某个内核原因。旧E24没有child入口时间戳，其313ms那对在这个更细边界仍无法追溯确定，全局资源噪声也不事后过滤。'), '',
        tr('All stages, calibrated and complete tasks, negative task pairs and launch strata remain in [tool-launch-stages.csv](data/tool-launch-stages.csv), [tool-launch-tasks.csv](data/tool-launch-tasks.csv), [tool-launch-pairs.csv](data/tool-launch-pairs.csv), [tool-launch-pair-summary.csv](data/tool-launch-pair-summary.csv) and [tool-launch-executions.csv](data/tool-launch-executions.csv). E26 adds167 native rows:18 calibration calls+6 tasks,105 evaluation calls+35 tasks,3 status. Three fresh-KV references check123 call rows. All246 actual check executions (including calibration) remain charged within their123 tools. Repeated checks add work, so compare controls within E26 only. Runtime capacity/ETA/cleanup defaults remain resident/timed/return; the incremental recipe is a benchmark/deployment choice, not automatic cxf command rewriting.',
           '完整阶段、标定／任务、负逐对及首次执行分层见[tool-launch-stages.csv](data/tool-launch-stages.csv)、[tool-launch-tasks.csv](data/tool-launch-tasks.csv)、[tool-launch-pairs.csv](data/tool-launch-pairs.csv)、[tool-launch-pair-summary.csv](data/tool-launch-pair-summary.csv)及[tool-launch-executions.csv](data/tool-launch-executions.csv)。E26新增167条native：18标定调用＋6任务、105评价调用＋35任务、3状态；3个fresh-KV参考核对123调用。含标定共246次实际check执行，全部计入123个工具成本。重复测试增加工作，只比较E26同轮对照。运行时默认仍resident／timed／return，增量命令是基准／部署选择，不是cxf自动改写命令。'), '',
        '## '+tr('E27 · normal resident multi-tool flow', 'E27 · 正常常驻多轮工具流程'), '',
        tr('Both controls retain KV throughout the active task. Six real calls read configuration, observe a failed validator, write the correction, pass validation, read back, and validate again. Native prepares the actual receipt and samples first greedy after every call. Compare direct original-tool execution with the existing resident coordinator; no artificial save, eviction, restore or recomputation baseline. Both use the same prebuilt validator and fixed command, with no compilation or incremental-build optimization during timing. Model/initial history and recorded fixture first-execution warmup are excluded. Scripted choices isolate execution from0.6B tool-decision quality.',
           '两个控制都在任务内持续保留KV。六次真实调用依次读配置、观察验证失败、写修正、验证通过、回读、再次验证；每轮真实收据经Native准备后取首greedy。原工具直接执行与现有resident协调包装比较，不造save／逐出／恢复或重算基线。两组使用同一预构建验证程序及固定命令，计时内无编译或增量构建优化。模型／初始历史及已记录的程序首次执行暖机单列排除，动作固定以隔离执行成本，不是0.6B自主能力评分。'), '',
        table(['History words','Mode','n tasks','Complete six-call task ms','After-tool wait sum ms','New evaluated tokens'],[
            [r['words'],r['mode'],r['samples'],f"{r['task_ms']['median']:.3f}",f"{r['post_tool_sum_ms']['median']:.3f}",r['evaluated_tokens']['median']] for r in e['E27']['summary'] if r['kind']=='task_timing']), '',
        '![Resident multi-tool flow](figures/resident-flow.svg)', '',
        table(['History words','Wrapper task saving median [min,max] ms','Faster /7','After-tool saving median ms','Evaluated-token difference'],[
            [r['words'],f"{r['task_ms']['median_saving']:.3f} [{r['task_ms']['min_saving']:.3f}, {r['task_ms']['max_saving']:.3f}]",r['task_ms']['positive_pairs'],f"{r['post_tool_sum_ms']['median_saving']:.3f}",r['evaluated_tokens']['median_saving']] for r in e['E27']['paired_summary']]), '',
        tr('No extra preparation is available while tools run without new input: stable history KV is already ready. Both modes evaluate397 new tokens across six calls and preserve the full canonical/fresh-greedy result. Short/long wrapper task saving is−14.293/+5.014ms, faster2/7 and4/7, with negative pairs in both cases. This is no stable extra latency benefit. Prefix reuse counters quantify shared work avoided, not a measured speedup against deliberately discarded KV. Raw task wall, five excluded post-token oracle intervals, common collector bookkeeping and fixed tool durations remain separate.',
           '工具执行期间若没有新输入，就没有额外准备可隐藏：稳定历史KV已经就绪。两模式六轮均只新增评估397 token，完整canonical／fresh-greedy一致。短／长包装任务逐对中位节省−14.293／+5.014ms，仅2/7／4/7更快，两档都有负对，不能证明稳定额外收益。前缀复用计数表示免做的工作量，不是对主动丢KV基线的实测提速；原任务墙钟、前五段首token后oracle排除值、共同collector记账及固定工具耗时分列。'), '',
        tr('Indexed calls/tasks/pairs are in [resident-flow-calls.csv](data/resident-flow-calls.csv), [resident-flow-tasks.csv](data/resident-flow-tasks.csv) and [resident-flow-pairs.csv](data/resident-flow-pairs.csv). E27 adds199 records:168 tool calls,28 complete tasks,3 status. Twelve independent fresh references validate all calls. Save/evict/restore/speculative preparation and private KV files are zero. ASan/UBSan, actual repair, Native14K, existing Stream and default regressions pass; no production API/default or build command changed.',
           '逐条调用／任务／对照见[resident-flow-calls.csv](data/resident-flow-calls.csv)、[resident-flow-tasks.csv](data/resident-flow-tasks.csv)及[resident-flow-pairs.csv](data/resident-flow-pairs.csv)。E27新增199记录：168次工具调用、28完整任务、3状态；12个独立fresh参考核对全部调用。save／逐出／恢复／推测准备和私有KV文件均为0。ASan/UBSan、实际修正、Native14K、既有Stream及默认回归通过，未改生产API／默认值或构建命令。'), '',
        '## '+tr('E28 · resident streaming from a real process', 'E28 · 真实进程源的常驻流式预填充'), '',
        tr('A real prebuilt process reads twelve immutable report units from a file, performs0/40M compute iterations per unit, and immediately flushes each completed NDJSON unit. Payload80/240 words and wait-all/coalesced/eager modes have identical content and CPU work within each case. There are no sleep/network gaps or free idle interval after the last payload. The model retains its1024-word initial history and all accepted-prefix KV. Task starts at producer launch; producer CPU/wall, input timing, first greedy and normal Stream close are charged. Full-token oracle work follows first greedy and is subtracted only from complete-task wall; independent fresh prefill occurs after normal close and is recorded separately.',
           '真实预构建进程从文件读12个不可变报告单元，每块实际执行0／4000万循环，完成后立即flush NDJSON；两档80／240词和wait_all／合批／急切三模式，在同场景内容及CPU工作完全相同。无sleep／人为网络gap，最后payload后无免费尾部空等。模型保留1024词初始历史和已接收前缀KV。从producer启动计完整任务，计入源CPU／墙钟、输入、首greedy及Stream正常关闭；完整分词oracle在首greedy后执行，仅从完成墙钟扣除，独立fresh预填充在正常close后单列记录。'), '',
        table(['Words / unit','CPU iterations / unit','Mode','n','After-input first median ms','Total first median ms','Complete incl. close median ms','Prefill execution median ms','Decode calls median'],[
            [r['words'],r['producer_iterations'],r['mode'],r['samples'],f"{r['post_input_first_ms']['median']:.3f}",f"{r['task_first_ms']['median']:.3f}",f"{r['task_complete_ms']['median']:.3f}",f"{r['prefill_ms']['median']:.3f}",r['input_decode_calls']['median']] for r in e['E28']['summary']]), '',
        '![Resident real-process streaming](figures/resident-stream.svg)', '',
        table(['Words / unit','CPU iterations','Baseline','Candidate','Complete task saving median [min,max] ms','Faster /7','After-input saving median ms','Prefill execution saving median ms'],[
            [r['words'],r['producer_iterations'],r['baseline'],r['mode'],f"{r['task_complete_ms']['median_saving']:.3f} [{r['task_complete_ms']['min_saving']:.3f}, {r['task_complete_ms']['max_saving']:.3f}]",r['task_complete_ms']['positive_pairs'],f"{r['post_input_first_ms']['median_saving']:.3f}",f"{r['prefill_ms']['median_saving']:.3f}"] for r in e['E28']['paired_summary']]), '',
        tr('CPU-paced complete-task paired savings are136.054/159.388ms at80 words for coalesced/eager, and213.587/234.151ms at240 words; all four comparisons are7/7 faster. Burst input has no stable gain: coalesced/eager paired savings are−3.167/−.975ms at80 words and−8.827/−4.210ms at240. At80-word paced input, coalesced prefill execution increases by71.886ms and producer wall by17.769ms in paired medians, yet complete task improves136.054ms: latency is reduced by overlapping paid work, not by cheaper inference. Do not add these separate medians to reconstruct a total. Digest text differs between0/40M source scenarios (1126/1222 input tokens at80 words,3046/3142 at240); only within-scenario mode comparisons are controlled, not cross-scenario subtraction.',
           'CPU持续产出场景的完整任务逐对节省：80词合批／急切136.054／159.388ms，240词213.587／234.151ms，四个对照均7/7更快。突发输入没有稳定收益：80词合批／急切节省−3.167／−.975ms，240词−8.827／−4.210ms。80词持续产出时，合批的预填充执行逐对中位增加71.886ms、producer墙钟增加17.769ms，完整任务仍快136.054ms；收益来自已计费工作的重叠，不是推理计算变便宜，且不能相加这些独立中位数反推总时。0／4000万源场景的digest正文不同（80词新增1126／1222 token、240词3046／3142），只做同场景模式对照，不跨场景相减。'), '',
        tr('Actual producer and arrival times are retained; overlap can slow the producer or increase small-batch execution. Lower after-input wait alone is not a task speedup. See [resident-stream-tasks.csv](data/resident-stream-tasks.csv), [resident-stream-pairs.csv](data/resident-stream-pairs.csv) and indexed [resident-stream-arrivals.csv](data/resident-stream-arrivals.csv), with [resident-summary.csv](data/resident-summary.csv) / [resident-pair-summary.csv](data/resident-pair-summary.csv) for full ranges. E28 has87 native records:84 complete streaming tasks and3 status. Arrival arrays expand only in derived CSV, not into new independent sample counts. All modes keep the same canonical tokens/greedy and evaluate only new input; no snapshot, forced eviction or build change is involved.',
           '保留真实producer与到达时间；重叠可能拖慢producer或增加小批次执行，输入后等待下降不能单独当任务提速。详见[resident-stream-tasks.csv](data/resident-stream-tasks.csv)、[resident-stream-pairs.csv](data/resident-stream-pairs.csv)及带索引的[resident-stream-arrivals.csv](data/resident-stream-arrivals.csv)，全范围在[resident-summary.csv](data/resident-summary.csv)／[resident-pair-summary.csv](data/resident-pair-summary.csv)。E28为87条native：84个完整流式任务＋3状态；arrival数组只在派生CSV展开，不虚增独立样本。各模式canonical／greedy相同，只评估新输入，不涉及snapshot、主动逐出或构建变更。'), '',
        tr('This is actual local-process semantic-unit streaming, not vendor cloud SSE, arbitrary token-fragment submission or automatic agentctl routing. Stream tools remain empty and its worker exclusive. A first sanitizer collector failed when its extra simultaneous fresh-reference KV exceeded shared8192 capacity after nine completed tasks. Failure artifacts are retained outside performance aggregates. The final oracle runs after normal stream close without changing workload/capacity; final stream collector ASan and unchanged Native/Stream regressions pass. Lazy GPU setup, machine drift, negative task pairs and additional execution remain visible; no E26 build savings or cross-experiment gains are added.',
           '这里是真实本地进程的完整语义块流水线，不是厂商云SSE、任意token片段提交或agentctl自动接线；Stream tools仍空、worker独占。首次sanitizer collector在9个完成任务后，因额外同时保留的fresh参考KV超过共享8192容量而失败，原失败保留且不进性能汇总。最终oracle移到正常stream close后，不改工作量／容量；最终collector ASan及未变Native/Stream回归通过。GPU惰性初始化、机器漂移、负任务对及额外执行都保留，不累加E26构建收益或跨实验收益。'), '',
        '## '+tr('E29 · actual agentctl tool-output prefill, resident KV', 'E29 · agentctl真实工具输出预填充，KV常驻'), '',
        tr('The output stream now passes through the actual native CLI REPL, capability registry, AgentStore and test.run executor. Both explicit off/on modes use identical body-first receipts with final status footers and identical trailing-LF admission. Default CLI receipts remain unchanged. A fixed Native-parser action sequence selects three real test calls then DONE; four actual Native prepare/first-greedy operations run per task. This measures integration and scheduling, not0.6B autonomous planning or full completion latency.',
           '输出流已经过实际Native CLI REPL、能力注册、AgentStore和test.run执行器。显式off/on使用完全相同的正文优先／最终状态尾收据及入站尾LF；默认CLI收据不变。固定Native-parser动作选择三次真实test.run再DONE，每任务执行四次实际Native准备／首greedy。这是接线和调度验收，不是0.6B自主规划或完整生成速度。'), '',
        table(['Words / line','CPU iterations / line','Mode','Tasks','Whole task ms [min,max]','Post-tool sum ms','Total prefill ms','Adopted KV tokens'],[
            [r['words'],r['producer_iterations'],r['mode'],r['samples'],f"{r['task_ms']['median']:.3f} [{r['task_ms']['min']:.3f},{r['task_ms']['max']:.3f}]",f"{r['post_tool_sum_ms']['median']:.3f}",f"{r['prefill_sum_ms']['median']:.3f}",r['adopted_tokens']['median']] for r in e['E29']['summary']]), '',
        '![Actual agentctl output integration](figures/tool-output-cli.svg)', '',
        table(['Words / line','CPU iterations / line','Task saving ms [min,max]','Faster /7','Post-tool saving ms','Extra prefill ms','Extra tool ms','Extra suffix-tokenization IDs'],[
            [r['words'],r['producer_iterations'],f"{r['task_ms']['median_saving']:.3f} [{r['task_ms']['min_saving']:.3f},{r['task_ms']['max_saving']:.3f}]",r['task_ms']['positive_pairs'],f"{r['post_tool_sum_ms']['median_saving']:.3f}",f"{-r['prefill_sum_ms']['median_saving']:.3f}",f"{-r['tool_sum_ms']['median_saving']:.3f}",-r['stream_tokenized_tokens']['median_saving']] for r in e['E29']['paired_summary']]), '',
        tr('Savings are paired off minus on, not differences between group medians. Raw wall, exact excluded post-token oracle cost, every warmup/evaluation task, phase timings and negative pairs remain archived.515 native rows contain64 tasks (8 warmup/56 evaluation),192 actual tool executions (168 evaluation),256 full-token oracles and3 status rows. Independent13-case sanitizer checks include fresh-KV greedy, Unicode/BPE tails, truncation, timeout, cancellation and execution-once. Native14K, original Stream and default C/HTTP regressions pass. The first CLI qualification failure (tool-only counter assumption and first-user LF invalidation) is retained separately; LF normalization precedes all performance tasks.',
           '节省为逐对off减on，不是两组中位数相减。保留原始wall、逐次排除的首token后oracle成本、全部预热／评价任务、阶段时序和负对。515条native包含64任务（8预热／56评价）、192次真实工具执行（168评价）、256次全文token oracle和3状态。独立13项sanitizer检查包含fresh-KV greedy、Unicode/BPE尾、截断、超时、取消及仅执行一次；Native14K、原Stream和默认C/HTTP回归通过。首次CLI验收失败（工具独占计数假设与首轮用户LF失效）单独保留；LF归一化早于全部性能任务。'), '',
        tr('There are no snapshots or forced eviction. Provisional output shares the current canonical session; latest-only4KiB queue and4KiB worker copy are bounded suffix scratch, not another full history. Full final template rendering and prefix verification still run. BPE may replay/discard tail tokens; first-token readiness and actual task savings must be assessed together with added small-batch work, join wait and producer contention.8192-token physical KV allocation remains896MiB. See [tasks](data/tool-output-tasks.csv), [pairs](data/tool-output-pairs.csv), [stages](data/tool-output-stages.csv), [summary](data/tool-output-summary.csv) and [paired summary](data/tool-output-pair-summary.csv). The separate production0.6B probe returned Native format error (rc7), executed zero tools and remains a capability failure. No build optimization, cloud SSE, Windows/CUDA or cross-experiment gains are included.',
           '没有快照或主动逐出。临时输出共享当前canonical session；latest-only4KiB队列和4KiB工作副本是有界后缀scratch，不是另一份完整历史。最终完整模板渲染／前缀检查仍执行；BPE可能重算／丢弃尾token。要把首token准备与实际任务收益、额外小批计算、join等待、producer争用一起看；8192 token物理KV仍896MiB。详见[任务](data/tool-output-tasks.csv)、[逐对](data/tool-output-pairs.csv)、[阶段](data/tool-output-stages.csv)、[汇总](data/tool-output-summary.csv)与[逐对汇总](data/tool-output-pair-summary.csv)。单独真实0.6B生产探测返回Native格式错误rc7、零工具执行，能力失败保留。不包含构建优化、云SSE、Windows/CUDA或跨实验叠加收益。'), '',
        '## '+tr('E30 · adaptive output observation gate', 'E30 · 输出观察窗与结束后停止预填充'), '',
        tr('A frozen10ms initial gate skips speculative work when the tool has already finished; queued tail work also stops after completion. New40/48-word,0/30M-iteration tasks;7 pairs each. Same real CLI and canonical/greedy checks as E29. Wrapper minus join includes owner messaging and setup, not pure thread cost. E29 burst frontend was1.632/3.037ms, non-join wrapper0.828/0.935ms, versus extra prefill46.853/12.951ms. These are phase measurements, not a causal small-batch/tokenizer/thread ablation.',
           '固定10ms首次观察窗：工具已结束则跳过推测，后续也不消费已结束工具的积压尾。新40/48词、0/3000万循环任务，各7对；真实CLI及canonical/greedy校验与E29一致。wrapper减join包含owner通信与准备，不是纯线程成本。E29突发前端1.632/3.037ms、非join包装0.828/0.935ms，额外prefill46.853/12.951ms；这是阶段测量，尚不是小批次／分词／线程的因果消融。'), '',
        table(['Words','Iterations','Task saving ms [min,max]','Faster /7','Extra prefill ms','Extra frontend ms'],[[r['words'],r['producer_iterations'],f"{r['task_ms']['median_saving']:.3f} [{r['task_ms']['min_saving']:.3f},{r['task_ms']['max_saving']:.3f}]",r['task_ms']['positive_pairs'],f"{-r['prefill_sum_ms']['median_saving']:.3f}",f"{-r['stream_front_ms']['median_saving']:.3f}"] for r in e['E30']['paired_summary']]), '',
        '![Adaptive output](figures/tool-output-adaptive.svg)', '',
        tr('All515 rows and negative pairs retained. The new workload differs from E29; no same-workload old-on comparison or isolated tokenization-only/thread-only control was measured. Do not infer a causal improvement magnitude from cross-experiment differences. See [E30 evidence](../evidence/tool-output-adaptive-20261031.json).',
           '全部515记录和负对保留。负载不同于E29，本轮没有同负载旧on或仅分词／仅线程隔离控制，不能用跨实验差值宣称因果改善幅度。见[E30证据](../evidence/tool-output-adaptive-20261031.json)。'), '',
        '## '+tr('E31/E32 · matched controls and output-growth gate', 'E31/E32 · 同负载隔离与输出增长门槛'), '',
        tr('E31 rotates off, E29 old, E30 on, thread/owner-only and tokenize-only on identical40/48-word workloads. E32 validates off/on/progress on new36/52-word workloads. Thread includes callbacks/queue/owner round trips; token adds only suffix construction and tokenizer without canonical/KV edits. Group differences include timing interactions; they are not additive pure-component costs.',
           'E31同40/48词负载轮换off、E29 old、E30 on、仅线程/owner和仅分词；E32换36/52词负载验证off/on/progress。thread含回调、队列和owner往返；token只增加局部后缀构造／分词，不改canonical/KV。组间差包含调度互动，不能相加为纯部件成本。'), '',
        table(['Experiment','Words','Iterations','Mode vs off','Task saving ms [min,max]','Faster /7','Extra prefill ms','Extra frontend ms'],[[eid,r['words'],r['producer_iterations'],r.get('mode','on'),f"{r['task_ms']['median_saving']:.3f} [{r['task_ms']['min_saving']:.3f},{r['task_ms']['max_saving']:.3f}]",r['task_ms']['positive_pairs'],f"{-r['prefill_sum_ms']['median_saving']:.3f}",f"{-r['stream_front_ms']['median_saving']:.3f}"] for eid in ['E31','E32'] for r in e[eid]['paired_summary']]), '',
        table(['Experiment','Words','Iterations','Baseline','Candidate','Task saving median [min,max] ms','Faster /7'],[[eid,r['words'],r['producer_iterations'],r['baseline'],r['mode'],f"{r['task_ms']['median_saving']:.3f} [{r['task_ms']['min_saving']:.3f},{r['task_ms']['max_saving']:.3f}]",r['task_ms']['positive_pairs']] for eid in ['E31','E32'] for r in e[eid]['contrasts']]), '',
        '![Matched controls](figures/tool-output-isolation.svg)', '',
        '![Growth gate](figures/tool-output-progress.svg)', '',
        tr('Progress: zero speculative updates in42 evaluated burst calls. Still not universally better than timer:36-word paced tasks regress32.246ms (2/7 faster),52-word burst regresses13.203ms. Worst burst versus off regresses30.462ms:23.740ms extra tool wall,5.691ms post-tool, zero streaming prefill/tokenization,0.391ms wrapper and0.058ms join. OS scheduling versus indirect contention is not separately traced. Keep progress optional; no stable tail-reduction claim.',
           '增长策略在42次评价突发调用中推测更新为零，但没有全面优于计时策略：36词持续组反慢32.246ms（2/7更快），52词突发反慢13.203ms。相对关闭最差突发仍慢30.462ms，其中工具墙钟增加23.740ms、工具后增加5.691ms，流式预填充与分词均为零，包装增加0.391ms、join增加0.058ms。未进一步分开系统调度与间接争用；策略保持显式可选，不宣称稳定尾延迟改善。'), '',
        tr('E30 worst pair:54.902ms slower, tool14.659ms plus post-tool41.046ms; extra prefill37.933ms and stream frontend0.918ms. Two burst tools started decode before their16ms completion, leaving113/132ms join waits. The10ms timer cannot distinguish late process reaping from ongoing output. Progress requires a later growing output observation at least10ms after first output, never timer expiry alone; final output cannot trigger speculation. All negatives, raw walls and oracle exclusions retained.',
           'E30最差对慢54.902ms，其中工具多14.659ms、工具后多41.046ms；额外prefill37.933ms、stream前端0.918ms。两次突发在约16ms完成前启动decode，留下113/132ms join等待。10ms计时器无法区分晚回收与持续输出。progress要求首输出后至少10ms再次观测到正文增长，单独超时和最终回调均不触发推测。负对、原始wall和oracle排除均保留。'), '',
        '## '+tr('E33–E35 · producer, process reaping and worker attribution', 'E33–E35 · 工具、进程回收与线程归因'), '',
        tr('E33 repeats36/52-word workloads with off/on/progress/thread controls. E34 freezes an optional macOS exit-event wait and validates new44/50-word workloads with all six original/event modes. EOF events change process waiting only; KV remains resident. Positive values below mean baseline minus candidate; every negative pair remains included.',
           'E33在36/52词同负载补off/on/progress/thread对照；E34冻结可选macOS退出事件等待，换44/50词新负载验证六组原版／事件版。事件模式只改进程等待，KV保持常驻。下表正值为baseline−candidate，所有负对保留。'), '',
        table(['Experiment','Words','Iterations','Baseline','Candidate','Task saving ms [min,max]','Faster /7','Tool saving ms','EOF wait saving ms'], [[eid,r['words'],r['producer_iterations'],r['baseline'],r['mode'],f"{r['task_ms']['median_saving']:.3f} [{r['task_ms']['min_saving']:.3f},{r['task_ms']['max_saving']:.3f}]",r['task_ms']['positive_pairs'],f"{r['tool_sum_ms']['median_saving']:.3f}",f"{r['eof_poll_ms']['median_saving']:.3f}"] for eid in ['E33','E34','E35'] for r in e[eid]['attribution']]), '',
        '![Process attribution](figures/tool-output-reap.svg)', '',
        '![Exit event validation](figures/tool-output-event.svg)', '',
        tr('E33 reproduces a30.529ms worst36-word burst regression:11.706ms extra tool wall,12.008ms extra EOF polling,14.309ms extra post-tool wait. The52-word worst44.648ms regression instead has8.741ms extra startup and34.043ms extra post-tool waiting, with essentially no polling difference. Growth performs no speculative updates in these burst cases. Thus polling explains a concrete portion, not every30ms-class regression.',
           'E33复现36词突发最差慢30.529ms：工具多11.706ms、EOF轮询多12.008ms、工具后多14.309ms。52词最差慢44.648ms却是启动多8.741ms、工具后多34.043ms，轮询差几乎为零。这些突发growth没有推测更新。轮询解释了具体一部分，不能解释所有30ms级回退。'), '',
        tr('The old36-word paced32.246ms growth regression does not repeat: E33 growth saves16.361ms versus timer. Producer wall saves6.135ms, CPU2.924ms and EOF wait changes only−0.030ms (separate medians are not additive). Worker queue/callback observations are much smaller than tens of milliseconds; they do not prove absence of indirect contention. No frequency/bandwidth/scheduler counters establish which hardware effect caused remaining variation. E32 has no such telemetry, so its exact historical pair cannot be retroactively resolved.',
           '旧36词持续组增长策略慢32.246ms未复现：E33相对定时快16.361ms，producer墙钟少6.135ms、CPU少2.924ms，EOF等待只多0.030ms（独立中位数不可相加）。观测到的worker排队／回调远小于几十毫秒，但不能据此排除间接争用。未采频率、带宽和OS调度计数器，不能进一步硬指认硬件原因。E32缺少该观测，旧单对无法追溯定论。'), '',
        tr('See [process stages](data/tool-output-process-stages.csv) and [all matched contrasts](data/tool-output-process-pairs.csv). Child-end precedes sidecar write/exit; after-producer time includes this overhead. eof_poll_ms retains its historical name but measures event waits in event modes. Native default remains unchanged; select on-event/progress-event explicitly in an idle REPL. off-event is the disabled-prefill executor control, used with prefill off.13 sanitizer unit cases per revision plus32/48 actual CLI tasks pass; process EOF/timeout/once checks and default C/HTTP/CLI regression pass. Native14K/full Stream checks remain historical E29 acceptance. No Windows/CUDA, cloud SSE, physical KV release or tail guarantee.',
           '明细见[进程阶段](data/tool-output-process-stages.csv)和[全部配对](data/tool-output-process-pairs.csv)。child-end在sidecar写入／退出前，后段包含观测IO；eof_poll_ms历史名称保留，在事件组表示实际事件等待。默认策略不变，空闲REPL显式选择on-event/progress-event；off-event须配合prefill off作为关闭推测的执行器控制。两版各13个sanitizer单元场景及32／48个真实CLI任务通过；EOF／超时／执行一次检查和默认C/HTTP/CLI回归通过。Native14K／完整Stream仍引用E29历史验收。不宣称Windows/CUDA、云SSE、物理KV释放或尾延迟保证。'), '',
        tr('E35:84 tool calls per mode show maximum EOF waits12.015ms (poll),12.013ms (off-event),12.006ms (growth-event),1.262ms (off-race) and1.263ms (growth-race). Paced executor-only off-race versus off task savings40.677/48.196ms are7/7 positive. Streaming versus the same corrected off-race executor saves152.085/138.815ms (7/7,6/7); burst savings−0.730/+7.770ms are not stable. Growth-race versus growth-event paced medians are−8.112/+6.640ms, not a universal improvement.',
           'E35每模式84次工具调用，EOF等待最大值分别为poll12.015ms、off-event12.013ms、growth-event12.006ms、off-race1.262ms、growth-race1.263ms。持续任务单独修正执行器off-race相对off快40.677／48.196ms，均7/7；流式相对相同修正的关闭组快152.085／138.815ms（7/7、6/7），突发−0.730／+7.770ms并不稳定。growth-race相对growth-event持续组为−8.112／+6.640ms，不是全面改善。'), '',
        tr('E35 worst burst off-race versus off still regresses144.433ms: post-tool115.438ms and cumulative prefill138.381ms extra, with zero EOF wait and zero speculative work. The54-word paced growth-race worst versus growth-event regresses110.100ms: producer wall127.289ms extra (CPU29.921ms, non-CPU97.368ms), owner queue only0.004ms extra, prefill14.954ms less. Those outliers do not support blaming direct worker queueing or the exit wait. Indirect resource contention and environment variation remain unresolved.',
           'E35最差突发off-race相对off仍慢144.433ms：工具后多115.438ms、累计prefill多138.381ms，EOF等待和推测工作均为零。54词持续growth-race相对growth-event最差慢110.100ms，producer墙钟多127.289ms（CPU29.921ms、非CPU97.368ms），owner排队仅多0.004ms，prefill反少14.954ms。这些负对不支持直接归咎于worker排队或退出等待；间接资源争用与环境波动仍未细分。'), '',
        tr('E33 per-tool owner queue medians for timer/growth/thread-only are0.013/0.006/0.014ms, maxima2.210/0.486/11.388ms; callback maxima0.063/0.030/0.074ms. The thread-only queue outlier remains visible; small medians are not an assertion of zero contention. All500 separate syscall observations are in [exit probes](data/tool-output-exit-probes.csv), with concurrent-development rows clearly labeled.',
           'E33逐工具owner排队中位数：timer/growth/thread-only为0.013／0.006／0.014ms，最大2.210／0.486／11.388ms；回调最大0.063／0.030／0.074ms。线程控制的排队异常值仍展示，小中位数不等于没有争用。另500条系统调用观测见[退出探针](data/tool-output-exit-probes.csv)，并发开发期记录单独标注。'), '',
        '![Exit race validation](figures/tool-output-race.svg)', '',
        tr('E34 retains large negatives:44-word burst on-event versus on has a132.809ms worst regression with zero speculative updates; startup adds26.051ms and cumulative prefill105.400ms.50-word burst progress-event versus progress has an82.384ms worst regression, including11.317ms EOF wait and73.088ms post-tool wait. No negative was removed or relabeled as a win.',
           'E34保留大幅负对：44词突发on-event相对on最差慢132.809ms，推测更新仍为零，启动多26.051ms、累计prefill多105.400ms；50词突发progress-event相对progress最差慢82.384ms，含11.317ms EOF等待和73.088ms工具后等待。没有删负样本或把负结果改称收益。'), '',
        tr('A separate100-exit syscall probe found10 waits over1ms, all after kevent registration returned ESRCH with no exit event. Retrying waitpid once is insufficient in that exit-in-progress window. E35 freezes a1ms bounded retry only after ESRCH or observed NOTE_EXIT, otherwise keeping normal event waiting and timeout checks. It validates46/54-word,0/15M-iteration new workloads, off/off-event/progress-event/off-race/progress-race, one warmup plus seven evaluations each. The isolated final200-exit alternating probe triggered no positive waits in either mode, retained as a non-reproduction rather than evidence of a speedup. Runtime success and timing effects are distinct.',
           '独立100次退出系统调用探针中，10次超过1ms的等待全部发生在kevent注册返回ESRCH且未收到退出事件之后；只再试一次waitpid不足以避开该退出窗口。E35冻结仅在ESRCH或已收到NOTE_EXIT后用1ms有界重试，其他情况仍正常等事件并检查超时。换46/54词、0/1500万循环的新负载，off/off-event/progress-event/off-race/progress-race各1预热＋7评价。最终独占200次交替探针两组均未触发正等待，保留为未复现，不能拿它当提速证据。功能正确与性能效果分开。'), '',
        tr('E35 adds13 sanitizer unit cases,40 real CLI tasks and both event/race EOF/status/timeout/once checks. The corrected policy remains opt-in: /tool-output-policy progress-race with prefill on; off-race with prefill off is its executor-only control. The report tables include the exact paired whole-task results and all negatives; old evidence/source archives remain immutable.',
           'E35增加13个sanitizer单元场景、40个真实CLI任务和event/race两路径的EOF／状态／超时／执行一次检查。修正版仍显式可选：/tool-output-policy progress-race配合prefill on；off-race配合prefill off是执行器控制。上表保留逐对完整任务成绩和全部负对；历史证据与实测源码归档不变。'), '',
        '## '+tr('E36/E37 · remaining regressions and lazy consumer launch', 'E36/E37 · 剩余回退隔离与按需启动消费者'), '',
        tr('E36 repeats46/54-word,0/15M-iteration tasks with off-race, dormant consumer, same-growth-gate owner no-op, tokenizer-only and real prefill controls. Every group retains the exit-race correction and identical final receipts/tokens/greedy. E37 freezes lazy consumer creation and validates new42/55-word,0/18M tasks with off/eager/lazy growth. KV remains resident; no snapshot, eviction or build-command change.',
           'E36同46/54词、0/1500万循环负载，比较off-race、休眠消费者、同增长门槛owner空操作、仅分词和真实预填充；退出竞态修正及最终收据／token／greedy保持一致。E37冻结按需创建消费者，再换42/55词、0/1800万循环验证off／原增长／按需增长。KV常驻，无快照、逐出或构建命令优化。'), '',
        table(['Experiment','Words','Iterations','Baseline','Candidate','Task saving ms [min,max]','Faster /7','Producer saving ms','Final prefill saving ms','Final prepare CPU saving ms'],[[eid,r['words'],r['producer_iterations'],r['baseline'],r['mode'],f"{r['task_ms']['median_saving']:.3f} [{r['task_ms']['min_saving']:.3f},{r['task_ms']['max_saving']:.3f}]",r['task_ms']['positive_pairs'],f"{r['producer_wall_ms']['median_saving']:.3f}",f"{r['final_prefill_ms']['median_saving']:.3f}",f"{r['final_prepare_cpu_ms']['median_saving']:.3f}"] for eid in ['E36','E37'] for r in e[eid]['contention_contrasts']]), '',
        '![Same-load isolation](figures/tool-output-contention.svg)', '',
        tr('Inference execution varies even with no consumer: burst off-race has identical work, eight decode calls and2407/2599 evaluated tokens at46/54 words, yet cumulative final prefill spans663.212–773.431ms and652.351–723.838ms. The46-word dormant-consumer worst pair regresses52.427ms: final prefill51.812ms extra, process CPU0.342ms less, producer0.292ms less. The46-word tokenizer-control worst regresses176.074ms: final prefill135.522ms extra but only11.641ms extra process CPU, identical work and zero stream evaluation. Later prepares execute after consumer join; direct simultaneous consumer/owner contention cannot explain this phase. Indirect effects and device/system scheduling remain unseparated; CPU time is not GPU time.',
           '没有消费者时推理执行也会波动：突发off-race的计算量固定为8次decode、46／54词分别2407／2599个evaluated token，累计最终prefill仍为663.212–773.431ms／652.351–723.838ms。46词休眠线程最差对慢52.427ms，prefill多51.812ms，processCPU反少0.342ms，producer少0.292ms。46词仅分词控制最差慢176.074ms，prefill多135.522ms、processCPU仅多11.641ms，计算量相同且推测求值为零。后续prepare在消费者join之后运行，不能用消费者同时抢owner解释该阶段；间接历史效应与设备／系统调度仍未分开，CPU时间不代表GPU时间。'), '',
        tr('Tool slowdown is also real: E36 paced46-word real prefill versus off has one46.110ms negative task pair despite232.001ms less final prefill. Producer wall increases82.061ms, comprising18.017ms CPU and64.044ms non-CPU; owner queue adds0.404ms. Dormant/thread/token controls do not establish a consistent tens-of-milliseconds thread penalty. No hardware counters prove which indirect effect caused producer descheduling. Separate component medians must not be added.',
           '工具阶段也确有回退：E36持续46词真实预填充相对关闭有一对任务慢46.110ms，尽管最终prefill少232.001ms。producer墙钟多82.061ms，其中CPU多18.017ms、非CPU多64.044ms，owner排队仅多0.404ms。休眠／线程／分词控制未建立稳定的几十毫秒线程惩罚；没有硬件计数器进一步证明哪种间接影响导致producer非CPU时间增加。独立分量中位数不可相加。'), '',
        '![Lazy consumer validation](figures/tool-output-lazy.svg)', '',
        tr('Lazy launch removes provably unused work, not the large regressions:42 evaluated burst tool calls create42 eager consumers versus zero lazy consumers, with zero speculative evaluation in both. Eager launch cost sums to paired medians0.079/0.049ms per three-tool burst task. Lazy whole-task medians versus eager regress2.556/1.157ms on burst and8.057/1.786ms on paced sources; all four comparisons are retained as negative. Sustained lazy streaming versus off still saves141.552/186.168ms (6/7,7/7); burst−1.018/−0.706ms has no stable gain. Resource reduction is verified; a latency improvement over eager is not established. Default remains unchanged.',
           '按需启动只消除确定无用的工作，没有消掉大回退：42次评价突发工具调用，原增长策略创建42个消费者、按需版为零，两组推测求值均为零。原线程构造在每个三工具突发任务的逐对中位合计0.079／0.049ms。按需版相对原版完整任务，突发反慢2.556／1.157ms，持续反慢8.057／1.786ms，四项负结果全部保留。按需持续流式相对关闭仍快141.552／186.168ms（6/7、7/7）；突发−1.018／−0.706ms无稳定收益。已证明减少资源操作，未证明比原增长策略更快。默认不切换。'), '',
        tr('E37 worst lazy versus eager paced regressions99.683/165.583ms primarily include producer wall+92.094/+244.519ms, non-CPU+69.954/+223.439ms. In the55-word pair final prefill is67.928ms less, owner queue4.553ms more and final decode count unchanged. Moving a0.05ms constructor cannot explain that165ms pair. Negative results and final timings stay visible; no outlier rejection or evaluation tuning.',
           'E37按需相对原版持续最差慢99.683／165.583ms，producer墙钟分别多92.094／244.519ms、非CPU多69.954／223.439ms；55词该对最终prefill反少67.928ms，owner排队多4.553ms、最终decode数相同。移动约0.05ms的构造不能解释165ms整任务负对。全部负结果保留，不剔除异常值、不据评价调参。'), '',
        tr('Detailed [tasks](data/tool-output-contention-tasks.csv), [all contrasts](data/tool-output-contention-pairs.csv) and [indexed phases](data/tool-output-contention-phases.csv) retain CPU, decode, producer and constructor observations. Process CPU may exceed wall due to parallel workers; final decode savings mean work moved earlier, not less total inference.13/22 native sanitizer cases and40/24 real CLI sanitizer tasks pass, plus final default C/HTTP/CLI regression. Lazy cancellation before launch, exit3, timeout, truncation, Unicode/NUL and once-only capacity are tested; thread-constructor OOM is not injected. Native14K/full Stream remain historical E29 checks. No GPU-frequency or Windows/CUDA conclusion.',
           '明细见[任务](data/tool-output-contention-tasks.csv)、[全部配对](data/tool-output-contention-pairs.csv)、[索引阶段](data/tool-output-contention-phases.csv)，保留CPU、decode、producer和线程构造观测。并行worker可使processCPU超过wall；最终decode减少表示工作提前，不表示总推理工作减少。13／22个原生sanitizer场景、40／24个真实CLI sanitizer任务及最终默认C/HTTP/CLI回归通过。覆盖按需启动前取消、exit3、超时、截断、Unicode/NUL和容量拒绝不重执行；未注入线程构造OOM。Native14K／完整Stream仍为E29历史检查。不作GPU频率或Windows/CUDA结论。'), '',
        '## '+tr('E38/E39 · decode, synchronization and runnable scheduling', 'E38/E39 · decode、同步与可运行调度'), '',
        tr('E38 repeats42/55-word,0/18M-iteration workloads with off/dormant/thread-only/growth controls. E39 validates new43/53-word,0/16M workloads with strict versus deferred internal synchronization, both with and without streaming. Each experiment has1027 CLI records plus96 contemporaneous producer-alone records,1123 total. All commands are prebuilt and identical within case; model/KV stay resident. Standalone stdout is /dev/null rather than the CLI pipe, so only producer computation/scheduling observations are compared, not cross-parent startup or IO cost.',
           'E38在42/55词、0/1800万循环同负载比较关闭／休眠／仅线程／增长；E39换43/53词、0/1600万循环比较逐块／合并内部同步，并保留流式开关。每实验1027条CLI记录＋96条同期producer-alone记录，共1123条。命令预构建，模型／KV常驻。独立producer的stdout为/dev/null，CLI为管道，因此只比较计算／调度观测，不把不同父进程启动或IO差计为收益。'), '',
        table(['Experiment','Words','Iterations','Baseline','Candidate','Task saving ms [min,max]','Faster /7','Final decode saving ms','Final sync saving ms','Final prefill saving ms'],[[eid,r['words'],r['producer_iterations'],r['baseline'],r['mode'],f"{r['task_ms']['median_saving']:.3f} [{r['task_ms']['min_saving']:.3f},{r['task_ms']['max_saving']:.3f}]",r['task_ms']['positive_pairs'],f"{r['final_decode_ms']['median_saving']:.3f}",f"{r['final_sync_ms']['median_saving']:.3f}",f"{r['final_prefill_ms']['median_saving']:.3f}"] for eid in ['E38','E39'] for r in e[eid]['decode_contrasts']]), '',
        '![Decode and scheduler attribution](figures/decode-schedule.svg)', '',
        tr('E38 burst off groups: cumulative final decode-call medians8.478/19.754ms, explicit synchronization518.959/668.833ms; sync owner CPU only0.114/0.313ms. The42-word off max-minus-min prefill difference21.640ms comprises20.305ms more sync and1.336ms more decode, with unchanged token/decode work. The55-word difference22.904ms includes17.699ms more sync. Earlier71–110ms ranges do not recur in these controls; historical data remains intact. Waiting is localized, not measured as pure GPU kernel duration: decode may internally wait and synchronization includes outstanding work plus host wakeup/scheduling.',
           'E38突发关闭组，累计最终decode调用中位8.478／19.754ms，显式同步518.959／668.833ms，sync ownerCPU仅0.114／0.313ms。42词关闭组prefill最大减最小21.640ms，其中sync多20.305ms、decode多1.336ms，token／decode计算量相同；55词差22.904ms，其中sync多17.699ms。旧71–110ms范围本轮未复现，历史数据仍保留。已定位等待阶段，不能当作GPU核函数时长：decode可内部等待，同步还包含未完成工作及host唤醒／调度。'), '',
        tr('The producer is single-threaded. On M5, raw proc counters require Mach scaling125/3; the100ms CPU calibration reports2,400,083 CPU ticks versus100,000,083ns from CLOCK_THREAD_CPUTIME_ID. E38 paced42-word standalone producer reaches56.318ms ready-wait proxy even while the model process is idle; median wall150.745ms versus CPU142.341ms. Off/idle/thread/growth maximum per-tool ready proxies55.041/75.191/79.983/75.607ms are not a monotonic consumer penalty. Retired-instruction medians stay near873–876M. At55 words, standalone max is1.260ms, growth max11.949ms, with all instruction medians about865M. This establishes background CPU supply delay and possible smaller mode-dependent variation, not an attribution to a particular competing PID or proof of zero indirect contention.',
           'producer是单线程。本M5的proc原始计数须乘125/3：100ms CPU校验得到2,400,083个CPU ticks，对照CLOCK_THREAD_CPUTIME_ID为100,000,083ns。E38持续42词，即使模型进程空闲，独立producer的ready等待代理仍达56.318ms，中位墙钟150.745ms、CPU142.341ms；关闭／休眠／仅线程／增长的逐工具最大ready代理55.041／75.191／79.983／75.607ms，不是随消费者工作递增的惩罚。退休指令中位约873–876M。55词独立最大1.260ms、增长最大11.949ms，各模式指令中位约865M。这证明存在背景CPU供给延迟，也保留较小的模式相关变化；没有定位竞争PID，不能声称间接争用为零。'), '',
        tr('Runnable includes running time, so ready proxy=(runnable−CPU)×timebase; blocked proxy=wall−runnable×timebase. Counter updates lag: negative values, including task-summed medians around−13ms, are retained and are not physical negative waiting. Setup/output medians in the paced42-word groups are roughly0.08–0.13ms per tool; large delays are in compute/runnable spans rather than the measured file/output spans. The independent100ms sleep calibration also exposes counter-update error. Paced42-word standalone involuntary switches have median790 versus37 at55 words. Across105 evaluated producer calls per shape (five modes), ready proxy and involuntary-switch Pearson r are0.700/0.405; correlation supports a scheduling mechanism but does not identify the competing process. No exact decomposition of every non-CPU millisecond is claimed.',
           'runnable包含正在运行，故ready代理=(runnable−CPU)×timebase，blocked代理=wall−runnable×timebase。计数更新有滞后，任务合计中位约−13ms等负值保留，不代表物理负等待。持续42词的setup／output逐工具中位约0.08–0.13ms，较大延迟落在compute／runnable跨度，未落在已测文件／输出跨度；独立100ms sleep校验也暴露更新误差。持续42词独立producer的非自愿切换中位790次，55词37次。每shape五模式共105次评价调用，ready代理与非自愿切换的Pearson r为0.700／0.405；相关支持调度机制，但没有定位竞争进程，不宣称精确分解每一毫秒非CPU时间。'), '',
        '![Deferred internal synchronization](figures/deferred-sync.svg)', '',
        tr('E39 removes explicit host barriers only inside a single prefill. Every public return drains pending work; greedy replay and failure still synchronize.43-word tasks save one final explicit sync call;53-word burst saves four. Most apparent sync savings move into decode:53-word burst off-drain versus off-race saves496.522ms explicit sync but adds491.394ms in decode; final prefill saves5.262ms and whole task2.369ms. That is not a497ms speedup. On43-word paced off-drain final prefill actually regresses2.956ms despite a2.616ms task median saving; all negatives remain.',
           'E39只在同一次prefill内部减少显式host屏障，公开返回前排空，greedy重放／失败仍同步。43词任务少一次最终显式同步，53词突发少四次。大部分同步下降移入decode：53词突发off-drain相对off-race，sync少496.522ms，decode多491.394ms，最终prefill只少5.262ms、整任务快2.369ms，不能宣传为497ms提速。43词持续off-drain的最终prefill反慢2.956ms，虽整任务中位快2.616ms；负结果均保留。'), '',
        tr('Matched off-drain task medians versus strict improve2.332/2.616/2.369/3.904ms across four cases; growth-drain improves10.408/8.022/1.331/11.822ms. Only4–5 of7 pairs improve, and worst regressions reach44.039ms (off) and35.597ms (growth). Streaming over the same deferred baseline still saves123.224/169.701ms on paced sources,7/7 each; burst+0.083/−3.296ms has no stable gain. Keep deferred synchronization explicit, defaults unchanged. No evaluation tuning, no outlier deletion, no cross-run subtraction.',
           '四个case中off-drain相对strict整任务中位快2.332／2.616／2.369／3.904ms；growth-drain快10.408／8.022／1.331／11.822ms，但每项仅4–5/7获益，最差仍慢44.039ms（关闭）和35.597ms（增长）。相同合并同步基线上，流式持续任务仍快123.224／169.701ms，均7/7；突发+0.083／−3.296ms无稳定收益。合并同步保持显式选择，默认不变；未据评价调参、未删负样本、不跨轮相减。'), '',
        tr('E39 worst off-drain53-word paced pair regresses44.039ms: producer wall23.529ms extra, CPU19.195ms extra and ready proxy only3.137ms extra; final prefill18.720ms extra, owner queue zero. Producer retired instructions increase0.186%, cycles0.316%, CPU time5.871%. Even without overlapping inference, execution rate varies; these counters are consistent with CPU-rate/supply effects but do not measure frequency or core residency. The arithmetic producer does not represent memory-bandwidth-heavy tools.',
           'E39最差off-drain持续53词对慢44.039ms：producer墙钟多23.529ms、CPU多19.195ms、ready代理仅多3.137ms；最终prefill多18.720ms、owner排队为零。producer退休指令多0.186%、cycles多0.316%、CPU时间多5.871%。没有重叠推理时运行速率也会变化；计数与CPU运行速率／供给变化相容，但没有直接测频率或核心驻留。算术producer也不能代表内存带宽密集工具。'), '',
        tr('Detailed [tasks](data/decode-schedule-tasks.csv), [matched pairs](data/decode-schedule-pairs.csv), [all producers](data/decode-schedule-producers.csv) and [indexed decode phases](data/decode-schedule-phases.csv) retain raw counters and negative proxies. E38/E39 have22/30 native sanitizer cases and32/32 real CLI sanitizer tasks plus24/24 producer controls; final E39 source passes14,357-token deferred Native, full strict Stream and default C/HTTP/CLI regression. Long/Stream checks are E39 acceptance, not a relabeling of E38 source. No injected decode failure or thread-constructor OOM; no Windows/CUDA or GPU hardware timestamps.',
           '明细见[任务](data/decode-schedule-tasks.csv)、[配对](data/decode-schedule-pairs.csv)、[全部producer](data/decode-schedule-producers.csv)和[decode阶段](data/decode-schedule-phases.csv)，保留原始计数与负代理值。E38/E39通过22／30个原生sanitizer场景、32／32个真实CLI sanitizer任务及24／24个producer控制；E39当前源码通过14,357-token合并同步Native、完整strict Stream与默认C/HTTP/CLI回归。长上下文／Stream是E39验收，不将其标为E38源码重跑。未注入decode失败或线程构造OOM；未验收Windows/CUDA或GPU硬件时间戳。'), '',
        tr('Interpretation is checked against the pinned [Metal synchronization code](https://github.com/ggml-org/llama.cpp/blob/f1e44dcc11d8802d107bd7331a3d3fd3e6f57b93/ggml/src/ggml-metal/ggml-metal-context.m), [XNU runnable timer lifecycle](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/kern/sched_prim.c) and [task timer aggregation](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/kern/task.c). The causal limits above are inferences from code plus measured controls, not hardware trace attribution.',
           '口径对照固定版本[Metal同步实现](https://github.com/ggml-org/llama.cpp/blob/f1e44dcc11d8802d107bd7331a3d3fd3e6f57b93/ggml/src/ggml-metal/ggml-metal-context.m)、[XNU runnable计时生命周期](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/kern/sched_prim.c)和[task计数汇总](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/kern/task.c)。上述因果边界是代码与对照观测的推断，不是硬件trace归因。'), '',
        '## '+tr('E40/E41 · model CPU source isolation', 'E40/E41 · 模型进程CPU竞争源隔离'), '',
        tr('E38 producer-alone still kept the model alive. E40/E41 therefore bracket resident observations with model-absent-before and model-absent-after controls. Every rep opens a fresh model, passes a real four-Native/three-tool fixture, observes immediately after completion and again after100ms idle, then exits the model normally. E40 initializes with prefill off; E41 with growth streaming enabled. Both use43/53-word,16M producers, three per state, one warmup plus seven evaluations:192 producer controls,16 fixture records and3 status records=211 each. Fixed order and time drift remain; model CPU is sampled across the whole producer subprocess, a slightly wider interval than producer main.',
           'E38的producer-alone仍让模型存活。E40/E41因此用加载前／退出后两侧absent控制夹住resident观测：每rep重开模型，通过真实四次Native／三工具fixture，完成后立即测、静置100ms后再测，然后正常退出模型。E40以prefill关闭任务初始化，E41开启增长流式。均43/53词、1600万循环，每状态3次producer，1预热＋7评价：192条producer、16条fixture、3条状态，共211条／实验。顺序固定、时间漂移仍存在；模型CPU采样覆盖整个producer子进程，比producer main窗口略宽。'), '',
        table(['Experiment','Words','State','Calls','Producer wall median [min,max] ms','Producer CPU median ms','Ready proxy max ms','Model CPU median / max ms'],[[eid,r['words'],r['mode'],r['samples'],f"{r['wall_ms']['median']:.3f} [{r['wall_ms']['min']:.3f},{r['wall_ms']['max']:.3f}]",f"{r['cpu_ms']['median']:.3f}",f"{r['ready_proxy_ms']['max']:.3f}",f"{r['model_cpu_ms']['median']:.4f} / {r['model_cpu_ms']['max']:.4f}"] for eid in ['E40','E41'] for r in e[eid]['idle_summary']]), '',
        '![Model CPU source controls](figures/model-idle.svg)', '',
        tr('The off-warm probe E40 does not reproduce a large wait; resident model CPU maxima are0.1324ms or less. The streaming-warm E41 does:53-word settled-idle producer ready proxy70.635ms, main wall184.853ms, own CPU108.246ms, compute184.680ms, output0.096ms and312 involuntary switches. During its whole subprocess the model uses0.4223ms CPU and the collector1.015ms. A model-absent-before43-word call also has27.228ms ready proxy, wall125.423ms versus CPU98.741ms. Thus substantial runnable delay exists without an active consumer, with negligible model CPU and even with the model absent. Residual model-thread CPU spinning is not supported as the explanation for these samples.',
           'E40关闭预热组没有复现大等待，resident模型CPU最高不超过0.1324ms。E41流式预热组复现了：53词静置后producer ready代理70.635ms，main墙钟184.853ms、自身CPU108.246ms、compute184.680ms、output0.096ms、非自愿切换312次；覆盖整个子进程的窗口里模型CPU仅0.4223ms、采集器1.015ms。另43词加载模型前也有27.228ms ready代理，墙钟125.423ms、CPU98.741ms。因此无活动消费者、模型CPU很少甚至模型进程不存在时，也会出现显著可运行延迟；这些样本不支持以模型残留线程忙转解释。'), '',
        tr('This distinguishes runnable CPU supply from own model-process CPU occupancy, not the identity of all competing work. No other PID inventory was collected. GPU/driver, cache, thermal and CPU/core-rate history are not fully profiled; no assertion excludes all indirect effects. Negative ready proxies remain. The absent-before/after bracket averages in CSV are descriptive, not a drift-corrected causal estimator. Both complete diagnostic runs are retained, including the initial non-reproduction. They add no runtime optimization or default change.',
           '这区分了可运行CPU供给延迟与自有模型进程CPU占用，没有定位所有竞争工作；没有采其他PID名单。GPU／驱动、缓存、热状态、CPU／核心运行速率的历史效应未全面profile，不声称排除全部间接影响。负ready代理保留。CSV的前后absent平均只作描述，不能当作已校正时间漂移的因果估计。两次完整诊断均保留，包括第一轮未复现；未据此再加运行时优化或改变默认。'), '',
        tr('See [all controls](data/model-idle-controls.csv), [summaries](data/model-idle-summary.csv), [bracket comparisons](data/model-idle-pairs.csv).32 model fixtures pass canonical/token/greedy and96 actual tool invocations;32 owned model processes close normally. These are CPU-source diagnostics using the E39 qualified binary, not192 extra autonomous model tasks per experiment or a general tool benchmark.',
           '见[全部控制](data/model-idle-controls.csv)、[统计](data/model-idle-summary.csv)、[前后括号对照](data/model-idle-pairs.csv)。32个模型fixture通过canonical／token／greedy和96次实际工具调用，32个自有模型进程正常关闭。这是使用E39已验收二进制的CPU竞争源诊断，不是每轮192个额外自主模型任务或通用工具能力成绩。'), '',
        '## '+tr('L01 · historical observations (reported only)', 'L01 · 早期观测（仅文档读数）'), '',
        tr('The earlier external-service concurrency, context-budget marker fixture, and memcpy timings are retained in [L01](../evidence/legacy-observations-20261020.json). Their original CSV/raw timing samples are unavailable, so they do not support the headline optimization claims. Concurrency p50/p95 values are historical reported figures, not recalculated tail estimates. The marker fixture measures retrieval coverage and budget, not answer accuracy. The concurrency-snapshot document was a design only and was never run.',
           '早期外部服务并发、上下文预算标记夹具与 memcpy 读数保存在 [L01](../evidence/legacy-observations-20261020.json)。原始 CSV／计时样本缺失，因此不支撑首页优化结论。并发 p50/p95 为历史文档读数，不是重新计算的尾部统计。标记夹具只测检索覆盖与预算，不代表回答准确率。并发快照方案文档只是设计，从未运行。'), '',
        table(['Concurrency','OK / requests','Wall s','Reported p50 / p95 s','Reported req/s'],[
            [r['concurrency'],f"{r['ok']} / {r['requests']}",r['wall_s'],f"{r['reported_p50_s']} / {r['reported_p95_s']}",r['reported_rps']]
            for r in e['L01']['external_service_capacity']['rows']]), '',
        table(['Budget fixture (12 files, 10136 bytes)','Markers found / 8','Worst budget bytes','Noise'],[
            [r['variant'],f"{r['hits']} / 8",r['worst_budget_bytes'] if r['worst_budget_bytes'] is not None else 'no finite budget',r['noise'] if r['noise'] is not None else '—']
            for r in e['L01']['context_budget_fixture']['rows']]), '',
        table(['Copy microbenchmark bytes','Reported approximate ns/op'],[
            [r['bytes'],r['approx_ns_per_op']] for r in e['L01']['copy_microbench']['reported']]), '',
        '## '+tr('Reproduce and publish', '复核与发布'), '',
        tr('No model or network is needed to recompute this report from the archived evidence:', '从归档证据重新生成／复核报告，不需要模型与网络：'), '',
        '```sh\npython3 tools/build_benchmark_report.py --check\npython3 tools/build_benchmark_report.py\n# Optional: ReportLab + macOS sips for SVG/PNG charts\npython3 tools/render_benchmark_figures.py\n```', '',
        tr('See [METHODOLOGY.md](METHODOLOGY.md) for model rerun commands and exclusions, [PUBLISHING.md](PUBLISHING.md) for release/privacy, [catalog.json](data/catalog.json) for hashes, and [run-inventory.json](data/run-inventory.json) for official/superseded runs. Follow the [maintenance workflow](MAINTENANCE.md) and [change record](CHANGELOG.md). Report generation is offline; E13–E49 were measured separately. No GitHub push.',
           '模型重跑与排除项见 [METHODOLOGY.md](METHODOLOGY.md)，发布与隐私见 [PUBLISHING.md](PUBLISHING.md)，哈希见 [catalog.json](data/catalog.json)，正式／被替代运行见 [run-inventory.json](data/run-inventory.json)。后续遵循 [维护流程](MAINTENANCE.md) 和 [变更记录](CHANGELOG.md)。报告生成本身离线，E13–E49 为独立实测；未推送 GitHub。'), '']
    index=lines.index('## '+tr('L01 · historical observations (reported only)', 'L01 · 早期观测（仅文档读数）'))
    lines[index:index]=metal_report(e,zh)+tailcapacity_report(e,zh)+contention_report(e,zh)
    return '\n'.join(lines)


def outputs(evidence, refresh_inventory=False):
    catalog, raw, metrics = data_products(evidence)
    checks = check_summaries(evidence)
    validation = validation_rows(evidence)
    shape_cells=[dict(experiment=eid,**{k:v for k,v in c.items() if k!='errors'},**{k:v for k,v in error.items() if k!='samples'})
                 for eid in ['E16','E17'] for c in evidence[eid]['cell_summary'] for error in c['errors']]
    frozen_cells=[dict(experiment='E19',**{k:v for k,v in c.items() if k!='errors'},**{k:v for k,v in error.items() if k!='samples'})
                  for c in evidence['E19']['cell_summary'] for error in c['errors']]
    traces=[r for r in evidence['E20']['filter_rows'] if r['kind']=='filter_prediction']
    kalman_cells=[]
    for batch in [256,64]:
        for base_id,base in enumerate(evidence['E20']['method']['bases']):
            for prefix in [1792,3840,5504]:
                for n in [11,18,26,88,208,400,688]:
                    group=[r for r in traces if r['batch_tokens']==batch and r['base_id']==base_id and r['prefix_tokens']==prefix and r['new_tokens']==n]
                    assert len(group)==7
                    for mode in ['base','ewma','fixed','scaled']:
                        errors=[r[mode+'_ms']-r['actual_ms'] for r in group]
                        kalman_cells.append(dict(batch_tokens=batch,base=base,prefix_tokens=prefix,new_tokens=n,mode=mode,samples=len(group),
                            mae_ms=statistics.mean(abs(x) for x in errors),rmse_ms=math.sqrt(statistics.mean(x*x for x in errors)),
                            max_abs_error_ms=max(abs(x) for x in errors),mean_signed_error_ms=statistics.mean(errors)))
    from bench_restore_timing import parts as timing_parts,common_parts
    from bench_restore_balance import parts as balance_parts
    from bench_tool_taskcost import parts as taskcost_parts
    stage_rows=[];stage_summaries=[];control_pairs=[];restore_tasks=[];estimate_audit=[];taskcost_pairs=[]
    for eid,parts in [('E22',timing_parts),('E23',balance_parts),('E24',taskcost_parts)]:
        for index,r in enumerate(evidence[eid]['rows']):
            if r['kind']=='timing' and r['phase']=='evaluation':
                stage_rows.append(dict(experiment=eid,record_index=index,**{k:r[k] for k in ['scenario','words','mode','rep','step']},**parts(r)))
        for g in evidence[eid]['stage_summary']:
            for metric,value in g.items():
                if isinstance(value,dict):stage_summaries.append(dict(experiment=eid,**{k:g[k] for k in ['scenario','words','mode','samples']},metric=metric,**value))
        for g in evidence[eid]['paired_summary']:
            for metric,value in g.items():
                if isinstance(value,dict):control_pairs.append(dict(experiment=eid,**{k:g[k] for k in ['scenario','words','baseline','mode','pairs']},both_lower_pairs=g.get('both_wait_and_hold_lower_pairs',''),metric=metric,**value))
        for g in evidence[eid]['summary']:
            if g['kind']=='task_timing':restore_tasks.append(dict(experiment=eid,**{k:g[k] for k in ['scenario','words','mode','samples']},**{f'{metric}_{stat}':value[stat] for metric,value in g.items() if isinstance(value,dict) for stat in ['median','min','max']}))
    for index,r in enumerate(evidence['E21']['rows']):
        if r['kind']=='timing' and r['phase']=='evaluation' and r['mode'].startswith('restore_'):
            estimate_audit.append(dict(experiment='E21',record_index=index,**{k:r[k] for k in ['scenario','words','mode','rep','step']},**common_parts(r)))
    from bench_tool_taskcost import COMPARISONS,COMPONENTS
    for scenario in ['read','compile_test']:
        for words in [512,2304]:
            for baseline,mode in COMPARISONS:
                select=lambda m:{r['rep']:r for r in evidence['E24']['task_accounting'] if r['phase']=='evaluation' and r['scenario']==scenario and r['words']==words and r['mode']==m}
                a,b=select(baseline),select(mode)
                for rep in range(7):
                    values={f+'_saving':a[rep][f]-b[rep][f] for f in ['task_ms',*COMPONENTS,'save_sum_ms']}
                    assert math.isclose(values['task_ms_saving'],sum(values[f+'_saving'] for f in COMPONENTS),abs_tol=1e-8)
                    taskcost_pairs.append(dict(experiment='E24',scenario=scenario,words=words,baseline=baseline,mode=mode,rep=rep,
                        baseline_task_index=a[rep]['record_index'],candidate_task_index=b[rep]['record_index'],
                        baseline_call_indices=a[rep]['source_call_indices'],candidate_call_indices=b[rep]['source_call_indices'],**values))
    stage_fields=['experiment','record_index','scenario','words','mode','rep','step']+sorted(set().union(*(set(r) for r in stage_rows))-{'experiment','record_index','scenario','words','mode','rep','step'})
    from bench_tool_isolate import parts as isolate_parts
    isolated=[dict(record_index=i,mode=r['mode'],phase=r['phase'],rep=r['rep'],step=r['step'],retained_file_bytes=r['restored_bytes'] if r['hold_snapshot_file'] else 0,retained_after_unlink_ms=(r['drop_end_ns']-r['early_drop_end_ns'])/1e6 if r['hold_snapshot_file'] else 0,**isolate_parts(r)) for i,r in enumerate(evidence['E25']['rows']) if r['kind']=='timing']
    isolated_fields=['record_index','mode','phase','rep','step']+sorted(set().union(*(set(r) for r in isolated))-{'record_index','mode','phase','rep','step'})
    isolated_pair_summary=[dict(baseline=r['baseline'],mode=r['mode'],pairs=r['pairs'],metric=f,**v) for r in evidence['E25']['paired_summary'] for f,v in r.items() if isinstance(v,dict)]
    from bench_tool_launch import parts as launch_parts
    launch_rows=[dict(record_index=i,mode=r['mode'],phase=r['phase'],rep=r['rep'],step=r['step'],**launch_parts(r)) for i,r in enumerate(evidence['E26']['rows']) if r['kind']=='timing']
    launch_fields=['record_index','mode','phase','rep','step']+sorted(set().union(*(set(r) for r in launch_rows))-{'record_index','mode','phase','rep','step'})
    launch_pairs=[dict(baseline=r['baseline'],mode=r['mode'],pairs=r['pairs'],metric=f,**v) for r in evidence['E26']['paired_summary'] for f,v in r.items() if isinstance(v,dict)]
    launch_stats=[dict(compiled_this_call=r['compiled_this_call'],execution=r['execution'],samples=r['samples'],metric=f,**v) for r in evidence['E26']['launch_summary'] for f,v in r.items() if isinstance(v,dict)]
    resident_calls=[dict(record_index=i,**r) for i,r in enumerate(evidence['E27']['rows']) if r['kind']=='timing']
    resident_stream=[{k:v for k,v in r.items() if k!='arrivals'} for r in evidence['E28']['tasks']]
    resident_arrivals=[dict(record_index=i,words=r['words'],producer_iterations=r['producer_iterations'],mode=r['mode'],rep=r['rep'],**a) for i,r in enumerate(evidence['E28']['rows']) if r['kind']=='timing' for a in r['arrivals']]
    resident_summaries=[dict(experiment=eid,**{k:v for k,v in r.items() if not isinstance(v,dict)},metric=f,**v) for eid in ['E27','E28'] for r in evidence[eid]['summary'] for f,v in r.items() if isinstance(v,dict)]
    resident_pairs=[dict(experiment=eid,**{k:v for k,v in r.items() if not isinstance(v,dict)},metric=f,**v) for eid in ['E27','E28'] for r in evidence[eid]['paired_summary'] for f,v in r.items() if isinstance(v,dict)]
    def union_fields(rows):return sorted(set().union(*(set(r) for r in rows)))
    from bench_tool_output import tasks as output_tasks
    output_summary=[dict(**{k:v for k,v in r.items() if not isinstance(v,dict)},metric=f,**v) for r in evidence['E29']['summary'] for f,v in r.items() if isinstance(v,dict)]
    output_pairs=[dict(**{k:v for k,v in r.items() if not isinstance(v,dict)},metric=f,**v) for r in evidence['E29']['paired_summary'] for f,v in r.items() if isinstance(v,dict)]
    output_detail=[dict(record_index=i,**r) for i,r in enumerate(evidence['E29']['rows']) if r['kind'] in ('step','tool')]
    isolation_rows=[dict(experiment=eid,record_index=i,**r) for eid in ['E31','E32'] for i,r in enumerate(evidence[eid]['rows']) if r['kind'] in ('task_timing','tool','step')]
    from analyze_output_reap import stages
    reap_rows=[dict(experiment=eid,**r) for eid in ['E33','E34','E35'] for r in stages(evidence[eid]['rows'])]
    reap_pairs=[dict(experiment=eid,**{k:v for k,v in r.items() if not isinstance(v,dict)},metric=f,**v) for eid in ['E33','E34','E35'] for r in evidence[eid]['attribution'] for f,v in r.items() if isinstance(v,dict)]
    from analyze_model_idle import controls as idle_controls
    idle_rows=[dict(experiment=eid,**r) for eid in ['E40','E41'] for r in idle_controls(evidence[eid]['rows'],False)]
    idle_summaries=[dict(experiment=eid,words=r['words'],mode=r['mode'],samples=r['samples'],metric=f,**v) for eid in ['E40','E41'] for r in evidence[eid]['idle_summary'] for f,v in r.items() if isinstance(v,dict)]
    idle_pairs=[dict(experiment=eid,**r) for eid in ['E40','E41'] for r in evidence[eid]['idle_pairs']]
    from analyze_metal_timeline import scope as metal_scope
    metal_rows=[dict(experiment=eid,**r) for eid in ['E42','E43','E44','E45','E46','E47','E49'] for r in evidence[eid]['metal_tasks']]
    metal_pairs=[dict(experiment=eid,**{k:v for k,v in r.items() if not isinstance(v,dict)},metric=f,**v) for eid in ['E42','E43','E44','E45','E46','E47','E49'] for r in evidence[eid]['metal_contrasts'] for f,v in r.items() if isinstance(v,dict)]
    metal_raw=[];metal_scopes=[];metal_waits=[]
    from analyze_batch_trace import batches as batch_samples
    batch_rows=[dict(experiment=eid,**r) for eid in ['E47','E48','E49'] for r in batch_samples(evidence[eid])]
    isolation_rows=evidence['E48']['isolation_samples']
    isolation_pairs=[dict(**{k:v for k,v in r.items() if not isinstance(v,dict)},metric=f,**v) for r in evidence['E48']['isolation_contrasts'] for f,v in r.items() if isinstance(v,dict)]
    for eid in ['E42','E43','E44','E45','E46','E47','E48','E49']:
        groups={}
        for i,r in enumerate(evidence[eid]['metal_commands']):
            if 'scope_id' in r:groups.setdefault(r['scope_id'],[]).append(r);metal_raw.append(dict(experiment=eid,aux_record_index=i,**r))
        owners={r.get('metal',{}).get('scope_id',r.get('scope_id')):(i,r) for i,r in enumerate(evidence[eid]['rows']) if 'metal' in r or r['kind']=='isolation'}
        for records in groups.values():
            commands={r['cb']:r for r in records if r['kind']=='metal_command'}
            for w in records:
                if w['kind']!='metal_wait':continue
                i,r=owners[w['scope_id']];c=commands[w['cb']]
                part=metal_scope(list(commands.values())+[w]);label={0:'other',1:'decode',2:'sync'}[w['api_phase']]
                metal_waits.append(dict(experiment=eid,record_index=i,**{k:r[k] for k in ['phase','words','producer_iterations','mode','rep','pressure','policy','prefix_tokens','new_tokens'] if k in r},**w,**{k:v for k,v in part.items() if k.startswith(label+'_')},notify_registered=c.get('notify_registered',False),notify_begin=c.get('notify_begin',0),notify_end=c.get('notify_end',0)))
        for i,r in enumerate(evidence[eid]['rows']):
            if r['kind']=='isolation':metal_scopes.append(dict(experiment=eid,record_index=i,**{k:r[k] for k in ['kind','phase','pressure','policy','rep','prefix_tokens','new_tokens']},scope_id=r['scope_id'],**metal_scope(groups.get(r['scope_id'],[]))))
            if 'metal' in r:metal_scopes.append(dict(experiment=eid,record_index=i,**{k:r[k] for k in ['kind','phase','words','producer_iterations','mode','rep']},scope_id=r['metal']['scope_id'],**metal_scope(groups.get(r['metal']['scope_id'],[]))))
    decode_rows=[dict(experiment=eid,**r) for eid in ['E38','E39'] for r in evidence[eid]['decode_tasks']]
    decode_pairs=[dict(experiment=eid,**{k:v for k,v in r.items() if not isinstance(v,dict)},metric=f,**v) for eid in ['E38','E39'] for r in evidence[eid]['decode_contrasts'] for f,v in r.items() if isinstance(v,dict)]
    from analyze_decode_schedule import producer
    decode_producers=[dict(experiment=eid,record_index=i,**{k:r[k] for k in ['kind','words','producer_iterations','mode','rep','phase']},**{'raw_'+k:v for k,v in r['producer'].items()},**producer(r['producer'])) for eid in ['E38','E39'] for i,r in enumerate(evidence[eid]['rows']) if r['kind'] in ('tool','producer_control')]
    decode_phases=[dict(experiment=eid,record_index=i,**{k:v for k,v in r.items() if not isinstance(v,dict)}) for eid in ['E38','E39'] for i,r in enumerate(evidence[eid]['rows']) if r['kind'] in ('step','tool')]
    contention_rows=[dict(experiment=eid,**r) for eid in ['E36','E37'] for r in evidence[eid]['contention_tasks']]
    contention_pairs=[dict(experiment=eid,**{k:v for k,v in r.items() if not isinstance(v,dict)},metric=f,**v) for eid in ['E36','E37'] for r in evidence[eid]['contention_contrasts'] for f,v in r.items() if isinstance(v,dict)]
    contention_phases=[dict(experiment=eid,record_index=i,**{k:v for k,v in r.items() if not isinstance(v,dict)},**{'producer_'+k:v for k,v in r.get('producer',{}).items()}) for eid in ['E36','E37'] for i,r in enumerate(evidence[eid]['rows']) if r['kind'] in ('step','tool')]
    probe_rows=[dict(stage=p['stage'],**r) for p in evidence['E35']['exit_probes'] for r in p['rows']]
    files = {
        'docs/benchmarks/data/model-idle-controls.csv':csv_text(union_fields(idle_rows),idle_rows),
        'docs/benchmarks/data/model-idle-summary.csv':csv_text(union_fields(idle_summaries),idle_summaries),
        'docs/benchmarks/data/model-idle-pairs.csv':csv_text(union_fields(idle_pairs),idle_pairs),
        'docs/benchmarks/data/tailwait-old86.csv':csv_text(union_fields(old86_steps(evidence['E44'])),old86_steps(evidence['E44'])),
        'docs/benchmarks/data/tailwait-worst.csv':csv_text(union_fields(tailwait_worst(evidence['E45'])),tailwait_worst(evidence['E45'])),
        'docs/benchmarks/data/capacity-worst.csv':csv_text(union_fields(capacity_worst(evidence['E46'])),capacity_worst(evidence['E46'])),
        'docs/benchmarks/data/capacity-runs.csv':csv_text(['id','capacity','block','row_begin','row_end','metal_compute_mib','cpu_compute_mib','physical_kv_mib','child_evidence_sha256','metal_raw_sha256','measured_source_archive_sha256'],[dict(**{k:v for k,v in r.items() if k!='allocation_mib'},**{k+'_mib':v for k,v in r['allocation_mib'].items()}) for r in evidence['E46']['capacity_runs']]),
        'docs/benchmarks/data/metal-batches.csv':csv_text(union_fields(batch_rows),batch_rows),
        'docs/benchmarks/data/device-isolation-samples.csv':csv_text(union_fields(isolation_rows),isolation_rows),
        'docs/benchmarks/data/device-isolation-pairs.csv':csv_text(union_fields(isolation_pairs),isolation_pairs),
        'docs/benchmarks/data/metal-tasks.csv':csv_text(union_fields(metal_rows),metal_rows),
        'docs/benchmarks/data/metal-pairs.csv':csv_text(union_fields(metal_pairs),metal_pairs),
        'docs/benchmarks/data/metal-commands.csv':csv_text(union_fields(metal_raw),[r for r in metal_raw if r['kind']=='metal_command']),
        'docs/benchmarks/data/metal-waits.csv':csv_text(union_fields(metal_waits),metal_waits),
        'docs/benchmarks/data/metal-scopes.csv':csv_text(union_fields(metal_scopes),metal_scopes),
        'docs/benchmarks/data/decode-schedule-tasks.csv':csv_text(union_fields(decode_rows),decode_rows),
        'docs/benchmarks/data/decode-schedule-pairs.csv':csv_text(union_fields(decode_pairs),decode_pairs),
        'docs/benchmarks/data/decode-schedule-producers.csv':csv_text(union_fields(decode_producers),decode_producers),
        'docs/benchmarks/data/decode-schedule-phases.csv':csv_text(union_fields(decode_phases),decode_phases),
        'docs/benchmarks/data/tool-output-contention-tasks.csv':csv_text(union_fields(contention_rows),contention_rows),
        'docs/benchmarks/data/tool-output-contention-pairs.csv':csv_text(union_fields(contention_pairs),contention_pairs),
        'docs/benchmarks/data/tool-output-contention-phases.csv':csv_text(union_fields(contention_phases),contention_phases),
        'docs/benchmarks/data/tool-output-exit-probes.csv':csv_text(union_fields(probe_rows),probe_rows),
        'docs/benchmarks/data/tool-output-process-stages.csv':csv_text(union_fields(reap_rows),reap_rows),
        'docs/benchmarks/data/tool-output-process-pairs.csv':csv_text(union_fields(reap_pairs),reap_pairs),
        'docs/benchmarks/data/tool-output-isolation.csv':csv_text(union_fields(isolation_rows),isolation_rows),
        'docs/benchmarks/data/tool-output-tasks.csv':csv_text(union_fields(output_tasks(evidence['E29']['rows'])),output_tasks(evidence['E29']['rows'])),
        'docs/benchmarks/data/tool-output-pairs.csv':csv_text(union_fields(evidence['E29']['pair_rows']),evidence['E29']['pair_rows']),
        'docs/benchmarks/data/tool-output-stages.csv':csv_text(union_fields(output_detail),output_detail),
        'docs/benchmarks/data/tool-output-summary.csv':csv_text(union_fields(output_summary),output_summary),
        'docs/benchmarks/data/tool-output-pair-summary.csv':csv_text(union_fields(output_pairs),output_pairs),
        'docs/benchmarks/data/resident-flow-calls.csv':csv_text(list(resident_calls[0]),resident_calls),
        'docs/benchmarks/data/resident-flow-tasks.csv':csv_text(list(evidence['E27']['tasks'][0]),evidence['E27']['tasks']),
        'docs/benchmarks/data/resident-flow-pairs.csv':csv_text(list(evidence['E27']['pair_rows'][0]),evidence['E27']['pair_rows']),
        'docs/benchmarks/data/resident-stream-tasks.csv':csv_text(list(resident_stream[0]),resident_stream),
        'docs/benchmarks/data/resident-stream-pairs.csv':csv_text(list(evidence['E28']['pair_rows'][0]),evidence['E28']['pair_rows']),
        'docs/benchmarks/data/resident-stream-arrivals.csv':csv_text(list(resident_arrivals[0]),resident_arrivals),
        'docs/benchmarks/data/resident-summary.csv':csv_text(union_fields(resident_summaries),resident_summaries),
        'docs/benchmarks/data/resident-pair-summary.csv':csv_text(union_fields(resident_pairs),resident_pairs),
        'docs/benchmarks/data/tool-launch-stages.csv':csv_text(launch_fields,launch_rows),
        'docs/benchmarks/data/tool-launch-tasks.csv':csv_text(list(evidence['E26']['task_accounting'][0]),evidence['E26']['task_accounting']),
        'docs/benchmarks/data/tool-launch-pairs.csv':csv_text(list(evidence['E26']['pair_rows'][0]),evidence['E26']['pair_rows']),
        'docs/benchmarks/data/tool-launch-pair-summary.csv':csv_text(list(launch_pairs[0]),launch_pairs),
        'docs/benchmarks/data/tool-launch-executions.csv':csv_text(list(launch_stats[0]),launch_stats),
        'docs/benchmarks/data/tool-isolate-resources.csv':csv_text(isolated_fields,isolated),
        'docs/benchmarks/data/tool-isolate-tasks.csv':csv_text(list(evidence['E25']['task_accounting'][0]),evidence['E25']['task_accounting']),
        'docs/benchmarks/data/tool-isolate-pairs.csv':csv_text(list(evidence['E25']['pair_rows'][0]),evidence['E25']['pair_rows']),
        'docs/benchmarks/data/tool-isolate-pair-summary.csv':csv_text(list(isolated_pair_summary[0]),isolated_pair_summary),
        'docs/benchmarks/data/tool-taskcost-pairs.csv':csv_text(list(taskcost_pairs[0]),taskcost_pairs),
        'docs/benchmarks/data/tool-taskcost-accounting.csv':csv_text(list(evidence['E24']['task_accounting'][0]),evidence['E24']['task_accounting']),
        'docs/benchmarks/data/tool-taskcost-e23-audit.csv':csv_text(list(evidence['E24']['e23_audit']['pairs'][0]),evidence['E24']['e23_audit']['pairs']),
        'docs/benchmarks/data/restore-stages.csv':csv_text(stage_fields,stage_rows),
        'docs/benchmarks/data/restore-stage-summary.csv':csv_text(['experiment','scenario','words','mode','samples','metric','mean','median','min','max'],stage_summaries),
        'docs/benchmarks/data/restore-control-pairs.csv':csv_text(['experiment','scenario','words','baseline','mode','pairs','both_lower_pairs','metric','median_saving_ms','min_saving_ms','max_saving_ms','positive_pairs'],control_pairs),
        'docs/benchmarks/data/restore-task-summary.csv':csv_text(list(restore_tasks[0]),restore_tasks),
        'docs/benchmarks/data/restore-estimate-audit.csv':csv_text(list(estimate_audit[0]),estimate_audit),
        'docs/benchmarks/data/tool-prepare-restore-components.csv':csv_text(list(tool_restore_components(evidence['E21'])[0]),tool_restore_components(evidence['E21'])),
        'docs/benchmarks/data/tool-prepare-tasks.csv':csv_text(list(next(r for r in evidence['E21']['rows'] if r['kind']=='task_timing')),
            [r for r in evidence['E21']['rows'] if r['kind']=='task_timing']),
        'docs/benchmarks/data/tool-prepare-summary.csv':csv_text(['scenario','words','mode','samples','task_median_ms','task_min_ms','task_max_ms','wait_sum_median_ms','tool_sum_median_ms','save_sum_median_ms','early_hold_sum_median_ms'],[
            dict(scenario=r['scenario'],words=r['words'],mode=r['mode'],samples=r['samples'],task_median_ms=r['task_ms']['median'],task_min_ms=r['task_ms']['min'],task_max_ms=r['task_ms']['max'],
                wait_sum_median_ms=r['post_tool_sum_ms']['median'],tool_sum_median_ms=r['tool_sum_ms']['median'],save_sum_median_ms=r['save_sum_ms']['median'],early_hold_sum_median_ms=r['early_hold_sum_ms']['median'])
            for r in evidence['E21']['summary'] if r['kind']=='task_timing']),
        'docs/benchmarks/data/tool-prepare-pairs.csv':csv_text(['scenario','words','baseline','mode','pairs','metric','median_saving_ms','min_saving_ms','max_saving_ms','positive_pairs'],[
            dict(**{k:r[k] for k in ['scenario','words','baseline','mode','pairs']},metric=metric,**r[metric])
            for r in evidence['E21']['paired_summary'] for metric in ['task_ms','post_tool_sum_ms','tool_sum_ms','early_hold_sum_ms']]),
        'docs/benchmarks/data/kalman-summary.csv':csv_text(list(evidence['E20']['prediction_summary'][0]),evidence['E20']['prediction_summary']),
        'docs/benchmarks/data/kalman-diagnostics.csv':csv_text(list(evidence['E20']['diagnostics'][0]),evidence['E20']['diagnostics']),
        'docs/benchmarks/data/kalman-errors.csv':csv_text(list(kalman_cells[0]),kalman_cells),
        'docs/benchmarks/data/kalman-replay.csv':csv_text(list(traces[0]),traces),
        **{f'docs/benchmarks/data/{stem}-{suffix}.csv':csv_text(union_fields(rows),rows) for eid,stem in [('E56','sandbox-cpp-diagnostic'),('E57','sandbox-cpp-tasks')] for suffix,rows in [('samples',evidence[eid]['rows']),('pairs',evidence[eid]['pairs']),('summary',cpp_flat_summary(evidence[eid]['summary'])),('paired-summary',cpp_flat_summary(evidence[eid]['paired_summary']))]},
        'docs/benchmarks/data/sandbox-cpp-aborted.csv':csv_text(union_fields([dict(trial=a['trial'],**r) for a in evidence['E57']['aborted_runs'] for r in a['rows']]),[dict(trial=a['trial'],**r) for a in evidence['E57']['aborted_runs'] for r in a['rows']]),
        'docs/benchmarks/data/sandbox-cpp-e55-audit.csv':csv_text(union_fields(evidence['E56']['historical_e55_pair_audit']),evidence['E56']['historical_e55_pair_audit']),
        **{f'docs/benchmarks/data/{stem}-{suffix}.csv':csv_text(union_fields(rows),rows) for eid,stem in [('E54','sandbox-cc-diagnostic'),('E55','sandbox-cc-tasks')] for suffix,rows in [('samples',evidence[eid]['rows']),('pairs',evidence[eid]['pairs']),('summary',cc_flat_summary(evidence[eid]['summary'])),('paired-summary',cc_flat_summary(evidence[eid]['paired_summary'],True))]},
        'docs/benchmarks/data/sandbox-cc-ipc.csv':csv_text(union_fields(evidence['E54']['ipc_rows']),evidence['E54']['ipc_rows']),
        'docs/benchmarks/data/test-sandbox-host-samples.csv':csv_text(sorted(set().union(*(r.keys() for r in evidence['E53']['rows']))),evidence['E53']['rows']),
        'docs/benchmarks/data/test-sandbox-host-pairs.csv':csv_text(sorted(set().union(*(r.keys() for r in evidence['E53']['pairs']))),evidence['E53']['pairs']),
        'docs/benchmarks/data/test-sandbox-host-summary.csv':csv_text(['kind','case','files','pairs','on_slower','metric','median_ms','min_ms','max_ms','off_wall_median_ms','on_wall_median_ms'],[dict(**{k:r[k] for k in ['kind','case','files','pairs','on_slower','off_wall_median_ms','on_wall_median_ms']},metric=k,median_ms=v['median'],min_ms=v['min'],max_ms=v['max']) for r in evidence['E53']['summary'] for k,v in r.items() if isinstance(v,dict)]),
        'docs/benchmarks/data/test-sandbox-samples.csv':csv_text(sorted(set().union(*(r.keys() for r in evidence['E52']['rows']))),evidence['E52']['rows']),
        'docs/benchmarks/data/test-sandbox-pairs.csv':csv_text(sorted(set().union(*(r.keys() for r in evidence['E52']['pairs']))),evidence['E52']['pairs']),
        'docs/benchmarks/data/test-sandbox-summary.csv':csv_text(['kind','case','files','pairs','on_slower','metric','median_ms','min_ms','max_ms','off_wall_median_ms','on_wall_median_ms'],[dict(**{k:r[k] for k in ['kind','case','files','pairs','on_slower','off_wall_median_ms','on_wall_median_ms']},metric=k,median_ms=v['median'],min_ms=v['min'],max_ms=v['max']) for r in evidence['E52']['summary'] for k,v in r.items() if isinstance(v,dict)]),
        'docs/benchmarks/README.md': report(evidence,catalog),
        'docs/benchmarks/README.zh-CN.md': report(evidence,catalog,True),
        'docs/benchmarks/data/catalog.json': dump({'schema_version':1,'snapshot':'2026-11-02','experiments':catalog}),
        'docs/benchmarks/data/raw-records.jsonl': ''.join(json.dumps(r,ensure_ascii=False,separators=(',',':'))+'\n' for r in raw),
        'docs/benchmarks/data/metrics.csv': csv_text(['experiment','dimensions','metric','unit','n','median','min','max','source'],metrics),
        'docs/benchmarks/data/validation.csv': csv_text(['category','case','result','evidence','detail'],validation),
        'docs/benchmarks/data/cost-shape-errors.csv': csv_text(['experiment','batch_tokens','prefix_tokens','new_tokens','mode','samples','wall_median_ms','wall_min_ms','wall_max_ms','wall_mad_ms','mae_ms','signed_bias_ms','max_abs_error_ms','wape_percent'],shape_cells),
        'docs/benchmarks/data/frozen-calibration-errors.csv': csv_text(['experiment','batch_tokens','prefix_tokens','new_tokens','mode','samples','wall_median_ms','wall_min_ms','wall_max_ms','wall_mad_ms','mae_ms','signed_bias_ms','max_abs_error_ms','wape_percent'],frozen_cells),
        'docs/benchmarks/data/frozen-calibration-anchors.csv': csv_text(['batch_tokens','prefix_tokens','new_tokens','stage','samples','median_ms','min_ms','max_ms'],evidence['E19']['anchor_summary']),
        'docs/benchmarks/data/frozen-calibration-training.csv': csv_text(['batch_tokens','coverage','mode','samples','mae_ms','signed_bias_ms','max_abs_error_ms','wape_percent'],frozen_training(evidence['E19'])),
        'docs/benchmarks/data/integrity.json': dump({'raw_records':len(raw),'evidence_files':len(catalog),
            'performance_record_files':sum(bool(c['raw_records']) for c in catalog),
            'metal_auxiliary_records':sum(len(d.get('metal_commands',[])) for d in evidence.values()),
            'metal_command_records':sum(r['kind']=='metal_command' for d in evidence.values() for r in d.get('metal_commands',[])),
            'metal_batch_records':sum(r['kind']=='metal_batch' for d in evidence.values() for r in d.get('metal_commands',[])),
            'metal_wait_records':sum(r['kind']=='metal_wait' for d in evidence.values() for r in d.get('metal_commands',[])),
            'summary_scalar_comparisons':checks,'all_archived_summary_values_match_raw':True,
            'stream_token_and_timestamp_invariants':True,'stream_paired_summaries_match_raw':True,
            'metric_summary_rows':len(metrics),'validation_rows':len(validation),
            'inference_rerun_during_consolidation':False}),
    }
    inv=ROOT/'docs/benchmarks/data/run-inventory.json'
    files['docs/benchmarks/data/run-inventory.json'] = dump(inventory(evidence)) if refresh_inventory or not inv.exists() else inv.read_text()
    return files


def allowlist():
    files = [ROOT/'README.md', ROOT/'README.zh-CN.md', ROOT/'docs/RELEASING.md', ROOT/'docs/basic-agent.md', ROOT/'docs/adaptation-status.md',
             ROOT/'tools/build_benchmark_report.py', ROOT/'tools/render_benchmark_figures.py', ROOT/'tools/render_sandbox_benchmark.py', ROOT/'tools/render_sandbox_final.py', ROOT/'tools/bench_test_sandbox.py', ROOT/'tools/bench_sandbox_cc.py', ROOT/'tools/bench_sandbox_cc_tasks.py', ROOT/'tools/probe_sandbox_cc.c', ROOT/'tools/probe_cc_phase.c', ROOT/'tools/probe_cc_ipc.c', ROOT/'tools/render_sandbox_cc.py', ROOT/'tools/bench_sandbox_cpp.py', ROOT/'tools/bench_sandbox_cpp_tasks.py', ROOT/'tools/build_sandbox_group_candidate.py', ROOT/'tools/probe_sandbox_cpp.c', ROOT/'tools/probe_cpp_cache.c', ROOT/'tools/probe_cpp_compiler.c', ROOT/'tools/probe_sandbox_patterns.c', ROOT/'tools/run_cpp_batch.sh', ROOT/'tools/render_sandbox_cpp.py',ROOT/'tools/bench_cost_estimator.py',ROOT/'tools/bench_roofline_estimator.py',ROOT/'tools/bench_roofline_diagnose.py',ROOT/'tools/bench_cost_shapes.py',ROOT/'tools/bench_frozen_calibration.py',ROOT/'tools/bench_kalman_cost.py',ROOT/'tools/bench_tool_prepare.py',ROOT/'tools/bench_restore_timing.py',ROOT/'tools/bench_restore_balance.py',ROOT/'tools/bench_tool_taskcost.py',ROOT/'tools/bench_tool_isolate.py',ROOT/'tools/bench_tool_launch.py',ROOT/'tools/bench_resident.py',ROOT/'tools/bench_tool_output.py',ROOT/'tools/analyze_output_isolation.py',ROOT/'tools/analyze_output_reap.py',ROOT/'tools/analyze_output_contention.py',ROOT/'tools/analyze_decode_schedule.py',ROOT/'tools/analyze_metal_timeline.py',ROOT/'tools/bench_batch_capacity.py',ROOT/'tools/bench_device_contention.py',ROOT/'tools/analyze_batch_trace.py',ROOT/'tools/probe_schedule_clock.c',ROOT/'tools/probe_model_idle.py',ROOT/'tools/analyze_model_idle.py',ROOT/'tools/probe_output_exit.c',ROOT/'tools/test_tool_prepare_cli.py',ROOT/'tools/analyze_frozen_calibration.py']
    files += [ROOT/'LICENSE'] if (ROOT/'LICENSE').exists() else []
    files += [ROOT/'docs/self-speculation-handoff.md',ROOT/'tools/run_self_speculate.py',ROOT/'tools/test_self_speculate.py']
    files += sorted((ROOT/'docs/evidence').glob('*.json'))
    files += sorted((ROOT/'docs/design').glob('*.md'))
    files += sorted(p for p in OUT.rglob('*') if p.is_file() and p.name!='publication-manifest.json')
    return sorted(set(files))


def privacy_and_links(files):
    checks = [r'/Users/',r'/home/',r'[A-Za-z]:\\Users\\',
              r'https?://(?:127\.|10\.|192\.168\.|172\.(?:1[6-9]|2[0-9]|3[01])\.)',
              r'\bsk-[A-Za-z0-9_-]{20,}',r'\bgh[pousr]_[A-Za-z0-9]{20,}',
              r'Bearer\s+[A-Za-z0-9_.-]{16,}']
    failures=[]
    allowed={p.resolve() for p in files}
    allowed.add((OUT/'publication-manifest.json').resolve())
    def heading_ids(path):
        seen=collections.Counter()
        identifiers=set(re.findall(r'<a id="([^"]+)"></a>',path.read_text()))
        for heading in re.findall(r'^#{1,6}\s+(.+?)\s*#*$',path.read_text(),re.MULTILINE):
            heading=re.sub(r'<[^>]+>','',heading).lower()
            slug=re.sub(r'[^\w\- ]','',heading).replace(' ','-')
            suffix=seen[slug];seen[slug]+=1
            identifiers.add(slug+('-'+str(suffix) if suffix else ''))
        return identifiers
    for path in files:
        if path.suffix == '.png':
            continue
        text=path.read_text()
        # The scanner's own literal patterns are not credential contents.
        if path.name != 'build_benchmark_report.py':
            for pattern in checks:
                if re.search(pattern,text): failures.append((str(path.relative_to(ROOT)),'privacy pattern'))
        if path.suffix=='.md':
            for target in re.findall(r'!?\[[^\]]*\]\(([^)]+)\)',text):
                if re.match(r'https?://',target): continue
                target,separator,fragment=target.partition('#')
                destination=(path.parent/unquote(target)).resolve() if target else path.resolve()
                if destination not in allowed:
                    failures.append((str(path.relative_to(ROOT)), 'missing/unexported link '+target))
                elif fragment and destination.suffix=='.md' and unquote(fragment) not in heading_ids(destination):
                    failures.append((str(path.relative_to(ROOT)), 'missing heading '+target+'#'+fragment))
    assert not failures, failures  # Report filenames/categories, never matching secret values.


def check_figures(evidence):
    provenance=json.loads((OUT/'figures/provenance.json').read_text())
    assert provenance['renderer_source_sha256']==sha(ROOT/'tools/render_benchmark_figures.py'), 'stale figure renderer'
    for name,digest in provenance['evidence_sha256'].items():
        assert sha(ROOT/name)==digest, 'stale figure evidence: '+name
    for record in provenance['input_statistics']:
        assert stats(evidence,record['experiment'],record['metric'],**record['filters'])==record['statistics'], 'stale figure statistics'
    current=json.loads((OUT/'figures/sandbox-provenance.json').read_text())
    assert current['renderer_source_sha256']==sha(ROOT/'tools/render_sandbox_benchmark.py')
    assert current['evidence_sha256']==sha(ROOT/'docs/evidence/test-sandbox-20261101.json')
    assert current['summary']==evidence['E52']['summary']
    for name,digest in current['figures'].items():assert sha(OUT/'figures'/name)==digest
    final=json.loads((OUT/'figures/sandbox-final-provenance.json').read_text())
    assert final['renderer_source_sha256']==sha(ROOT/'tools/render_sandbox_final.py')
    assert final['base_renderer_sha256']==sha(ROOT/'tools/render_sandbox_benchmark.py')
    assert final['evidence_sha256']==sha(ROOT/'docs/evidence/test-sandbox-host-20261101.json')
    assert final['summary']==evidence['E53']['summary']
    for name,digest in final['figures'].items():assert sha(OUT/'figures'/name)==digest
    cc=json.loads((OUT/'figures/sandbox-cc-provenance.json').read_text())
    assert cc['renderer_source_sha256']==sha(ROOT/'tools/render_sandbox_cc.py')
    for name,digest in cc['evidence_sha256'].items():assert sha(ROOT/'docs/evidence'/name)==digest
    assert cc['diagnostic_summary']==evidence['E54']['summary']
    assert cc['task_paired_summary']==evidence['E55']['paired_summary']
    for name,digest in cc['figures'].items():assert sha(OUT/'figures'/name)==digest
    cpp=json.loads((OUT/'figures/sandbox-cpp-provenance.json').read_text())
    assert cpp['renderer_source_sha256']==sha(ROOT/'tools/render_sandbox_cpp.py')
    for name,digest in cpp['evidence_sha256'].items():assert sha(ROOT/'docs/evidence'/name)==digest
    assert cpp['diagnostic_summary']==evidence['E56']['summary'] and cpp['paired_summary']==evidence['E57']['paired_summary']
    for name,digest in cpp['figures'].items():assert sha(OUT/'figures'/name)==digest


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--check',action='store_true')
    ap.add_argument('--refresh-inventory',action='store_true',help='Refresh safe metadata of local run directories')
    ap.add_argument('--export',type=Path,help='Write deterministic allowlisted ZIP after validation')
    args=ap.parse_args()
    evidence=load_evidence()
    check_figures(evidence)
    generated=outputs(evidence,args.refresh_inventory)
    if args.check:
        for name,content in generated.items():
            assert (ROOT/name).read_text()==content, 'stale generated report: '+name
    else:
        for name,content in generated.items():
            path=ROOT/name; path.parent.mkdir(parents=True,exist_ok=True); path.write_text(content)
    files=allowlist()
    privacy_and_links(files)
    manifest={'schema_version':1,'snapshot':'2026-11-02',
              'scope':'Benchmark data release only; not a whole-repository credential audit or inference rerun',
              'files':[{'path':str(p.relative_to(ROOT)),'bytes':p.stat().st_size,'sha256':sha(p)} for p in files]}
    manifest_path=OUT/'publication-manifest.json'
    if args.check:
        assert json.loads(manifest_path.read_text())==manifest,'stale publication manifest'
    else: manifest_path.write_text(dump(manifest))
    if args.export:
        args.export.parent.mkdir(parents=True,exist_ok=True)
        with zipfile.ZipFile(args.export,'w',compression=zipfile.ZIP_DEFLATED,compresslevel=9) as archive:
            for path in sorted(files+[manifest_path]):
                item=zipfile.ZipInfo(str(path.relative_to(ROOT)),date_time=(2026,10,31,0,0,0))
                item.compress_type=zipfile.ZIP_DEFLATED
                item.external_attr=0o644 << 16
                archive.writestr(item,path.read_bytes())
        print('Exported',args.export.name,'sha256='+sha(args.export))
    print(f'PASS: {len(SPECS)} evidence files, {sum(len(d.get("rows",[])) for d in evidence.values())} raw records; summary/invariant, link, manifest and scoped privacy checks; {len(files)+1} release files')


if __name__=='__main__':
    main()
