"""Keep failed/negative tasks and explicit rate denominators in handoff reports."""
import importlib.util
from pathlib import Path
import unittest

spec=importlib.util.spec_from_file_location('spec_report',Path(__file__).resolve().parents[1]/'tools/run_self_speculate.py')
report=importlib.util.module_from_spec(spec);spec.loader.exec_module(report)


class ReportTests(unittest.TestCase):
    def row(self,mode,block,**kw):
        row=dict(kind='model_task',case='a',mode=mode,block=block,warmup=False,total_ms=10,main_ok=True,task_ok=True,
                 prediction_valid=mode=='on',tool_name_hit=mode=='on',exact_hit=mode=='on',adopted=mode=='on',
                 main_sha256='m',tool_sha256='t',final_sha256='f');row.update(kw);return row

    def test_negative_and_failed_not_dropped(self):
        rows=[self.row('off',0),self.row('on',0,total_ms=15),self.row('off',1),self.row('on',1,total_ms=1,task_ok=False,main_ok=False,prediction_valid=False,exact_hit=False,adopted=False)]
        summary,pairs=report.summarize(rows)
        self.assertEqual(len(pairs),2);self.assertEqual(len(summary['negative_pairs']),1)
        self.assertEqual(summary['all_task_paired_saving_ms']['n'],2);self.assertEqual(summary['complete_equal_task_paired_saving_ms']['n'],1)
        self.assertEqual(summary['rates_count_denominator']['adopt_per_on_task'],[1,2])
        self.assertEqual(summary['rates_count_denominator']['exact_hit_per_official_call'],[1,1])

    def test_mismatch_and_incomplete_pairs(self):
        rows=[self.row('off',0),self.row('on',0,main_sha256='different')]
        summary,pairs=report.summarize(rows);self.assertFalse(pairs[0]['comparable']);self.assertEqual(summary['complete_equal_task_paired_saving_ms']['n'],0)
        with self.assertRaises(ValueError):report.summarize(rows[:1])
        with self.assertRaises(ValueError):report.summarize(rows+[rows[0]])


if __name__=='__main__':unittest.main()
