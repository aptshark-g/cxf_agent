#!/usr/bin/env python3
"""Render E56/E57 only; retain all historical evidence and figures."""
import hashlib,json,math,subprocess,tempfile
from pathlib import Path
import reportlab
from reportlab.graphics.shapes import Drawing,Rect,Line,String
from reportlab.graphics import renderPDF,renderSVG
from reportlab.lib.colors import HexColor
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'docs/benchmarks/figures'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def text(d,x,y,s,size=13,color='#334155',anchor='start'):
    d.add(String(x,y,s,fontName='Helvetica',fontSize=size,fillColor=HexColor(color),textAnchor=anchor))
def panel(d,bottom,title,labels,series,low,high,colors,height=210):
    left,right=260,1100;width=right-left;x=lambda v:left+(v-low)/(high-low)*width
    text(d,48,bottom+height+61,title,20,'#0f172a');text(d,48,bottom+height+36,'Milliseconds; median and observed min/max, not confidence intervals',12)
    for i in range(6):
        value=low+(high-low)*i/5;xx=x(value);d.add(Line(xx,bottom,xx,bottom+height,strokeColor=HexColor('#e2e8f0')));text(d,xx,bottom-20,f'{value:.0f}',11,anchor='middle')
    d.add(Line(x(0),bottom,x(0),bottom+height,strokeColor=HexColor('#64748b')))
    for i,label in enumerate(labels):
        yy=bottom+height-(i+.5)*height/len(labels);text(d,left-14,yy-4,label,13,anchor='end')
        for j,values in enumerate(series):
            value=values[i];y=yy+(len(series)-1)*8-j*16;color=colors[j];a,b=x(0),x(value['median'])
            d.add(Rect(min(a,b),y-6,max(1,abs(b-a)),12,fillColor=HexColor(color),strokeColor=None));d.add(Line(x(value['min']),y,x(value['max']),y,strokeColor=HexColor('#334155')))
            for edge in ['min','max']:d.add(Line(x(value[edge]),y-4,x(value[edge]),y+4,strokeColor=HexColor('#334155')))
            text(d,right+12,y-4,f"{value['median']:.1f}",12,color)
def main():
    names=['sandbox-cpp-diagnostic-20261102.json','sandbox-cpp-tasks-20261102.json'];a,b=[json.loads((ROOT/'docs/evidence'/n).read_text()) for n in names]
    def diag(case,mode):return next(r for r in a['summary'] if (r['case'],r['mode'])==(case,mode))['sys_ms']
    def paired(case,label):return next(r for r in b['paired_summary'] if (r['case'],r['comparison'])==(case,label))['wall_delta_ms']
    d=Drawing(1200,1480);d.add(Rect(0,0,1200,1480,fillColor=HexColor('#ffffff'),strokeColor=None))
    text(d,48,1437,'Fixed-target C++: header access, cold caches and new tasks',26,'#0f172a')
    text(d,48,1409,'Apple M5 | explicit macOS26.0 target | same compiler/SDK | default security and logging retained',13)
    text(d,48,1380,'E55 residual was target-confounded (bound off26.0 / on10.4); old evidence and plots remain unchanged.',13,'#9a3412')
    cpu=[[diag(c,m) for c in ['preprocess','object','preprocessed']] for m in ['off','on']]
    high=math.ceil(max(v['max'] for group in cpu for v in group)/50)*50
    panel(d,1060,'A. E56 sys CPU: textual headers versus preprocessed input',['Preprocess headers','Compile to object','Preprocessed input'],cpu,0,high,['#0e7490','#d97706'],190)
    text(d,268,1020,'Blue: off. Orange: on. Ordinary compilation produced zero PCM files.',12)
    cases=['single_map','single_regex','batch3','batch5'];labels=['Single map (1 TU)','Single regex (1 TU)','Batch (3 TUs)','Batch (5 TUs)']
    panel(d,675,'B. E57 complete-CLI PCH saving, including cold generation',labels,[[paired(c,'pch_saving') for c in cases]],-250,1800,['#0f766e'],210)
    text(d,268,633,'Positive is faster. Single-file regressions are retained; 3/5-TU tasks improve7/7.',12)
    panel(d,290,'C. Experimental grouped rules: complete-CLI saving versus default',labels,[[paired(c,'group_saving') for c in cases]],-1400,1200,['#9a3412'],210)
    text(d,268,248,'Positive is faster. Mixed signs and a1.30s worst regression: do not enable by default.',12)
    text(d,48,192,'E56:270 main rows. E57:180 actual CLI tasks,2 warmup+7 evaluation blocks, all negative pairs retained.',13)
    text(d,48,165,'Optional per-task PCH: explicit recipe only, no shared/persistent cache, new grants or disabled validation.',13)
    text(d,48,138,'PCH payload14.56-15.51MB is temporary logical file size; process RSS and model/KV memory were not measured.',12)
    text(d,48,111,'Interrupted warmups remain separate; final dual-clock gap <0.029ms. No system power or agent timeout change.',12)
    text(d,48,84,'Remaining execution/scheduling tails are not fully attributed. Ranges are not p95/p99 or stability guarantees.',12)
    text(d,48,57,'No inference code changed; E55 Native acceptance is referenced by source identity, not rerun or relabeled.',12)
    stem=OUT/'sandbox-cpp';renderSVG.drawToFile(d,str(stem.with_suffix('.svg')))
    with tempfile.TemporaryDirectory(prefix='cxf-cpp-chart-') as td:
        pdf=Path(td)/'chart.pdf';renderPDF.drawToFile(d,str(pdf));subprocess.run(['/usr/bin/sips','-s','format','png',str(pdf),'--out',str(stem.with_suffix('.png'))],check=True,capture_output=True)
    p=dict(renderer='ReportLab '+reportlab.Version,renderer_source_sha256=sha(Path(__file__)),evidence_sha256={n:sha(ROOT/'docs/evidence'/n) for n in names},diagnostic_summary=a['summary'],paired_summary=b['paired_summary'],figures={f.name:sha(f) for f in [stem.with_suffix('.svg'),stem.with_suffix('.png')]})
    (OUT/'sandbox-cpp-provenance.json').write_text(json.dumps(p,indent=2)+'\n');print('Rendered E56/E57 only; prior40 figure pairs unchanged')
if __name__=='__main__':main()
