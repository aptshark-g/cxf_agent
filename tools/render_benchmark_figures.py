#!/usr/bin/env python3
"""Render benchmark charts with ReportLab; macOS sips creates the PNG previews.

No inference, network or credentials. Read only the archived evidence. Charts show
medians and observed min/max, never confidence intervals. SVG is the vector master.
"""
import math
import json
from pathlib import Path
import subprocess
import tempfile

from reportlab.graphics.charts.barcharts import VerticalBarChart
from reportlab.graphics.shapes import Drawing, Line, Rect, String
from reportlab.graphics import renderPDF, renderSVG
from reportlab.lib.colors import HexColor
from build_benchmark_report import ROOT, OUT, load_evidence, stats, sha
import reportlab

COLORS = ['#64748b', '#087f8c', '#d97706']
INK = HexColor('#152638')
MUTED = HexColor('#4c6073')


def label(d, x, y, text, size=15, color=INK, anchor='start', bold=False):
    d.add(String(x,y,text,fontSize=size,fillColor=color,textAnchor=anchor,
                 fontName='Helvetica-Bold' if bold else 'Helvetica'))


def canvas(title, subtitle, height=1050, date="2026-10-30"):
    d=Drawing(1200,height)
    d.add(Rect(0,0,1200,height,fillColor=HexColor('#ffffff'),strokeColor=None))
    label(d,42,height-46,title,26,bold=True)
    label(d,42,height-76,subtitle,15,color=MUTED)
    label(d,42,25,'cxf x llama.cpp | Apple M5 / Metal | Qwen3 0.6B Q8_0 | '+date,12,color=MUTED)
    return d


def legend(d, y, names):
    for i,name in enumerate(names):
        x=54+i*365
        d.add(Rect(x,y-2,15,15,fillColor=HexColor(COLORS[i]),strokeColor=None))
        label(d,x+24,y,name,14)


def panel(d, y, title, categories, series, unit, height=310, width=1058, x=84, ymax=None, digits=1):
    # ReportLab computes all scales/bar positions; overlays add range whiskers.
    c=VerticalBarChart()
    c.x=x;c.y=y;c.width=width;c.height=height
    c.data=[[s['median'] for s in values] for values in series]
    c.categoryAxis.categoryNames=categories
    c.categoryAxis.labels.fontName='Helvetica'
    c.categoryAxis.labels.fontSize=14
    c.categoryAxis.labels.dy=-10
    c.categoryAxis.strokeColor=HexColor('#8a9aad')
    top=max(s['max'] for values in series for s in values)
    if ymax is None:
        rough=top*1.24
        magnitude=10**math.floor(math.log10(rough))
        ymax=math.ceil(rough/magnitude*2)/2*magnitude
    c.valueAxis.valueMin=0;c.valueAxis.valueMax=ymax;c.valueAxis.valueStep=ymax/4
    c.valueAxis.labels.fontName='Helvetica';c.valueAxis.labels.fontSize=12
    c.valueAxis.labels.dx=-7
    c.valueAxis.strokeColor=None
    c.valueAxis.visibleGrid=True
    c.valueAxis.gridStrokeColor=HexColor('#e4eaf0')
    c.groupSpacing=20;c.barSpacing=3
    for i in range(len(series)):
        c.bars[i].fillColor=HexColor(COLORS[i]);c.bars[i].strokeColor=None
    c.draw()  # Populate positions using the chart library's actual layout.
    d.add(c)
    label(d,x-28,y+height+48,title,19,bold=True)
    label(d,x-28,y+height+22,unit,13,color=MUTED)
    for i,values in enumerate(series):
        for j,s in enumerate(values):
            bx,by,bw,bh=c._barPositions[i][j]
            center=bx+bw/2
            low=y+s['min']/ymax*height;high=y+s['max']/ymax*height
            d.add(Line(center,low,center,high,strokeColor=INK,strokeWidth=1.15))
            for end in [low,high]:d.add(Line(center-5,end,center+5,end,strokeColor=INK,strokeWidth=1.15))
            label(d,center,high+9,f"{s['median']:.{digits}f}",12,anchor='middle',bold=True)


def save(d,name):
    dest=OUT/'figures';dest.mkdir(parents=True,exist_ok=True)
    renderSVG.drawToFile(d,str(dest/(name+'.svg')))
    with tempfile.TemporaryDirectory(prefix='cxf-figure-') as tmp:
        pdf=Path(tmp)/'figure.pdf'
        renderPDF.drawToFile(d,str(pdf))
        subprocess.run(['sips','-s','format','png',str(pdf),'--out',str(dest/(name+'.png'))],
                       check=True,capture_output=True)
    print('Rendered',name+'.svg / .png')


