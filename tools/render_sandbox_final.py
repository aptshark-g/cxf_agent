#!/usr/bin/env python3
"""Render E53 current overhead, preserving E52 and all prior plots."""
import json
from pathlib import Path
import subprocess
import tempfile
from reportlab.graphics.shapes import Drawing,Rect
from reportlab.graphics.charts.barcharts import VerticalBarChart
from reportlab.graphics import renderSVG,renderPDF
from reportlab.lib.colors import HexColor
import reportlab
from render_sandbox_benchmark import text,panel,sha
ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT/'docs/evidence/test-sandbox-host-20261101.json'
OUT=ROOT/'docs/benchmarks/figures'
def main():
    e=json.loads(SOURCE.read_text());summary=e['summary'];d=Drawing(1200,1050);d.add(Rect(0,0,1200,1050,fillColor=HexColor('#ffffff'),strokeColor=None))
    text(d,48,1004,'test.run: final host protection and isolation cost',27,color='#111827')
    text(d,48,974,'E53 | Apple M5 / macOS | current off/on | serial and unsanitized | all negative pairs retained',14)
    def cell(case):return next(v for v in summary if v['kind']=='cli_timing' and v['case']==case)
    panel(d,645,'A. Complete CLI: paired on minus off overhead',['No-op tool','Explicit CLT compile'],[[cell(c)['wall_ms_overhead'] for c in ['noop','clt_compile_tmp']]],['#087f8c'],height=220)
    panel(d,270,'B. Standard Xcode cc: paired complete-CLI regression',['Standard cc compile and run'],[[cell('system_cc_compile_write')['wall_ms_overhead']]],['#d97706'],height=220)
    for y,case in [(178,'noop'),(154,'clt_compile_tmp'),(130,'system_cc_compile_write')]:
        r=cell(case);v=r['wall_ms_overhead'];text(d,55,y,f"{case}: paired {v['median']:+.3f} ms; range [{v['min']:+.3f}, {v['max']:+.3f}]; on slower {r['on_slower']}/{r['pairs']}",13)
    text(d,55,100,'230 rows / 93 evaluation pairs. Positive cost is slower; ranges are observed min/max, not confidence intervals.',12)
    text(d,55,77,'Host environment: unrestricted control sees the synthetic marker; protected host omits it even with network allowed.',12)
    text(d,55,54,'File/network permissions remain explicit. Other same-UID processes, VM isolation and peak child RSS are not covered.',12)
    text(d,55,31,'Timing has scripted model choices, no model inference. E52 remains the pre-fix historical experiment.',12)
    for obj in d.contents:
        if isinstance(obj,VerticalBarChart) and len(obj.data[0])==2:obj.categoryAxis.labels.dy=-60
    stem=OUT/'test-sandbox-final';renderSVG.drawToFile(d,str(stem.with_suffix('.svg')))
    with tempfile.TemporaryDirectory(prefix='cxf-sandbox-final-') as td:
        pdf=Path(td)/'chart.pdf';renderPDF.drawToFile(d,str(pdf));subprocess.run(['/usr/bin/sips','-s','format','png',str(pdf),'--out',str(stem.with_suffix('.png'))],check=True,capture_output=True)
    p=dict(renderer='ReportLab '+reportlab.Version,renderer_source_sha256=sha(Path(__file__)),base_renderer_sha256=sha(ROOT/'tools/render_sandbox_benchmark.py'),evidence_sha256=sha(SOURCE),summary=e['summary'],figures={v.name:sha(v) for v in [stem.with_suffix('.svg'),stem.with_suffix('.png')]})
    (OUT/'sandbox-final-provenance.json').write_text(json.dumps(p,indent=2)+'\n');print('Rendered E53 only; previous38 figure pairs unchanged')
if __name__=='__main__':main()
