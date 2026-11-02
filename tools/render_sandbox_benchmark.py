#!/usr/bin/env python3
"""Render E52 only; never rewrite historical figures or their provenance."""
import hashlib
import json
import math
from pathlib import Path
import subprocess
import tempfile
from reportlab.graphics.charts.barcharts import VerticalBarChart
from reportlab.graphics.shapes import Drawing,Rect,String,Line
from reportlab.graphics import renderPDF,renderSVG
from reportlab.lib.colors import HexColor
import reportlab
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'docs/benchmarks/figures'
EVIDENCE=ROOT/'docs/evidence/test-sandbox-20261101.json'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def text(d,x,y,s,size=14,anchor='start',color='#374151'):
    d.add(String(x,y,s,fontName='Helvetica',fontSize=size,textAnchor=anchor,fillColor=HexColor(color)))
def panel(d,y,title,categories,series,colors,height=230):
    text(d,54,y+height+42,title,20,color='#111827')
    c=VerticalBarChart();c.x=100;c.y=y;c.width=1010;c.height=height;c.data=[[v['median'] for v in a] for a in series]
    c.categoryAxis.categoryNames=categories;c.categoryAxis.labels.fontName='Helvetica';c.categoryAxis.labels.fontSize=13;c.categoryAxis.labels.dy=-10
    lo=min(0,min(v['min'] for a in series for v in a));hi=max(v['max'] for a in series for v in a)
    step=10**math.floor(math.log10(max(hi-lo,1)))/2;low=math.floor(lo*1.2/step)*step;high=math.ceil(hi*1.16/step)*step
    c.valueAxis.valueMin=low;c.valueAxis.valueMax=high;c.valueAxis.valueStep=(high-low)/5;c.valueAxis.labels.fontName='Helvetica';c.valueAxis.labels.fontSize=12
    c.valueAxis.visibleGrid=True;c.valueAxis.gridStrokeColor=HexColor('#e5e7eb');c.valueAxis.strokeColor=None;c.groupSpacing=28;c.barSpacing=5
    for i,color in enumerate(colors):c.bars[i].fillColor=HexColor(color);c.bars[i].strokeColor=None
    d.add(c);c.draw()  # Use actual library bar geometry for range overlays.
    def vy(v):return y+(v-low)/(high-low)*height
    d.add(Line(c.x,vy(0),c.x+c.width,vy(0),strokeColor=HexColor('#64748b'),strokeWidth=1))
    for k,a in enumerate(series):
        for i,v in enumerate(a):
            bx,_,bw,_=c._barPositions[k][i];x=bx+bw/2
            d.add(Line(x,vy(v['min']),x,vy(v['max']),strokeColor=HexColor('#334155'),strokeWidth=1.2))
            for end in ['min','max']:d.add(Line(x-5,vy(v[end]),x+5,vy(v[end]),strokeColor=HexColor('#334155')))
            text(d,x,vy(max(0,v['median']))+8,f"{v['median']:.1f}",13,'middle',colors[k])
    text(d,55,y+height+13,'Milliseconds; medians with observed min/max (not confidence intervals)',12)
def main():
    e=json.loads(EVIDENCE.read_text());summary=e['summary'];rows=e['rows']
    def group(kind,case,files):return next(v for v in summary if (v['kind'],v['case'],v['files'])==(kind,case,files))
    d=Drawing(1200,1380);d.add(Rect(0,0,1200,1380,fillColor=HexColor('#ffffff'),strokeColor=None))
    text(d,48,1334,'test.run: file and network isolation has a cost',28,color='#111827')
    text(d,48,1304,'E52 | Apple M5 / macOS | current off vs Seatbelt on | serial, unsanitized timing',15)
    a=[group('sandbox_timing',case,count)['wall_ms_overhead'] for count,case in [(8,'noop'),(8,'read'),(256,'noop'),(256,'read')]]
    panel(d,980,'A. Real command API: paired on minus off overhead',['8 extra / no-op','8 extra / read','256 extra / no-op','256 extra / read'],[a],['#d97706'])
    b=[group('cli_timing',case,0)['wall_ms_overhead'] for case in ['noop','clt_compile_tmp']]
    panel(d,595,'B. Complete CLI: paired overhead, scripted model choices',['No-op tool','Explicit CLT compile'],[b],['#087f8c'])
    case='system_cc_compile_write';series=[]
    for mode in ['off','on']:
        vals=[r['wall_ms'] for r in rows if r['kind']=='cli_timing' and r['case']==case and r['phase']=='evaluation' and r['mode']==mode]
        import statistics
        series.append([dict(median=statistics.median(vals),min=min(vals),max=max(vals))])
    panel(d,210,'C. Standard Xcode cc: complete CLI wall time (large regression)',['Standard cc compile and run'],series,['#64748b','#d97706'])
    text(d,850,495,'Gray: off    Orange: isolated',13)
    syscc=group('cli_timing',case,0)['wall_ms_overhead']
    text(d,55,127,f"Standard cc paired cost: +{syscc['median']:.1f} ms median, +{syscc['max']:.1f} ms worst; 7/7 slower.",15,color='#9a3412')
    text(d,55,101,'Most standard-cc growth is inside the command. Cache/IPC causes are not separately proven.',13)
    text(d,55,77,'230 rows include warmups; 93 evaluation pairs. Full raw pairs retain negative values and failures.',13)
    text(d,55,53,'No model inference or real cloud in timing. Fixed job stack capacity is not peak RSS or physical KV.',13)
    text(d,55,29,'Default: workspace read-only + private temporary writes + network denied; explicit grants only.',12)
    OUT.mkdir(parents=True,exist_ok=True);stem=OUT/'test-sandbox-overhead'
    renderSVG.drawToFile(d,str(stem.with_suffix('.svg')))
    with tempfile.TemporaryDirectory(prefix='cxf-sandbox-figure-') as td:
        pdf=Path(td)/'chart.pdf';renderPDF.drawToFile(d,str(pdf));subprocess.run(['/usr/bin/sips','-s','format','png',str(pdf),'--out',str(stem.with_suffix('.png'))],check=True,capture_output=True)
    provenance=dict(renderer='ReportLab '+reportlab.Version,renderer_source_sha256=sha(Path(__file__)),evidence_sha256=sha(EVIDENCE),summary=e['summary'],figures={v.name:sha(v) for v in [stem.with_suffix('.svg'),stem.with_suffix('.png')]})
    (OUT/'sandbox-provenance.json').write_text(json.dumps(provenance,indent=2)+'\n')
    print('Rendered E52 figure only; historical37 figure pairs unchanged')
if __name__=='__main__':main()
