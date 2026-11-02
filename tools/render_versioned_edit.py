#!/usr/bin/env python3
"""Render E59/E61 measured costs; all previous figures remain unchanged. Requires ReportLab."""
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
def panel(d,bottom,title,series,names,colors,low,high):
    left,right,height=230,1080,190;x=lambda v:left+(v-low)/(high-low)*(right-left)
    text(d,45,bottom+height+58,title,20,'#0f172a')
    for i in range(6):
        v=low+(high-low)*i/5;d.add(Line(x(v),bottom,x(v),bottom+height,strokeColor=HexColor('#e2e8f0')));text(d,x(v),bottom-20,f'{v:.1f}',11,anchor='middle')
    d.add(Line(x(0),bottom,x(0),bottom+height,strokeColor=HexColor('#64748b'),strokeWidth=.7))
    for i,label in enumerate(['1KiB','16KiB','64KiB']):
        y=bottom+height-(i+.5)*height/3;text(d,left-16,y-4,label,14,anchor='end')
        for j,values in enumerate(series):
            row=values[i];yy=y+16-j*16;color=colors[j];a,b=x(0),x(row['median']);
            d.add(Rect(min(a,b),yy-5,max(1,abs(b-a)),10,fillColor=HexColor(color),strokeColor=None));d.add(Line(x(row['min']),yy,x(row['max']),yy,strokeColor=HexColor('#334155')))
            for field in ['min','max']:d.add(Line(x(row[field]),yy-3,x(row[field]),yy+3,strokeColor=HexColor('#334155')))
            text(d,right+12,yy-4,f"{row['median']:.3f}",11,color)
    for j,name in enumerate(names):text(d,230+j*290,bottom+height+25,name,12,colors[j])
    text(d,1080,bottom-42,'Milliseconds / operation; observed median and min/max',11,anchor='end')
def main():
    names=['versioned-edit-overhead-20261102.json','versioned-edit-cost-probe-20261102.json'];a,b=[json.loads((ROOT/'docs/evidence'/n).read_text()) for n in names]
    sizes=[1024,16384,65536]
    def scale(v):return {k:x/1000 for k,x in v.items()}
    pairs=[[scale(next(r for r in a['paired_summary'] if r['case']==case and r['file_bytes']==size)['overhead_us']) for size in sizes] for case in ['single','multi','workflow']]
    policies=[[scale(next(r for r in b['summary'] if r['case']==case and r['file_bytes']==size)['wall_us']) for size in sizes] for case in ['policy_0','policy_1','policy_16']]
    d=Drawing(1200,990);d.add(Rect(0,0,1200,990,fillColor=HexColor('#ffffff'),strokeColor=None))
    text(d,45,946,'Versioned multiline editing: measured overhead',27,'#0f172a')
    text(d,45,916,'Apple M5 | default file/content restrictions retained | serial unsanitized C-handler measurements',13)
    low=min(0,min(v['min'] for g in pairs for v in g));high=math.ceil(max(v['max'] for g in pairs for v in g))
    panel(d,600,'A. Added wall time versus legacy editing (positive is slower)',pairs,['Single line -> versioned','Whole file -> multiline','Read + line -> read + edit'],['#0e7490','#d97706','#7c3aed'],math.floor(low),high)
    panel(d,230,'B. Isolated cost of one full text-policy scan',policies,['0 registered secrets','1 registered secret','16 registered secrets'],['#0f766e','#d97706','#9f1239'],0,math.ceil(max(v['max'] for g in policies for v in g)))
    text(d,45,150,'fstat + version formatting: ~0.43-0.46us. Lock + unlock: ~0.46-0.49us. Source and result scans are retained.',13)
    text(d,45,125,'E59:363 block records (2 warmup +9 evaluation blocks). E61:153 diagnostics (1 warmup +9 evaluation blocks).',12)
    text(d,45,100,'Block means cover8 or64 handler operations. Independent probe medians cannot be added to reconstruct task time.',12)
    text(d,45,75,'Large whole-file writes are C-handler controls; CLI args stay1KiB. Cloud model/network time is excluded.',12)
    text(d,45,50,'All negative pairs retained. No general speedup, p95/p99, RSS or physical-KV claim.',12)
    stem=OUT/'versioned-edit';renderSVG.drawToFile(d,str(stem.with_suffix('.svg')))
    with tempfile.TemporaryDirectory(prefix='cxf-edit-chart-') as temp:
        pdf=Path(temp)/'chart.pdf';renderPDF.drawToFile(d,str(pdf));subprocess.run(['/usr/bin/sips','-s','format','png',str(pdf),'--out',str(stem.with_suffix('.png'))],capture_output=True,check=True)
    provenance={'renderer':'ReportLab '+reportlab.Version,'renderer_source_sha256':sha(Path(__file__)),'evidence_sha256':{n:sha(ROOT/'docs/evidence'/n) for n in names},'paired_summary':a['paired_summary'],'probe_summary':b['summary'],'figures':{p.name:sha(p) for p in [stem.with_suffix('.svg'),stem.with_suffix('.png')]}}
    (OUT/'versioned-edit-provenance.json').write_text(json.dumps(provenance,indent=2)+'\n');print('Rendered E59/E61; previous41 figure pairs unchanged')
if __name__=='__main__':main()
