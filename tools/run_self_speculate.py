#!/usr/bin/env python3
"""D-SPEC native acceptance or off/on experiment; local files only, no server/key.

All raw rows, failed tasks and negative pairs are retained in a fresh directory.
Outputs are local/private until manually reviewed; never auto-publish them.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import platform
import statistics
import subprocess

ROOT=Path(__file__).resolve().parents[1]


def digest(path):
    h=hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda:stream.read(1024*1024),b''):h.update(block)
    return h.hexdigest()


def summarize(rows):
    tasks=[r for r in rows if r['kind']=='model_task' and not r['warmup']]
    on=[r for r in tasks if r['mode']=='on'];official=[r for r in on if r['main_ok']]
    predictions=[r for r in on if r['prediction_valid']]
    pairs=[]
    for key in sorted({(r['case'],r['block']) for r in tasks}):
        members=[r for r in tasks if (r['case'],r['block'])==key]
        group={r['mode']:r for r in members}
        if len(members)!=2 or set(group)!={'off','on'}:raise ValueError('incomplete/duplicate off/on pair')
        a,b=group['off'],group['on']
        equal_main=a.get('main_sha256') is not None and a.get('main_sha256')==b.get('main_sha256')
        equal_final=a.get('final_sha256') is not None and a.get('final_sha256')==b.get('final_sha256')
        equal_tool=a.get('tool_sha256') is not None and a.get('tool_sha256')==b.get('tool_sha256')
        pairs.append(dict(case=key[0],block=key[1],saving_ms=a['total_ms']-b['total_ms'],
                          off_ms=a['total_ms'],on_ms=b['total_ms'],main_equal=equal_main,tool_equal=equal_tool,final_equal=equal_final,
                          both_complete=a['task_ok'] and b['task_ok'],comparable=a['task_ok'] and b['task_ok'] and equal_main and equal_tool and equal_final))
    rates={
        'prediction_parse_per_on_task':[sum(r['prediction_valid'] for r in on),len(on)],
        'tool_name_hit_per_official_call':[sum(r['tool_name_hit'] for r in official),len(official)],
        'exact_hit_per_official_call':[sum(r['exact_hit'] for r in official),len(official)],
        'exact_hit_per_prediction':[sum(r['exact_hit'] for r in predictions),len(predictions)],
        'adopt_per_on_task':[sum(r['adopted'] for r in on),len(on)],
        'off_completed':[sum(r['task_ok'] for r in tasks if r['mode']=='off'),sum(r['mode']=='off' for r in tasks)],
        'on_completed':[sum(r['task_ok'] for r in on),len(on)],
    }
    def cell(values):
        return {'n':len(values),'median':statistics.median(values),'min':min(values),'max':max(values)} if values else {'n':0}
    return {'rates_count_denominator':rates,'all_task_paired_saving_ms':cell([r['saving_ms'] for r in pairs]),
            'complete_equal_task_paired_saving_ms':cell([r['saving_ms'] for r in pairs if r['comparable']]),
            'negative_pairs':[r for r in pairs if r['saving_ms']<0],
            'inconsistent_or_failed_pairs':[r for r in pairs if not r['comparable']],
            'on_probe_ms':cell([r.get('probe_ms',0) for r in on]),
            'limits':['Unscored semantic answer quality; completion means EOG and final DONE.',
                      'Single owner pauses main decode while probe runs; all costs charged.',
                      'All-task timings include failed tasks; speed claims require complete, equal outputs.',
                      'Short fs.read can cost less than probe overhead; do not discard negative pairs.']},pairs


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    for name in ('binary','model','runtime','out'):ap.add_argument('--'+name,type=Path,required=True)
    ap.add_argument('--check',action='store_true',help='short real-KV/template acceptance only; no quality/performance claim')
    ap.add_argument('--tasks',type=Path,help='JSON array of {id,prompt,expected_args?}; no execution commands')
    ap.add_argument('--root',type=Path,help='workspace root for custom tasks')
    ap.add_argument('--repeats',type=int,default=7);ap.add_argument('--warmup',type=int,default=1)
    ap.add_argument('--main-tokens',type=int,default=1024);ap.add_argument('--probe-tokens',type=int,default=128)
    ap.add_argument('--final-tokens',type=int,default=256);ap.add_argument('--fork-after',type=int,default=1)
    ap.add_argument('--context-tokens',type=int,default=8192);ap.add_argument('--threads',type=int,default=4)
    ap.add_argument('--gpu-layers',type=int,default=99);ap.add_argument('--timeout',type=int,default=3600)
    args=ap.parse_args()
    if platform.system() not in ('Darwin','Linux'):ap.error('use macOS/Linux or WSL2; native Windows is not accepted')
    if not 1<=args.repeats<=30 or not 0<=args.warmup<=5:ap.error('repeats 1..30, warmup 0..5')
    if not 1<=args.fork_after<args.main_tokens<=8192 or not 1<=args.probe_tokens<=512 or not 1<=args.final_tokens<=2048:ap.error('invalid token budgets')
    if not 2048<=args.context_tokens<=131072 or not 1<=args.threads<=128 or not 0<=args.gpu_layers<=999 or not 1<=args.timeout<=86400:ap.error('invalid capacity/thread/GPU/timeout limits')
    if bool(args.tasks)!=bool(args.root):ap.error('--tasks and --root must be supplied together')
    if args.check and args.tasks:ap.error('--check uses its own synthetic fixtures')
    if args.out.exists():ap.error('choose a fresh output directory; previous evidence is never overwritten')
    args.out.mkdir(parents=True)
    if args.tasks:
        tasks=json.loads(args.tasks.read_text());root=args.root.resolve()
    else:
        root=(args.out/'workspace').resolve();root.mkdir()
        texts={'alpha.txt':'alpha = 17\n','beta.txt':'beta = 29\n','notes.txt':'A local read-only fixture.\n'*120}
        for name,text in texts.items():(root/name).write_text(text)
        tasks=[{'id':name.removesuffix('.txt'),'prompt':f'Read {name} with fs.read, then report its first line.','expected_args':name} for name in texts]
    if not isinstance(tasks,list) or not 1<=len(tasks)<=100 or len({t['id'] for t in tasks})!=len(tasks):ap.error('tasks must have 1..100 unique IDs')
    for t in tasks:
        if not isinstance(t.get('id'),str) or not isinstance(t.get('prompt'),str) or len(t['prompt'])>16384:ap.error('invalid task')
    config={k:getattr(args,k) for k in ('repeats','warmup','main_tokens','probe_tokens','final_tokens','fork_after','context_tokens','threads','gpu_layers')}
    config.update(acceptance=args.check,root=str(root),tasks=tasks)
    cfg=args.out/'run-config.json';cfg.write_text(json.dumps(config,indent=2)+'\n')
    source_files=['tests/agent_speculate.cpp','tests/agent_speculate.h','tests/self_speculate_native.cpp','tests/agent_native.cpp','tests/agent_native.h','tests/agent_basic.c','tests/agent_basic.h','src/llama.c','tools/run_self_speculate.py']
    build_manifest=args.binary.resolve().parent/'manifest.json'
    compiled=json.loads(build_manifest.read_text()) if build_manifest.exists() else {}
    metadata={'platform':{'system':platform.system(),'release':platform.release(),'machine':platform.machine()},
              'model_sha256':digest(args.model),'binary_sha256':digest(args.binary),'build_manifest_sha256':digest(build_manifest) if build_manifest.exists() else None,
              'llama_commit':compiled.get('llama_commit'),'compiled_source_sha256':compiled.get('sources_sha256',{}),
              'working_source_sha256':{name:digest(ROOT/name) for name in source_files},
              'runner_sanitized_binary':compiled.get('sanitizers'),
              'scope':'real-KV functional acceptance' if args.check else 'local model off/on trial, all outcomes retained'}
    if compiled.get('sanitizers') and not args.check:ap.error('sanitized binary is for correctness, never performance')
    (args.out/'metadata.json').write_text(json.dumps(metadata,indent=2)+'\n')
    command=[str(args.binary.resolve()),str(args.model.resolve()),str(args.runtime.resolve()),str(cfg.resolve())]
    with (args.out/'rows.jsonl').open('w') as stdout,(args.out/'engine.log').open('w') as stderr:
        try:completed=subprocess.run(command,stdout=stdout,stderr=stderr,timeout=args.timeout)
        except subprocess.TimeoutExpired:
            (args.out/'failure.json').write_text(json.dumps({'reason':'whole-process timeout','timeout_s':args.timeout})+'\n');raise SystemExit('timeout; all partial rows/logs retained')
    if completed.returncode:
        (args.out/'failure.json').write_text(json.dumps({'returncode':completed.returncode})+'\n');raise SystemExit('native run failed; inspect retained engine.log')
    rows=[json.loads(line) for line in (args.out/'rows.jsonl').read_text().splitlines()]
    if args.check:
        assert len([r for r in rows if r['kind']=='correctness' and r['passed']])==8
        print('real KV/template acceptance: 8 passed (two scripted flows); no model-quality or speed measurement');return
    assert len([r for r in rows if r['kind']=='model_task'])==2*len(tasks)*(args.repeats+args.warmup)
    for row in rows:
        if row['kind']=='model_task':
            for key,value in row.items():
                if key.endswith('_rc') and type(value) is not int:raise ValueError('result codes must be numeric, never boolean')
    summary,pairs=summarize(rows)
    (args.out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    with (args.out/'pairs.csv').open('w') as stream:
        writer=csv.DictWriter(stream,fieldnames=list(pairs[0]));writer.writeheader();writer.writerows(pairs)
    print(json.dumps(summary,indent=2))


if __name__=='__main__':main()