def main():
    e=load_evidence()
    inputs=[]
    def measured(eid,field,**filters):
        value=stats(e,eid,field,**filters)
        inputs.append({'experiment':eid,'metric':field,'filters':filters,'statistics':value})
        return value
    modes=['wait_all','stream_coalesced','stream_eager']
    cases=[('text_prefill',20),('text_prefill',100),('complete_file_test',20),('complete_file_test',100)]
    categories=['Text / 20 ms','Text / 100 ms','C file / 20 ms','C file / 100 ms']
    d=canvas('Preparation can overlap input arrival',
             'E11 | controlled semantic-unit replay | n=7 per mode | median labels, whiskers = observed min/max')
    legend(d,927,['Wait for complete input','Stream / coalesce 128 tokens','Stream / eager'])
    for y,title,field in [(550,'A. Total source-start to model-ready time','total_ready_ms'),
                           (125,'B. Remaining wait after the final input payload','post_input_ready_ms')]:
        series=[[measured('E11',field,kind='timing',scenario=s,gap_ms=g,mode=mode) for s,g in cases] for mode in modes]
        panel(d,y,title,categories,series,'Milliseconds (lower is better)')
    label(d,55,66,'Same final event boundaries and input-token work. Model load / initial context excluded. No live cloud or Windows/CUDA.',13,color=MUTED)
    save(d,'stream-latency')

    d=canvas('Frontend improvements have different memory and time effects',
             'Independent within-experiment comparisons | median labels, whiskers = observed min/max',1040)
    legend(d,917,['Matched baseline','Optimized path'])
    series=[]
    for mode in ['normalized_json','jinja_values']:
        values=[]
        for shape,size in [('single_text',14336),('tool_turns',128)]:
            s=measured('E09','heap_extra_bytes',kind='memory',shape=shape,size=size,mode=mode)
            values.append({k:v/1024 if k!='n' else v for k,v in s.items()})
        series.append(values)
    panel(d,545,'A. E09 sampled temporary heap (n=3)',
          ['14,504 logical tokens','128 tool records'],series,'KiB above pre-call heap; model / KV excluded',height=300)
    series=[[measured('E06','front_ms',prefix_lines=p,mode=mode) for p in [16,96]]
            for mode in ['native_checkpoint','native_prepare_cache']]
    panel(d,130,'B. E06 template preparation cache, six appends (n=5)',
          ['345-token prefix','1,225-token prefix'],series,'Frontend milliseconds; different experiment from panel A',height=300)
    label(d,55,65,'Temporary heap savings are about 370 KiB for the long-text fixture; whole RSS and preallocated KV did not shrink.',13,color=MUTED)
    save(d,'frontend-memory')

    d=canvas('Less tail wait can require more local execution',
             'E11 | text / 100 ms source gap | same 701 input tokens per mode | n=7 | observed min/max',1000)
    categories=['Wait all','Coalesce 128','Eager']
    # Each category gets one bar; colors consistently identify the scheduling modes.
    for y,title,field,unit in [(520,'A. Cumulative local input-prefill execution','prefill_ms','Milliseconds (execution time; not FLOPs or energy)'),
                               (130,'B. Remaining wait after input ends','post_input_ready_ms','Milliseconds (lower is better)')]:
        values=[measured('E11',field,kind='timing',scenario='text_prefill',gap_ms=100,mode=m) for m in modes]
        panel(d,y,title,categories,[values],unit,height=285)
    for i,m in enumerate(modes):
        calls=measured('E11','input_decode_calls',kind='timing',scenario='text_prefill',gap_ms=100,mode=m)['median']
        label(d,250+i*355,900,f'{categories[i]}: {calls:g} input decode calls',15,anchor='middle')
    label(d,55,65,'Token work is equal; more small batches increase measured execution time while hiding work under the input window.',13,color=MUTED)
    save(d,'stream-tradeoff')
    d=canvas('Calibration improves warm-prefill duration estimates',
             'E13 | 63 calibration / 84 evaluation samples | labels = mean absolute error; whiskers = observed error min/max')
    legend(d,927,['Fixed token/s (same calibration)','Frozen work model','Online correction'])
    def error_values(mode,filters):
        key=mode+'_abs_error_ms';s=measured('E13',key,scenario='evaluation',**filters)
        values=[r[key] for r in e['E13']['rows'] if r.get('scenario')=='evaluation' and all(r.get(k)==v for k,v in filters.items())]
        return dict(s,median=sum(values)/len(values))
    series=[[error_values(mode,f) for f in [{},{'frozen_extrapolated':False},{'frozen_extrapolated':True}]] for mode in ['fixed','frozen','online']]
    panel(d,550,'A. Prediction error by frozen calibration domain',
          ['All evaluation (84)','Inside range (42)','Outside range (42)'],series,'Mean absolute error in ms (lower is better)')
    series=[[error_values(mode,{'prefix_tokens':p}) for p in [0,1024,4096]] for mode in ['fixed','frozen','online']]
    panel(d,125,'B. Prediction error by prefix length',
          ['No cached prefix (28)','1,024 prefix tokens (28)','4,096 prefix tokens (28)'],series,'Mean absolute error in ms (lower is better)')
    label(d,55,66,'Same measured jobs for every predictor. Warm service time only; no scheduler change or inference-speedup claim.',13,color=MUTED)
    save(d,'cost-estimation')
    d=canvas('Roofline needs workload calibration on this backend',
             'E14 | 63 calibration / 84 evaluation samples | mean absolute error labels; observed min/max whiskers',1130)
    def roof_error(mode,filters):
        key=mode+'_abs_error_ms';s=measured('E14',key,scenario='evaluation',**filters)
        values=[r[key] for r in e['E14']['rows'] if r.get('scenario')=='evaluation' and all(r.get(k)==v for k,v in filters.items())]
        return dict(s,median=sum(values)/len(values))
    filters=[{},{'frozen_extrapolated':False},{'frozen_extrapolated':True}]
    categories=['All evaluation (84)','Inside range (42)','Outside range (42)']
    legend(d,1008,['Fixed token/s','Frozen empirical','Raw Roofline'])
    series=[[roof_error(mode,f) for f in filters] for mode in ['fixed','frozen','roofline']]
    panel(d,615,'A. Uncalibrated formula: negative result retained',categories,series,
          'Mean absolute error in ms (lower is better)',height=285)
    legend(d,535,['Frozen empirical','Calibrated Roofline','Online empirical'])
    series=[[roof_error(mode,f) for f in filters] for mode in ['frozen','roofline_calibrated','online']]
    panel(d,140,'B. Calibrated comparison (different vertical scale)',categories,series,
          'Mean absolute error in ms (lower is better)',height=285)
    label(d,55,78,'Same jobs within E14. Frozen fits use the same 63 labels; online learns prior evaluation rows.',13,color=MUTED)
    label(d,55,56,'Small single-run calibrated difference is not a stable advantage or an inference-speedup result.',13,color=MUTED)
    save(d,'roofline-estimation')
    d=canvas('Most raw error came from mismatched compute-rate probes',
             'E15 | same 84 measured jobs | mean absolute error labels; whiskers = observed min/max',1130)
    def diagnosis_error(mode,filters):
        key=mode+'_abs_error_ms';s=measured('E15',key,scenario='evaluation',**filters)
        values=[r[key] for r in e['E15']['rows'] if r.get('scenario')=='evaluation' and all(r.get(k)==v for k,v in filters.items())]
        return dict(s,median=sum(values)/len(values))
    filters=[{'prefix_tokens':p} for p in [0,1024,4096]]
    categories=['No cached prefix (28)','1,024 prefix tokens (28)','4,096 prefix tokens (28)']
    legend(d,1008,['Legacy raw','FFN count only','Rate correction only'])
    series=[[diagnosis_error(mode,f) for f in filters] for mode in ['legacy','math_only','rate_only']]
    panel(d,615,'A. Correct rate probes remove most raw estimation error',categories,series,
          'Mean absolute error in ms (lower is better)',height=285)
    legend(d,535,['Corrected raw','Corrected + calibration','Original online'])
    series=[[diagnosis_error(mode,f) for f in filters] for mode in ['corrected','corrected_calibrated','online']]
    panel(d,140,'B. Remaining error (different vertical scale)',categories,series,
          'Mean absolute error in ms (lower is better)',height=285)
    label(d,55,78,'Only the estimator changed. Correct FFN counts slightly worsen rate-only raw MAE: compensating errors remain.',13,color=MUTED)
    label(d,55,56,'63 calibration labels for frozen fits; online learns prior evaluation rows. No inference-speedup claim.',13,color=MUTED)
    save(d,'roofline-diagnosis')
    d=canvas('Small-batch errors follow a real kernel boundary',
             'E16 | cap512 | n=7 per prefix/width cell | the later-superseded interpolation result is retained',1130)
    legend(d,1008,['19 query tokens (vec path)','20 query tokens (dense path)'])
    series=[[measured('E16','wall_ms',scenario='evaluation',batch_tokens=512,prefix_tokens=p,new_tokens=n) for p in [0,1024,4096]] for n in [19,20]]
    panel(d,615,'A. More tokens can take less time across the 19/20 boundary',
          ['No cached prefix','1,024 prefix tokens','4,096 prefix tokens'],series,
          'Actual milliseconds; median labels, observed min/max',height=285)
    def shape_error(eid,mode,filters):
        key=mode+'_abs_error_ms';s=measured(eid,key,scenario='evaluation',**filters)
        values=[r[key] for r in e[eid]['rows'] if r.get('scenario')=='evaluation' and all(r.get(k)==v for k,v in filters.items())]
        return dict(s,median=sum(values)/len(values))
    legend(d,535,['Empirical online','Single-shape Roofline online','Shape online (old interpolation)'])
    series=[[shape_error('E16',mode,{'batch_tokens':512,'new_tokens':n}) for n in [8,16,19,20,64,256,768]] for mode in ['empirical_online','roof_online','shape_online']]
    panel(d,140,'B. E16 improves narrow widths but regresses at 64 tokens',
          ['8','16','19','20','64','256','768'],series,'MAE in ms; 21 samples per width across three prefixes',height=285)
    label(d,55,78,'Boundary labels follow pinned Metal source; independent FA probes show the same discontinuity.',13,color=MUTED)
    label(d,55,56,'The 64-token regression motivated time interpolation. This negative result was not overwritten.',13,color=MUTED)
    save(d,'cost-shape-boundary')
    d=canvas('Time interpolation plus online correction on new shapes',
             'E17 | 147 new-shape evaluation jobs per cap | labels = MAE; whiskers = observed error min/max',1130)
    legend(d,1008,['Empirical online','Single-shape Roofline online','Shape online (time interpolation)'])
    series=[[shape_error('E17',mode,{'batch_tokens':b}) for b in [512,128]] for mode in ['empirical_online','roof_online','shape_online']]
    panel(d,615,'A. Fair online comparison: same past labels and 64-slot windows',
          ['Batch limit 512','Batch limit 128'],series,'Mean absolute error in ms (lower is better)',height=285)
    legend(d,535,['Single-shape raw Roofline','Shape raw (time interpolation)'])
    series=[[shape_error('E17',mode,{'batch_tokens':b}) for b in [512,128]] for mode in ['roof_raw','shape_raw']]
    panel(d,140,'B. Uncalibrated estimates (different vertical scale)',
          ['Batch limit 512','Batch limit 128'],series,'Mean absolute error in ms (lower is better)',height=285)
    label(d,55,78,'Shape table: 960 bytes; one calibration: 3,120 bytes. Extra profiling cost is reported separately.',13,color=MUTED)
    label(d,55,56,'Frozen shape calibration still regresses in some comparisons. One machine/model; no inference speedup.',13,color=MUTED)
    save(d,'cost-shape-v2')
    d=canvas('Frozen regression has more than a sample-coverage problem',
             'E19 | unseen caps256/64 and context/token grid | 147 shared jobs per cap | MAE labels, observed min/max',1130)
    legend(d,1008,['Legacy / clipped / sparse','Legacy / extended / sparse','Legacy / extended / diverse'])
    series=[[shape_error('E19',mode,{'batch_tokens':b}) for b in [256,64]] for mode in ['legacy_clipped_sparse','legacy_sparse','legacy_diverse']]
    panel(d,615,'A. Table coverage and equal-label training coverage',
          ['Batch limit 256','Batch limit 64'],series,'Mean absolute error in ms (lower is better)',height=285)
    legend(d,535,['Raw shape prior','Residual / sparse','Residual / diverse'])
    series=[[shape_error('E19',mode,{'batch_tokens':b}) for b in [256,64]] for mode in ['raw','residual_sparse','residual_diverse']]
    panel(d,140,'B. New three-parameter residual model still loses to raw at cap64',
          ['Batch limit 256','Batch limit 64'],series,'Mean absolute error in ms (lower is better)',height=285)
    label(d,55,78,'Each frozen fit uses 63 labels; sparse and diverse samples interleave. No evaluation labels enter these fits.',13,color=MUTED)
    label(d,55,56,'Clipped-table fix regresses at cap256. E18 incomplete setup run and all negative results are retained.',13,color=MUTED)
    save(d,'frozen-calibration')
    d=canvas('A residual filter can improve or worsen the same predictor',
             'E20 | fresh timings + C prequential replay | n=147 per base/cap | MAE labels; observed error min/max',1130,date='2026-10-31')
    legend(d,1008,['Unfiltered base','EWMA = fixed Kalman','Task-scaled R Kalman'])
    def kalman_error(mode,batch,base_id):
        key=mode+'_abs_error_ms'
        value=measured('E20',key,kind='filter_prediction',batch_tokens=batch,base_id=base_id)
        values=[r[key] for r in e['E20']['filter_rows'] if r['kind']=='filter_prediction' and r['batch_tokens']==batch and r['base_id']==base_id]
        return dict(value,median=sum(values)/len(values))
    for y,batch,title in [(615,256,'A. Batch limit 256'),(140,64,'B. Batch limit 64')]:
        series=[[kalman_error(mode,batch,i) for i in range(3)] for mode in ['base','fixed','scaled']]
        panel(d,y,title,['Raw shape prior','Frozen residual / diverse','Shape online'],series,'Mean absolute error in ms (lower is better)',height=285)
    label(d,55,78,'Noise parameters use calibration labels only. Each prediction precedes its own observed label; zero initial bias.',13,color=MUTED)
    label(d,55,56,'Numeric replay is not extra inference work or a live-agent speedup. Gaussian intervals are diagnostic only.',13,color=MUTED)
    save(d,'kalman-cost')
    d=canvas('Real tools: exposed wait and complete task cost differ',
             'E21 | actual file reads / compiler-test processes | three calls per task | n=7 | median and observed min/max',1550,date='2026-10-31')
    cases=[('read',128),('read',2048),('compile_test',128),('compile_test',2048)]
    categories=['Read / short history','Read / long history','Compile / short history','Compile / long history']
    def task_values(mode,field):
        return [measured('E21',field,kind='task_timing',phase='evaluation',scenario=scenario,words=words,mode=mode) for scenario,words in cases]
    for y,ly,title,prefix in [(1030,1428,'A. Recompute KV: wait remaining after tools','prefill'),(590,988,'B. Restore saved KV: wait remaining after tools','restore')]:
        legend(d,ly,['Start after tool','Start immediately','EWMA-timed start'])
        series=[task_values(prefix+'_'+policy,'post_tool_sum_ms') for policy in ['late','immediate','timed']]
        panel(d,y,title,categories,series,'Sum of three after-tool first-token waits, ms (lower is better)',height=260)
    legend(d,548,['Keep KV resident','Recompute / EWMA timed','Save + restore / EWMA timed'])
    series=[task_values(mode,'task_ms') for mode in ['resident','prefill_timed','restore_timed']]
    panel(d,150,'C. Complete task cost charges checkpoint, saving, tools and suffix work',categories,series,'Task ms; initial history / model load / oracle validation excluded',height=260)
    label(d,55,78,'Nonresident states are explicit capacity-policy tests. The ordinary resident-KV baseline remains visible.',13,color=MUTED)
    label(d,55,56,'No artificial sleep window; warm OS file cache. Scripted actions isolate scheduling from model decision quality.',13,color=MUTED)
    save(d,'tool-prepare')
    d=canvas('Process overhead and the waiting / holding tradeoff',
             'E22: 21 calls per cell | E23: 7 three-call tasks per cell | observed ranges; separate measured fixtures',1550,date='2026-10-31')
    legend(d,1428,['Before native restore','Native restore + sync','Worker queue'])
    series=[]
    for metric in ['before_native_ms','native_ms','queue_ms']:
        values=[]
        for words in [128,2048]:
            value=measured('E22','stage:'+metric,kind='timing',phase='evaluation',scenario='compile_test',words=words,mode='restore_immediate')
            mean=next(r[metric]['mean'] for r in e['E22']['stage_summary'] if r['scenario']=='compile_test' and r['words']==words and r['mode']=='restore_immediate')
            values.append(dict(value,median=mean))
        series.append(values)
    panel(d,1030,'A. Fork overlap: missing delay occurs before native restore (E22)',
          ['128 history words','2,048 history words'],series,'Per-call mean ms; bars show means, whiskers observed min/max',height=260)
    cases=[('fork',320),('fork',1792),('spawn',320),('spawn',1792)]
    categories=['Fork / short','Fork / long','Spawn / short','Spawn / long']
    for y,ly,title,field,unit in [(590,988,'B. After-tool waiting on new compiler-test fixtures (E23)','post_tool_sum_ms','Sum of three first-token waits, ms; median labels'),
                                 (150,548,'C. Ready-prefix early holding (not physical memory release)','early_hold_sum_ms','Sum of three PREFIX_READY idle intervals, ms; median labels')]:
        legend(d,ly,['Immediate','Original timed','Parent-progress ETA'])
        series=[[measured('E23',field,kind='task_timing',phase='evaluation',scenario='compile_test',words=words,mode='restore_'+backend+'_'+policy) for backend,words in cases] for policy in ['immediate','timed','progress']]
        panel(d,y,title,categories,series,unit,height=260)
    label(d,55,78,'Changes to launch method and ETA are separate controls. Timing actions are scripted; real tools execute.',13,color=MUTED)
    label(d,55,56,'Fixed EWMA and 5 ms margin; no evaluation tuning. Both-lower pair counts and negative results are in the report.',13,color=MUTED)
    save(d,'restore-balance')
    d=canvas('Complete task cost and optional early snapshot cleanup',
             'E24 | new affine-repair / rotate-xor tasks | 7 tasks and 21 calls per cell | observed min/max',1550,date='2026-10-31')
    cases=[512,2304]
    legend(d,1428,['Timed + event','Progress + return drop','Progress + early drop'])
    modes=['restore_timed','restore_progress','restore_progress_early']
    series=[[{k:(v if k=='n' else v*1000) for k,v in measured('E24','stage:callback_ms',kind='timing',phase='evaluation',scenario='compile_test',words=w,mode=m).items()} for w in cases] for m in modes]
    panel(d,1030,'A. Direct parent progress callback duration (wall clock)',
          ['512 history words','2,304 history words'],series,'Per-call microseconds; medians; indirect interference is excluded',height=260)
    legend(d,988,['Timed + event','Progress + return drop','Progress + early drop'])
    series=[[measured('E24','stage:exposed_drop_ms',kind='timing',phase='evaluation',scenario='compile_test',words=w,mode=m) for w in cases] for m in modes]
    panel(d,590,'B. Snapshot deletion time exposed after the tool returns',
          ['512 history words','2,304 history words'],series,'Per-call ms; overlap is bounded by the remaining tool window',height=260)
    legend(d,548,['Timed + event','Progress + return drop','Progress + early drop'])
    series=[[measured('E24','task_ms',kind='task_timing',phase='evaluation',scenario='compile_test',words=w,mode=m) for w in cases] for m in modes]
    panel(d,150,'C. Complete three-call task: saving, tools, repair, suffix and cleanup included',
          ['512 history words','2,304 history words'],series,'Task ms; pairwise differences and all five controls are in the report',height=260)
    label(d,55,78,'Stage medians cannot be added to explain a task median. Direct callback time does not measure all system interference.',13,color=MUTED)
    label(d,55,56,'Original negative E23 pairs remain immutable. Frozen alpha / 5 ms margin; default timed / return-drop unchanged.',13,color=MUTED)
    save(d,'tool-taskcost')
    d=canvas('Isolating tool variability from snapshot deletion',
             'E25 | 10 three-call tasks per mode | 30 child-phase observations | forward / reverse Latin order',1550,date='2026-10-31')
    modes=['resident','restore_late','restore_progress','restore_progress_early','restore_progress_hold']
    categories=['Resident','Restore late','Progress / return','Progress / early','Unlink / hold fd']
    for y,ly,title,phase in [(1030,1428,'A. Real compiler phase','compile'),(590,988,'B. Check launch through process reaping','run')]:
        legend(d,ly,['Wall time','Accumulated child CPU'])
        series=[[measured('E25','stage:'+phase+'_'+f,kind='timing',phase='evaluation',mode=m) for m in modes] for f in ['wall_ms','cpu_ms']]
        panel(d,y,title,categories,series,'Per-call ms; medians and observed min/max; CPU includes waited descendants',height=260)
    legend(d,548,['Early deletion / unlink','Return deletion / last close'])
    modes=['restore_progress','restore_progress_early','restore_progress_hold']
    series=[[measured('E25','stage:'+f,kind='timing',phase='evaluation',mode=m) for m in modes] for f in ['early_drop_ms','drop_ms']]
    panel(d,150,'C. Separating unlink from last-reference release',
          ['Return cleanup','Full early cleanup','Early unlink / last close'],series,'Per-call ms; holding fd retains disk budget until final close',height=260)
    label(d,55,78,'The large non-CPU tool interval also exists with resident KV. This trace does not name an OS security mechanism.',13,color=MUTED)
    label(d,55,56,'Global VM counters include other processes; zero IO operations do not exclude cache or filesystem CPU work.',13,color=MUTED)
    save(d,'tool-isolate')
    d=canvas('New executable launch delay and incremental build control',
             'E26 | new 768-word / 12M workload | 7 three-call tasks per mode | both check executions charged',1550,date='2026-10-31')
    modes=['resident','restore_progress','restore_progress_early','restore_progress_hold','resident_incremental']
    categories=['Resident / always','Progress / return','Progress / early','Unlink / hold fd','Resident / incremental']
    legend(d,1428,['First run pre-main','Same binary rerun pre-main'])
    series=[[measured('E26','stage:'+p+'_pre_main_ms',kind='timing',phase='evaluation',mode=m) for m in modes] for p in ['run','rerun']]
    panel(d,1030,'A. Executable start to entry into main (launch is contained here)',categories,series,
          'Per-call ms; incremental group mixes 2 new builds with 1 reused artifact per task',height=260)
    legend(d,988,['First run measured compute','Same binary rerun compute'])
    series=[[measured('E26','stage:'+p+'_compute_ms',kind='timing',phase='evaluation',mode=m) for m in modes] for p in ['run','rerun']]
    panel(d,590,'B. Time spent inside the actual rotate / xor calculation',categories,series,'Per-call ms; same inputs and matching exit status on both executions',height=260)
    legend(d,548,['Complete task'])
    series=[[measured('E26','task_ms',kind='task_timing',phase='evaluation',mode=m) for m in modes]]
    panel(d,150,'C. Complete task includes actual tests, saving, repair and final suffix work',categories,series,
          'Task ms; incremental control skips only unchanged compilation; tests still execute',height=260)
    label(d,55,78,'First-artifact versus repeated execution is measured; the responsible kernel / security subsystem is not sampled.',13,color=MUTED)
    label(d,55,56,'Incremental recipe is an explicit deployment benchmark, not automatic caching or rewriting of user tool commands.',13,color=MUTED)
    save(d,'tool-launch')
    d=canvas('Normal resident KV: six actual tool rounds',
             'E27 | read / fail / write / pass / read / pass | 7 tasks per mode | no save, eviction or restore',1150,date='2026-10-31')
    modes=['resident_direct','resident_wrapper'];categories=['512 history words','3,072 history words']
    legend(d,1028,['Direct original tools','Resident coordinator wrapper'])
    series=[[measured('E27','task_ms',kind='task_timing',words=w,mode=m) for w in [512,3072]] for m in modes]
    panel(d,650,'A. Complete six-call task includes actual receipts and final suffix work',categories,series,'Task ms; independent post-token oracle work excluded and recorded',height=260)
    legend(d,588,['Direct original tools','Resident coordinator wrapper'])
    series=[[measured('E27','evaluated_tokens',kind='task_timing',words=w,mode=m) for w in [512,3072]] for m in modes]
    panel(d,210,'B. Both modes retain existing KV and evaluate the same new suffix',categories,series,'Tokens evaluated across all six calls; no forced full-context recomputation',height=260)
    label(d,55,138,'Stable history is already ready; absent new input, wrapping the tool creates no additional prefill to hide.',13,color=MUTED)
    label(d,55,112,'Wrapper task pairs have mixed signs. The paired median is not the difference between group medians.',13,color=MUTED)
    label(d,55,86,'Identical prebuilt validator and fixed command. Initial history and recorded fixture first-execution warmup excluded.',13,color=MUTED)
    save(d,'resident-flow')
    d=canvas('Resident streaming: actual process output, no sleep gaps',
             'E28 | 12 immutable units | 7 tasks per cell | same helper / content / work; no snapshots or build changes',1550,date='2026-10-31')
    cases=[(80,0),(80,40000000),(240,0),(240,40000000)]
    categories=['80 words / burst','80 words / CPU-paced','240 words / burst','240 words / CPU-paced']
    modes=['wait_all','stream_coalesced','stream_eager']
    for y,ly,title,field,unit in [(1030,1428,'A. Complete input task including normal Stream close','task_complete_ms','Task ms; common post-token oracle work subtracted, fresh reference after normal close'),
                                (590,988,'B. Wait after actual input ends to the first greedy token','post_input_first_ms','After-input ms; lower waiting alone is not sufficient for a total-task gain'),
                                (150,548,'C. Cumulative local prefill execution cost','prefill_ms','Execution ms; extra small-batch work and source interference remain charged')]:
        legend(d,ly,['Wait for all input','Coalesced streaming','Eager streaming'])
        series=[[measured('E28',field,kind='timing',words=w,producer_iterations=i,mode=m) for w,i in cases] for m in modes]
        panel(d,y,title,categories,series,unit,height=260)
    label(d,55,78,'CPU-paced units are emitted after real 40M-iteration work; no synthetic sleep window or EOF idle interval.',13,color=MUTED)
    label(d,55,56,'Complete semantic messages from a local process, not vendor cloud SSE. Burst negatives and producer slowdown retained.',13,color=MUTED)
    save(d,'resident-stream')
    d=canvas('Actual agentctl tool-output prefill with resident KV',
             'E29 | three real test.run calls per task | 7 off/on pairs per cell | identical command and final tokens',1550,date='2026-10-31')
    cases=[(32,0),(32,40000000),(56,0),(56,40000000)]
    categories=['32 words / burst','32 words / CPU-paced','56 words / burst','56 words / CPU-paced']
    for y,ly,title,field,unit in [(1030,1428,'A. Complete CLI task, including actual tools and all four Native preparations','task_ms','Task ms; exact post-token oracle work excluded and raw wall retained'),
                                 (590,988,'B. Sum of tool-end to next first-token waits, including stream join','post_tool_sum_ms','Per-task ms; lower waiting alone is not enough for a whole-task gain'),
                                 (150,548,'C. Cumulative stream plus final-request prefill execution','prefill_sum_ms','Per-task execution ms; increased small-batch work and tool interference remain charged')]:
        legend(d,ly,['Prefill off','Prefill on'])
        series=[[measured('E29',field,kind='task_timing',phase='evaluation',words=w,producer_iterations=i,mode=m) for w,i in cases] for m in ['off','on']]
        panel(d,y,title,categories,series,unit,height=260)
    label(d,55,78,'Scripted model choices, actual CLI/registry/AgentStore/tool routing. One resident model process; no snapshots or eviction.',13,color=MUTED)
    label(d,55,56,'8 warmup + 56 evaluation tasks; all pairs retained. No task-time compilation, build-command optimization or cloud SSE.',13,color=MUTED)
    save(d,'tool-output-cli')
    d=canvas('Adaptive output gate with resident KV',
             'E30 | three real test.run calls per task | 7 off/on pairs per cell | identical command and final tokens',1550,date='2026-10-31')
    cases=[(40,0),(40,30000000),(48,0),(48,30000000)]
    categories=['40 words / burst','40 words / CPU-paced','48 words / burst','48 words / CPU-paced']
    for y,ly,title,field,unit in [(1030,1428,'A. Complete CLI task, including actual tools and all four Native preparations','task_ms','Task ms; exact post-token oracle work excluded and raw wall retained'),
                                 (590,988,'B. Sum of tool-end to next first-token waits, including stream join','post_tool_sum_ms','Per-task ms; lower waiting alone is not enough for a whole-task gain'),
                                 (150,548,'C. Cumulative stream plus final-request prefill execution','prefill_sum_ms','Per-task execution ms; increased small-batch work and tool interference remain charged')]:
        legend(d,ly,['Prefill off','Prefill on'])
        series=[[measured('E30',field,kind='task_timing',phase='evaluation',words=w,producer_iterations=i,mode=m) for w,i in cases] for m in ['off','on']]
        panel(d,y,title,categories,series,unit,height=260)
    label(d,55,78,'Scripted model choices, actual CLI/registry/AgentStore/tool routing. One resident model process; no snapshots or eviction.',13,color=MUTED)
    label(d,55,56,'8 warmup + 56 evaluation tasks; all pairs retained. No task-time compilation, build-command optimization or cloud SSE.',13,color=MUTED)
    save(d,'tool-output-adaptive')
    d=canvas('Matched output strategy and component controls',
             'E31 | three real test.run calls per task | 7 matched repeats per mode | identical command and final tokens',1550,date='2026-10-31')
    cases=[(40,0),(40,30000000),(48,0),(48,30000000)]
    categories=['40 words / burst','40 words / CPU-paced','48 words / burst','48 words / CPU-paced']
    for y,ly,title,field,unit in [(1030,1428,'A. Complete CLI task, including actual tools and all four Native preparations','task_ms','Task ms; exact post-token oracle work excluded and raw wall retained'),
                                 (590,988,'B. Sum of tool-end to next first-token waits, including stream join','post_tool_sum_ms','Per-task ms; lower waiting alone is not enough for a whole-task gain'),
                                 (150,548,'C. Cumulative stream plus final-request prefill execution','prefill_sum_ms','Per-task execution ms; increased small-batch work and tool interference remain charged')]:
        legend(d,ly,['Off','Old E29','Timer E30'])
        series=[[measured('E31',field,kind='task_timing',phase='evaluation',words=w,producer_iterations=i,mode=m) for w,i in cases] for m in ['off','old','on']]
        panel(d,y,title,categories,series,unit,height=260)
    label(d,55,78,'Scripted model choices, actual CLI/registry/AgentStore/tool routing. One resident model process; no snapshots or eviction.',13,color=MUTED)
    label(d,55,56,'Warmups excluded; 7 evaluated tasks per mode/case; all pairs retained. No task-time compilation, build-command optimization or cloud SSE.',13,color=MUTED)
    save(d,'tool-output-isolation')
    d=canvas('Output-growth gate on new matched tasks',
             'E32 | three real test.run calls per task | 7 matched repeats per mode | identical command and final tokens',1550,date='2026-10-31')
    cases=[(36,0),(36,25000000),(52,0),(52,25000000)]
    categories=['36 words / burst','36 words / CPU-paced','52 words / burst','52 words / CPU-paced']
    for y,ly,title,field,unit in [(1030,1428,'A. Complete CLI task, including actual tools and all four Native preparations','task_ms','Task ms; exact post-token oracle work excluded and raw wall retained'),
                                 (590,988,'B. Sum of tool-end to next first-token waits, including stream join','post_tool_sum_ms','Per-task ms; lower waiting alone is not enough for a whole-task gain'),
                                 (150,548,'C. Cumulative stream plus final-request prefill execution','prefill_sum_ms','Per-task execution ms; increased small-batch work and tool interference remain charged')]:
        legend(d,ly,['Off','Timer E30','Output growth'])
        series=[[measured('E32',field,kind='task_timing',phase='evaluation',words=w,producer_iterations=i,mode=m) for w,i in cases] for m in ['off','on','progress']]
        panel(d,y,title,categories,series,unit,height=260)
    label(d,55,78,'Scripted model choices, actual CLI/registry/AgentStore/tool routing. One resident model process; no snapshots or eviction.',13,color=MUTED)
    label(d,55,56,'Warmups excluded; 7 evaluated tasks per mode/case; all pairs retained. No task-time compilation, build-command optimization or cloud SSE.',13,color=MUTED)
    save(d,'tool-output-progress')
    d=canvas('Process reaping and worker attribution',
             'E33 | 21 real tool calls per mode/case | median and observed range | event mode not yet applied',1100,date='2026-10-31')
    cases=[(36,0),(36,25000000),(52,0),(52,25000000)]
    categories=['36 / burst','36 / CPU-paced','52 / burst','52 / CPU-paced']
    for y,ly,title,field in [(590,988,'A. EOF wait: existing 10 ms polling','eof_poll_ms'),(150,548,'B. Worker queue wait (excludes actual tokenizer / prefill work)','owner_queue_ms')]:
        legend(d,ly,['Off','Output growth','Thread-only'])
        series=[[measured('E33',field,kind='tool',phase='evaluation',words=w,producer_iterations=i,mode=m) for w,i in cases] for m in ['off','progress','thread']]
        panel(d,y,title,categories,series,'Milliseconds per tool; min/max are not confidence intervals',height=260,digits=3 if field=='owner_queue_ms' else 1)
    label(d,55,78,'Producer wall / CPU and matched task-stage deltas are available in the process-stage CSV; separate medians are not additive.',13,color=MUTED)
    label(d,55,56,'Timing observations do not identify CPU frequency, memory bandwidth or OS scheduler causality.',13,color=MUTED)
    save(d,'tool-output-reap')
    d=canvas('Exit-event wait on fresh real-tool workloads',
             'E34 | 7 matched tasks per mode/case | all six modes retained in report and CSV',1550,date='2026-10-31')
    cases=[(44,0),(44,20000000),(50,0),(50,20000000)]
    categories=['44 / burst','44 / CPU-paced','50 / burst','50 / CPU-paced']
    for y,ly,title,field in [(1030,1428,'A. Complete task: generic executor and streaming gains separated','task_ms'),(590,988,'B. Tool window: producer and process lifecycle charged','tool_sum_ms'),(150,548,'C. Post-tool wait, including outstanding stream join','post_tool_sum_ms')]:
        legend(d,ly,['Off / poll','Off / event','Growth / event'])
        series=[[measured('E34',field,kind='task_timing',phase='evaluation',words=w,producer_iterations=i,mode=m) for w,i in cases] for m in ['off','off-event','progress-event']]
        panel(d,y,title,categories,series,'Milliseconds per task; identical receipts / canonical tokens / first greedy token',height=260)
    label(d,55,78,'macOS NOTE_EXIT only after EOF; waitpid remains the sole reaper; timeout checks and single execution preserved.',13,color=MUTED)
    label(d,55,56,'One M5 / Qwen 0.6B, scripted actions. Negative pairs retained; no stable tail-latency or Windows claim.',13,color=MUTED)
    save(d,'tool-output-event')
    d=canvas('Exit-in-progress retry: fresh-task validation',
             'E35 | 7 matched tasks per mode/case | original poll/event and corrected retry retained',1550,date='2026-10-31')
    cases=[(46,0),(46,15000000),(54,0),(54,15000000)]
    categories=['46 / burst','46 / CPU-paced','54 / burst','54 / CPU-paced']
    for y,ly,title,field in [(1030,1428,'A. Complete task: same command, resident model and KV','task_ms'),(590,988,'B. Tool window including producer, EOF and process reaping','tool_sum_ms'),(150,548,'C. EOF wait per tool: original event versus corrected exit retry','eof_poll_ms')]:
        legend(d,ly,['Off / corrected','Growth / event','Growth / corrected'])
        series=[[measured('E35',field,kind='tool' if field=='eof_poll_ms' else 'task_timing',phase='evaluation',words=w,producer_iterations=i,mode=m) for w,i in cases] for m in ['off-race','progress-event','progress-race']]
        panel(d,y,title,categories,series,'Milliseconds per tool (21 samples)' if field=='eof_poll_ms' else 'Milliseconds per task (7 samples); median and observed range; negatives retained',height=260)
    label(d,55,78,'Only ESRCH or observed NOTE_EXIT enables bounded 1 ms retry; ordinary long-running tools retain event waiting.',13,color=MUTED)
    label(d,55,56,'No default switch, stable-tail guarantee or cross-experiment subtraction; all five modes appear in the report and CSV.',13,color=MUTED)
    save(d,'tool-output-race')
    d=canvas('Same-workload isolation: tools, CPU and later inference',
             'E36 | 7 matched tasks per case/mode | diagnostic controls share the output-growth gate',1550,date='2026-10-31')
    cases=[(46,0),(46,15000000),(54,0),(54,15000000)]
    categories=['46 / burst','46 / CPU-paced','54 / burst','54 / CPU-paced']
    for y,ly,title,field in [(1030,1428,'A. Later Native prefill wall time (four preparations per task)','final_prefill_ms'),(590,988,'B. Native prepare process CPU (parallel workers included; not GPU time)','final_prepare_cpu_ms'),(150,548,'C. Producer main wall time (three tools per task)','producer_wall_ms')]:
        legend(d,ly,['Off','Dormant consumer','Real prefill'])
        series=[[measured('E36','contention:'+field,words=w,producer_iterations=i,mode=m) for w,i in cases] for m in ['off-race','idle-race','progress-race']]
        panel(d,y,title,categories,series,'Milliseconds per task; medians and observed min/max; all negatives retained',height=260)
    label(d,55,78,'Thread-only and tokenizer-only controls, exact decode/token work, owner CPU and all phase timestamps are retained in CSV.',13,color=MUTED)
    label(d,55,56,'Unchanged work with longer wall localizes a phase; it does not identify a GPU, frequency or scheduler cause.',13,color=MUTED)
    label(d,55,98,'Bars show independent group medians; paired differences in the report may have a different ordering.',13,color=MUTED)
    save(d,'tool-output-contention')
    d=canvas('Lazy consumer launch on new actual CLI tasks',
             'E37 | 7 matched tasks / 21 real tool calls per case/mode | same growth gate and resident KV',1550,date='2026-10-31')
    cases=[(42,0),(42,18000000),(55,0),(55,18000000)]
    categories=['42 / burst','42 / CPU-paced','55 / burst','55 / CPU-paced']
    for y,ly,title,field in [(1030,1428,'A. Complete CLI task, including producer and stream join','task_ms'),(590,988,'B. Post-tool first-token wait, including outstanding work','post_tool_sum_ms'),(150,548,'C. Consumer threads created per tool call','consumer_threads')]:
        legend(d,ly,['Off','Eager growth','Lazy growth'])
        series=[[measured('E37',field,kind='tool' if field=='consumer_threads' else 'task_timing',phase='evaluation',words=w,producer_iterations=i,mode=m) for w,i in cases] for m in ['off-race','progress-race','lazy-race']]
        panel(d,y,title,categories,series,'Threads per tool (21 samples)' if field=='consumer_threads' else 'Milliseconds per task (7 samples); median and observed range',height=260)
    label(d,55,78,'Only first eligible non-final growth launches a consumer; zero eligible burst growth creates no thread. Final oracle is unchanged.',13,color=MUTED)
    label(d,55,56,'Resource reduction does not guarantee lower whole-task latency. Defaults unchanged; no snapshots or eviction.',13,color=MUTED)
    label(d,55,98,'Bars show independent group medians; paired differences in the report may have a different ordering.',13,color=MUTED)
    save(d,'tool-output-lazy')
    d=canvas('Decode and synchronization: same-load attribution',
             'E38 | 7 tasks / 21 producer calls per case/mode | strict synchronization; no eviction',1550,date='2026-10-31')
    cases=[(42,0),(42,18000000),(55,0),(55,18000000)]
    categories=['42 / burst','42 / CPU-paced','55 / burst','55 / CPU-paced']
    for y,ly,title,field in [(1030,1428,'A. Sum of final llama_decode call spans (may include internal waits)','final_decode_ms'),(590,988,'B. Sum of following explicit synchronization waits','final_sync_ms')]:
        legend(d,ly,['Off','Thread-only','Growth'])
        series=[[measured('E38','decode:'+field,words=w,producer_iterations=i,mode=m) for w,i in cases] for m in ['off-race','thread-race','progress-race']]
        panel(d,y,title,categories,series,'Milliseconds per task; wall spans are not GPU kernel times',height=260)
    legend(d,548,['Producer alone','Off / CLI','Growth / CLI'])
    series=[[measured('E38','producer:wall_ms',kind='producer_control' if m=='producer-alone' else 'tool',phase='evaluation',words=w,producer_iterations=i,mode=m) for w,i in cases] for m in ['producer-alone','off-race','progress-race']]
    panel(d,150,'C. Producer main wall time; standalone model process remains idle',categories,series,'Milliseconds per producer; standalone stdout uses /dev/null, CLI uses a pipe',height=260)
    label(d,55,98,'Independent group medians and observed ranges; paired contrasts and negative runnable proxies are in the report / CSV.',13,color=MUTED)
    label(d,55,78,'Mach raw tick units validated against CPU-clock busy/sleep controls. Runnable includes running; counters have update lag.',13,color=MUTED)
    label(d,55,56,'Background CPU-supply delay exists without a consumer; no competing PID or pure GPU-duration attribution.',13,color=MUTED)
    save(d,'decode-schedule')
    d=canvas('Deferred internal sync: measure total work, not shifted waiting',
             'E39 | fresh 43/53-word tasks | 7 matched repeats | every public prefill return remains synchronized',1550,date='2026-10-31')
    cases=[(43,0),(43,16000000),(53,0),(53,16000000)]
    categories=['43 / burst','43 / CPU-paced','53 / burst','53 / CPU-paced']
    for y,ly,title,field in [(1030,1428,'A. Complete task wall time','task_ms'),(590,988,'B. Final decode-call spans: next decode may absorb pending work','final_decode_ms'),(150,548,'C. Final explicit synchronization: a decrease alone is not a speedup','final_sync_ms')]:
        legend(d,ly,['Off / strict','Off / deferred','Growth / deferred'])
        series=[[measured('E39','decode:'+field,words=w,producer_iterations=i,mode=m) for w,i in cases] for m in ['off-race','off-drain','progress-drain']]
        panel(d,y,title,categories,series,'Milliseconds per task; independent medians and observed min/max',height=260)
    label(d,55,98,'Paired task medians, all negative pairs and decode-plus-sync / prefill totals are in the report; do not subtract bar medians.',13,color=MUTED)
    label(d,55,78,'Only internal host barriers deferred; public return, greedy and failure paths drain. Strict remains default.',13,color=MUTED)
    label(d,55,56,'No GPU kernel timestamps, physical KV reduction, snapshots, Windows/CUDA claim or tail-latency guarantee.',13,color=MUTED)
    save(d,'deferred-sync')
    d=canvas('Model CPU source isolation: absent, immediate idle, settled idle',
             'E40/E41 | 21 producer calls per state / shape | disabled-prefill and streaming-prefill initialization',1550,date='2026-10-31')
    cases=[('E40',43),('E40',53),('E41',43),('E41',53)]
    categories=['Off-warm /43','Off-warm /53','Stream-warm /43','Stream-warm /53']
    for y,ly,title,field in [(1030,1428,'A. Producer main wall time (model load / fixture / settle excluded)','wall_ms'),(590,988,'B. Producer process CPU time','cpu_ms'),(150,548,'C. Model process CPU during the whole producer subprocess','model_cpu_ms')]:
        legend(d,ly,['Model absent','Resident / immediate','Resident /100ms idle'])
        series=[[measured(eid,'idle:'+field,words=w,mode=m) for eid,w in cases] for m in ['absent-before','resident-immediate','resident-settled']]
        panel(d,y,title,categories,series,'Milliseconds per producer; independent medians and observed min/max',height=260,digits=3 if field=='model_cpu_ms' else 1)
    label(d,55,98,'Absence-after, raw runnable counters, negative proxies and fixed-order bracket comparisons are in the report / CSV.',13,color=MUTED)
    label(d,55,78,'Model CPU interval includes child spawn/reap; no other process inventory. Low CPU does not exclude all indirect effects.',13,color=MUTED)
    label(d,55,56,'Both diagnostic runs retained, including the first non-reproduction. No runtime optimization or default change.',13,color=MUTED)
    save(d,'model-idle')
    cases=[(43,0),(43,16000000),(53,0),(53,16000000)]
    categories=['43 / burst','43 / CPU-paced','53 / burst','53 / CPU-paced']
    d=canvas('Metal timeline: device work and host return are different',
             'E42 | 7 tasks per case/mode | command intervals unioned; no completion marker',1550,date='2026-10-31')
    for y,ly,title,field in [(1030,1428,'A. Observed GPU command-buffer execution spans','metal_gpu_union_ms'),(590,988,'B. Explicit sync: GPU-end to host-return upper bound','metal_sync_return_lag_ms'),(150,548,'C. Commit-to-start interval outside observed own GPU execution','metal_queue_no_own_gpu_ms')]:
        legend(d,ly,['Off / strict','Growth / strict','Off / deferred'])
        series=[[measured('E42','metal:'+field,words=w,producer_iterations=i,mode=m) for w,i in cases] for m in ['off-race','progress-race','off-drain']]
        panel(d,y,title,categories,series,'Milliseconds per task; median and observed min/max',height=260,digits=2)
    label(d,55,98,'GPU spans include internal sync / memory stalls; uncovered submission time is not exclusive device-queue occupancy.',13,color=MUTED)
    label(d,55,78,'Return lag includes notification / handlers / CPU scheduling. Deferred decode may absorb waits; full CSV keeps both APIs.',13,color=MUTED)
    label(d,55,56,'Probe and bare controls, negative pairs and multi-second host/producer outliers are preserved in the report.',13,color=MUTED)
    save(d,'metal-timeline')
    d=canvas('Completion notification: delivery versus waiting-thread return',
             'E43 | timestamp-only marker is optional | diagnostic upper bounds, not pure CPU wakeup',1550,date='2026-10-31')
    for y,ly,title,field in [(1030,1428,'A. GPU end to completion-marker entry','metal_sync_completion_delivery_ms'),(590,988,'B. Marker exit to waitUntilCompleted return','metal_sync_after_notify_return_ms'),(150,548,'C. GPU execution overlapping explicit synchronization','metal_sync_gpu_overlap_ms')]:
        legend(d,ly,['Off / marker','Growth / marker'])
        series=[[measured('E43','metal:'+field,words=w,producer_iterations=i,mode=m) for w,i in cases] for m in ['off-race-notify','progress-race-notify']]
        panel(d,y,title,categories,series,'Milliseconds per task; median and observed min/max',height=260,digits=2)
    label(d,55,98,'Intervals A/B still mix driver / callback / CPU scheduling. Existing handlers may also contribute. No kernel wakeup trace.',13,color=MUTED)
    label(d,55,78,'Marker, no-marker and bare modes run in the same process/binary with rotated order; observer effects remain visible.',13,color=MUTED)
    label(d,55,56,'All negatives and coverage counts retained. Do not subtract independent bar medians or extrapolate to the old 44ms pair.',13,color=MUTED)
    save(d,'metal-notification')
    cases=[(45,0),(45,14000000),(51,0),(51,14000000)]
    categories=['45 / burst','45 / CPU-paced','51 / burst','51 / CPU-paced']
    d=canvas('Bounded tail coalescing: fresh tasks and equal batch capacity',
             'E44 | allocated batch1024 throughout | cap512 vs <=1024 tail | strict sync, resident KV',1550,date='2026-10-31')
    for y,ly,title,field in [(1030,1428,'A. Complete task wall time, observer disabled','task_ms'),(590,988,'B. Final prefill wall time, observer disabled','final_prefill_ms')]:
        legend(d,ly,['Off / split','Off / coalesced','Growth / coalesced'])
        series=[[measured('E44','metal:'+field,words=w,producer_iterations=i,mode=m) for w,i in cases] for m in ['off-split-bare','off-coalesce-bare','progress-coalesce-bare']]
        panel(d,y,title,categories,series,'Milliseconds per task; median and observed min/max',height=260)
    legend(d,548,['Off / split / probe','Off / coalesced / probe'])
    series=[[measured('E44','metal:metal_gpu_union_ms',words=w,producer_iterations=i,mode=m) for w,i in cases] for m in ['off-split','off-coalesce']]
    panel(d,150,'C. Observed device execution spans in separately profiled controls',categories,series,'Milliseconds per task; command intervals unioned',height=260)
    label(d,55,98,'Streaming budget512 is unchanged; larger tail only fuses if the whole remaining input fits current budget/capacity.',13,color=MUTED)
    label(d,55,78,'Both controls allocate batch1024, not the old512-capacity baseline. Compute-buffer cost is reported; no KV-memory win.',13,color=MUTED)
    label(d,55,56,'Paired contrasts and growth/split controls are in CSV/report, including every regression. Default batch512 unchanged.',13,color=MUTED)
    save(d,'tail-coalescing')
    cases=[(45,0),(45,14000000),(51,0),(51,14000000)]
    categories=['45 / burst','45 / CPU-paced','51 / burst','51 / CPU-paced']
    d=canvas('Sustained tail: repeat with device and notification markers',
             'E45 | 7 tasks per case/mode | old86ms bare tail cannot be retrospectively split',1550,date='2026-10-31')
    for y,ly,title,field in [(1030,1428,'A. GPU execution overlapping final explicit synchronization','final_metal_sync_gpu_overlap_ms'),(590,988,'B. GPU end to notification marker: delivery / callback scheduling','final_metal_sync_completion_delivery_ms'),(150,548,'C. Marker exit to wait return: host coordination / thread restoration','final_metal_sync_after_notify_return_ms')]:
        legend(d,ly,['Split / marker','Coalesced / marker'])
        series=[[measured('E45','metal:'+field,words=w,producer_iterations=i,mode=m) for w,i in cases] for m in ['off-split-notify','off-coalesce-notify']]
        panel(d,y,title,categories,series,'Milliseconds per task; median and observed min/max',height=260,digits=3 if y<1000 else 1)
    label(d,55,98,'Old86ms did not recur. New worst no-marker paced task regresses11.248ms: tool+14.212ms, prefill-3.054ms.',13,color=MUTED)
    label(d,55,78,'Marker and bare controls are separate. Notification/return intervals are upper bounds, not pure kernel CPU wakeup.',13,color=MUTED)
    label(d,55,56,'All pairs, non-reproduction and per-step historical audit retained; no QoS/busy-wait change or default switch.',13,color=MUTED)
    save(d,'sustained-tail')
    cases=[(47,0),(47,12000000),(54,0),(54,12000000)]
    categories=['47 / burst','47 / CPU-paced','54 / burst','54 / CPU-paced']
    d=canvas('Smaller compute reservation: fresh rotating capacity controls',
             'E46 | 7 load-order blocks | 512/split vs640/coalesced vs1024/coalesced | resident KV per load',1550,date='2026-10-31')
    for y,ly,title,field,modes in [(1030,1428,'A. Whole task, output prefill off / observer disabled','task_ms',['off-512-bare','off-640-bare','off-1024-bare']),(590,988,'B. Whole task, growth streaming / observer disabled','task_ms',['growth-512-bare','growth-640-bare','growth-1024-bare']),(150,548,'C. GPU command execution spans, separate notification-marker controls','metal_gpu_union_ms',['off-512-notify','off-640-notify','off-1024-notify'])]:
        legend(d,ly,['512 / split','640 / coalesced','1024 / coalesced'])
        series=[[measured('E46','metal:'+field,words=w,producer_iterations=i,mode=m) for w,i in cases] for m in modes]
        panel(d,y,title,categories,series,'Milliseconds per task; median and observed min/max',height=260)
    label(d,55,98,'Metal compute reservation:298.75 /373.44 /597.50MiB.640 saves224.06MiB vs1024; physical KV stays896MiB.',13,color=MUTED)
    label(d,55,78,'Models reload per capacity/block; load/warm excluded. No free online resize claim. Paired negatives remain in report/CSV.',13,color=MUTED)
    label(d,55,56,'Independent group medians; paired contrasts in report.7 eval+7 warm per case/mode; order rotates; default512 unchanged.',13,color=MUTED)
    save(d,'capacity-control')
    cases=[(47,0),(47,12000000),(54,0),(54,12000000)]
    categories=['47 / burst','47 / CPU-paced','54 / burst','54 / CPU-paced']
    d=canvas('Same capacity and work: tool stalls versus active prefill',
             'E47 | capacity640 / tail coalescing throughout | real CLI, exact token/greedy checks',1550,date='2026-10-31')
    for y,ly,title,field in [(1030,1428,'A. Complete task time, observer disabled','task_ms'),(590,988,'B. Sum of producer main wall time','producer_wall_ms')]:
        legend(d,ly,['Off','Thread-only','Growth prefill'])
        series=[[measured('E47','metal:'+field,words=w,producer_iterations=i,mode=m) for w,i in cases] for m in ['off-coalesce-bare','thread-coalesce-bare','progress-coalesce-bare']]
        panel(d,y,title,categories,series,'Milliseconds per task; independent medians and observed min/max',height=260)
    legend(d,548,['Off / marker','Growth / marker'])
    series=[[measured('E47','metal:metal_gpu_union_ms',words=w,producer_iterations=i,mode=m) for w,i in cases] for m in ['off-coalesce-notify','progress-coalesce-notify']]
    panel(d,150,'C. Total observed GPU execution spans, separately profiled groups',categories,series,'Milliseconds per task; interval unions, not pure shader busy time',height=260)
    label(d,55,98,'Thread-only has no GPU prefill during tools, yet producer wall can stall by1.76s; ready proxies and negatives retained.',13,color=MUTED)
    label(d,55,78,'Token-only and per-batch controls are in report/CSV. Bare and marker are separate; do not subtract their bar medians.',13,color=MUTED)
    label(d,55,56,'No snapshot/eviction; no competing-PID or full-device scheduler attribution. Default unchanged.',13,color=MUTED)
    save(d,'contention640-controls')
    categories=['128 tokens','224 tokens','512 tokens','582 tokens']
    d=canvas('Fixed-shape native inference: memory-path sensitivity',
             'E48 | logical prefix2048 / capacity640 / coalesced | pressure is an internal experimental condition',1550,date='2026-10-31')
    for y,ly,title,field in [(1030,1428,'A. Observed GPU command execution spans','metal_gpu_union_ms'),(590,988,'B. GPU milliseconds per evaluated token','gpu_per_token_ms'),(150,548,'C. Commit interval outside observed own GPU execution','metal_queue_no_own_gpu_ms')]:
        legend(d,ly,['Idle worker','Arithmetic worker','64MiB memory worker'])
        series=[[measured('E48','isolation:'+field,prefix_tokens=2048,new_tokens=n,pressure=m,policy='coalesce') for n in [128,224,512,582]] for m in ['idle','arithmetic','memory']]
        panel(d,y,title,categories,series,'Milliseconds/token' if field=='gpu_per_token_ms' else 'Milliseconds per increment; median and observed min/max',height=260,digits=3 if y<1000 else 1)
    label(d,55,98,'Setup/join outside timer; same64MiB allocation in all conditions. Logical bytes are not physical DRAM counters.',13,color=MUTED)
    label(d,55,78,'Shared root retains2048 cells;768 logical-prefix results in CSV are not a production physical-layout clone.',13,color=MUTED)
    label(d,55,56,'GPU spans include stalls/preemption. No dispatch-level timestamps, measured clock or exact bandwidth attribution.',13,color=MUTED)
    save(d,'device-isolation')
    cases=[(49,0),(49,10000000),(55,0),(55,10000000)]
    categories=['49 / burst','49 / CPU-paced','55 / burst','55 / CPU-paced']
    d=canvas('Larger output quantum: fresh complete-task validation',
             'E49 | capacity640 | publication512 vs1536 bytes | budget512 / min128 / resident KV',1550,date='2026-10-31')
    for y,ly,title,field in [(1030,1428,'A. Complete task time, observer disabled','task_ms'),(590,988,'B. Sum of producer main wall time, observer disabled','producer_wall_ms')]:
        legend(d,ly,['Off','Growth /512B','Growth /1536B'])
        series=[[measured('E49','metal:'+field,words=w,producer_iterations=i,mode=m) for w,i in cases] for m in ['off-coalesce-bare','progress-coalesce-bare','progress-coarse-bare']]
        panel(d,y,title,categories,series,'Milliseconds per task; independent medians and observed min/max',height=260)
    legend(d,548,['Off / marker','Growth512 / marker','Growth1536 / marker'])
    series=[[measured('E49','metal:stream_metal_gpu_union_ms',words=w,producer_iterations=i,mode=m) for w,i in cases] for m in ['off-coalesce-notify','progress-coalesce-notify','progress-coarse-notify']]
    panel(d,150,'C. GPU execution during tool windows, separately profiled groups',categories,series,'Milliseconds per task; reduced spans alone do not prove task speedup',height=260)
    label(d,55,98,'1536-byte gate frozen before new evaluation. Later start can lose overlap; all negative task/tool/device pairs retained.',13,color=MUTED)
    label(d,55,78,'Bare results determine end-to-end effects. Per-batch token sizes / counts and marker bounds are in CSV.',13,color=MUTED)
    label(d,55,56,'No added pressure in CLI validation; default512 capacity / prefill off unchanged. Paired results in report.',13,color=MUTED)
    save(d,'coarse640-validation')
    provenance={'renderer':'ReportLab '+reportlab.Version,'png_renderer':'macOS sips / PDF rasterization',
                'renderer_source_sha256':sha(Path(__file__)),
                'evidence_sha256':{str(p.relative_to(ROOT)):sha(p) for p in [ROOT/'docs/evidence'/name for name in
                    ['llama-template-cache-20261030.json','jinja-values-20261030.json','stream-pipeline-20261030.json','cost-estimator-20261030.json','cost-shapes-v2-20261030.json','roofline-estimator-20261030.json','roofline-diagnosis-20261030.json','cost-shapes-20261030.json','frozen-calibration-20261030.json','kalman-cost-20261031.json','tool-prepare-20261031.json','restore-timing-20261031.json','restore-balance-20261031.json','tool-taskcost-20261031.json','tool-isolate-20261031.json','tool-launch-20261031.json','resident-flow-20261031.json','resident-stream-20261031.json','tool-output-cli-20261031.json','tool-output-adaptive-20261031.json','tool-output-isolation-20261031.json','tool-output-progress-20261031.json','tool-output-reap-20261031.json','tool-output-event-20261031.json','tool-output-race-20261031.json','tool-output-contention-20261031.json','tool-output-lazy-20261031.json','tool-output-decode-20261031.json','tool-output-drain-20261031.json','model-idle-20261031.json','model-idle-stream-20261031.json','tool-output-metal-20261031.json','tool-output-notify-20261031.json','tool-output-coalesce-20261031.json','tool-output-tailwait-20261031.json','tool-output-capacity-20261031.json','tool-output-contention640-20261031.json','device-isolation-20261031.json','tool-output-coarse640-20261031.json']]},
                'input_statistics':inputs}
    (OUT/'figures/provenance.json').write_text(json.dumps(provenance,indent=2)+'\n')


if __name__=='__main__':main()
