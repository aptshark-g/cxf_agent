#!/usr/bin/env python3
"""E62/E63 cloud request measurements; prior figures remain byte-identical."""
import hashlib,json,subprocess,tempfile
from pathlib import Path
import reportlab
from reportlab.graphics import renderPDF,renderSVG
from reportlab.graphics.shapes import Drawing,Rect,Line,String,Circle
from reportlab.lib.colors import HexColor
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'docs/benchmarks/figures'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def text(d,x,y,s,size=13,color='#334155',anchor='start'):
    d.add(String(x,y,s,fontName='Helvetica',fontSize=size,fillColor=HexColor(color),textAnchor=anchor))
def latency(d,bottom,title,rows,lo,hi,ticks):
    left,right=245,1030;height=185;x=lambda v:left+(v-lo)/(hi-lo)*(right-left)
    text(d,45,bottom+height+45,title,19,'#0f172a')
    for v in ticks:
        d.add(Line(x(v),bottom,x(v),bottom+height,strokeColor=HexColor('#e2e8f0')))
        text(d,x(v),bottom-22,f'{v:g}',11,anchor='middle')
    d.add(Line(x(0),bottom,x(0),bottom+height,strokeColor=HexColor('#64748b')))
    for i,r in enumerate(rows):
        y=bottom+height-(i+.5)*height/4;v=r['task_ms_delta'];color='#b45309' if v>0 else '#0f766e'
        text(d,left-16,y-4,'Paired block '+str(r['block']),12,anchor='end')
        d.add(Line(x(0),y,x(v),y,strokeColor=HexColor(color),strokeWidth=2))
        d.add(Circle(x(v),y,4,fillColor=HexColor(color),strokeColor=None));text(d,right+15,y-4,f'{v:+.3f} ms',12,color)
    text(d,right,bottom-43,'After - before (ms): negative is faster; all4 evaluation pairs',11,anchor='end')
def main():
    names=['cloud-request-paired-20261102.json','cloud-serialization-20261102.json']
    a,b=[json.loads((ROOT/'docs/evidence'/n).read_text()) for n in names]
    d=Drawing(1200,1120);d.add(Rect(0,0,1200,1120,fillColor=HexColor('#ffffff'),strokeColor=None))
    text(d,45,1073,'Cloud requests: fewer bytes, no stable latency gain',26,'#0f172a')
    text(d,45,1043,'DeepSeek Flash gateway alias |20 real CLI tasks /50 API rounds |1 warmup +4 evaluation pairs per task',13)
    text(d,45,992,'A. Same first request: isolate prompt, whitespace and Unicode representation',18,'#0f172a')
    selections=[('before','ascii_spaced','Before: ASCII + spaces'),('before','ascii_compact','Before: ASCII compact'),('before','utf8_compact','Before: UTF-8 compact'),('after','utf8_compact','After: UTF-8 compact')]
    for i,(prompt,encoding,label) in enumerate(selections):
        v=next(r['request_bytes'] for r in b['summary'] if r['prompt']==prompt and r['encoding']==encoding);y=944-i*38
        text(d,45,y+3,label,12);d.add(Rect(245,y,785*v/2700,20,fillColor=HexColor('#0e7490' if prompt=='before' else '#0f766e'),strokeColor=None));text(d,245+785*v/2700+12,y+4,f'{v} B',12)
    text(d,245,785,'2513 ->1840 B: -82 whitespace, -510 escaping, -81 prompt. Model input: -18 tokens/round (E62).',12)
    latency(d,505,'B. Direct read: whole-task deltas (individual paired results)',[r for r in a['pairs'] if r['case']=='direct'],-150,100,[-150,-100,-50,0,50,100])
    latency(d,180,'C. List then read: whole-task deltas (different linear axis)',[r for r in a['pairs'] if r['case']=='listed'],-14000,1000,[-14000,-10000,-6000,-2000,0])
    text(d,45,103,'4/8 evaluated pairs regress. Paired medians: +12.707ms direct, +46.684ms listed. No tail guarantee.',13)
    text(d,45,78,'The -12.593s pair includes8.531s /7.709s before-mode API waits; local relay <1ms each. Cause upstream unresolved.',12)
    text(d,45,53,'Cache fields: cached_tokens returned50/50; dedicated hit/miss absent50/50. No forced cold-cache or cache-gain claim.',12)
    text(d,45,28,'E63 wall-time negative retained: old spaced ASCII7.066us/op -> new compact UTF-8 8.325us/op; API time excluded.',12)
    stem=OUT/'cloud-request';renderSVG.drawToFile(d,str(stem.with_suffix('.svg')))
    with tempfile.TemporaryDirectory(prefix='cxf-cloud-chart-') as td:
        pdf=Path(td)/'figure.pdf';renderPDF.drawToFile(d,str(pdf));subprocess.run(['/usr/bin/sips','-s','format','png',str(pdf),'--out',str(stem.with_suffix('.png'))],check=True,capture_output=True)
    provenance={'renderer':'ReportLab '+reportlab.Version,'renderer_source_sha256':sha(Path(__file__)),'evidence_sha256':{n:sha(ROOT/'docs/evidence'/n) for n in names},'figures':{p.name:sha(p) for p in [stem.with_suffix('.svg'),stem.with_suffix('.png')]}}
    (OUT/'cloud-request-provenance.json').write_text(json.dumps(provenance,indent=2)+'\n');print('Rendered E62/E63; prior42 figure pairs unchanged')
if __name__=='__main__':main()
