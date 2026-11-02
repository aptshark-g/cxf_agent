#!/usr/bin/env python3
"""Render E54/E55 only; preserve every historical figure byte."""
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
def panel(d,bottom,title,labels,values,low,high,color,height=190):
    left,right=265,1115;width=right-left
    text(d,48,bottom+height+65,title,20,'#0f172a')
    text(d,48,bottom+height+39,'Milliseconds; median and observed min/max, not confidence intervals',12)
    x=lambda v:left+(v-low)/(high-low)*width
    for i in range(6):
        value=low+(high-low)*i/5;xx=x(value);d.add(Line(xx,bottom,xx,bottom+height,strokeColor=HexColor('#e2e8f0')));text(d,xx,bottom-21,f'{value:.0f}',11,anchor='middle')
    d.add(Line(x(0),bottom,x(0),bottom+height,strokeColor=HexColor('#64748b')))
    for i,(label,value) in enumerate(zip(labels,values)):
        yy=bottom+height-(i+.5)*height/len(labels);mid=value['median'];a,b=x(0),x(mid)
        d.add(Rect(min(a,b),yy-14,max(abs(b-a),1),28,fillColor=HexColor(color),strokeColor=None))
        d.add(Line(x(value['min']),yy,x(value['max']),yy,strokeColor=HexColor('#334155')))
        for end in ['min','max']:d.add(Line(x(value[end]),yy-6,x(value[end]),yy+6,strokeColor=HexColor('#334155')))
        text(d,left-15,yy-4,label,13,anchor='end');text(d,right+12,yy-4,f'{mid:.1f}',13,color)
def main():
    a=json.loads((ROOT/'docs/evidence/sandbox-cc-diagnostic-20261101.json').read_text());b=json.loads((ROOT/'docs/evidence/sandbox-cc-tasks-20261101.json').read_text())
    def diag(case,field):return next(r for r in a['summary'] if r['case']==case and r['mode']=='on')[field]
    def paired(case,label):return next(r for r in b['paired_summary'] if r['case']==case and r['comparison']==label)['wall_delta_ms']
    d=Drawing(1200,1320);d.add(Rect(0,0,1200,1320,fillColor=HexColor('#ffffff'),strokeColor=None))
    text(d,48,1274,'Same Xcode and SDK: cache diagnosis and explicit binding',26,'#0f172a')
    text(d,48,1245,'Apple M5 / macOS | serial, unsanitized | same deny rules | all negative pairs retained',13)
    panel(d,947,'A. E54: unchanged sandbox policy; lookup-cache reuse/invalidation',['First compile','Second, cache reused','Second, cache removed'],[diag('reuse','phase_first_ms'),diag('reuse','phase_second_ms'),diag('invalidate','phase_second_ms')],0,2800,'#0e7490',160)
    cases=['checksum','archive','templates'];labels=['C checksum','C static archive','C++ templates']
    panel(d,594,'B. E55: complete-CLI saving, original on minus explicitly bound on',labels,[paired(c,'binding_saving') for c in cases],0,4500,'#0f766e',170)
    panel(d,259,'C. Remaining cost: bound on minus bound off',labels,[paired(c,'bound_isolation_overhead') for c in cases],-200,150,'#b45309',170)
    text(d,48,178,'A: 5 evaluation blocks. B/C: 7 matched evaluation blocks per new task; 108 CLI rows including warmups.',13)
    text(d,48,151,'Binding is explicit PATH + SDKROOT, not a new grant, automatic compiler substitution or shared cache.',13)
    text(d,48,124,'Zero-config shims retain their old regression. C++ residual range remains wide, including negative pairs.',13)
    text(d,48,97,'No-log and SDKROOT-only candidates did not remove the seconds-scale cost; both remain negative controls.',13)
    text(d,48,70,'Denied-API microprobes do not allocate all Xcode initialization/retry time; stack sampling returned no call graph.',12)
    text(d,48,43,'This is a tool-command optimization; no model inference, KV memory or historical streaming speedup is claimed.',12)
    stem=OUT/'sandbox-cc';renderSVG.drawToFile(d,str(stem.with_suffix('.svg')))
    with tempfile.TemporaryDirectory(prefix='cxf-cc-chart-') as td:
        pdf=Path(td)/'chart.pdf';renderPDF.drawToFile(d,str(pdf));subprocess.run(['/usr/bin/sips','-s','format','png',str(pdf),'--out',str(stem.with_suffix('.png'))],check=True,capture_output=True)
    provenance=dict(renderer='ReportLab '+reportlab.Version,renderer_source_sha256=sha(Path(__file__)),evidence_sha256={name:sha(ROOT/'docs/evidence'/name) for name in ['sandbox-cc-diagnostic-20261101.json','sandbox-cc-tasks-20261101.json']},diagnostic_summary=a['summary'],task_paired_summary=b['paired_summary'],figures={p.name:sha(p) for p in [stem.with_suffix('.svg'),stem.with_suffix('.png')]})
    (OUT/'sandbox-cc-provenance.json').write_text(json.dumps(provenance,indent=2)+'\n');print('Rendered E54/E55 only; prior39 figure pairs unchanged')
if __name__=='__main__':main()
