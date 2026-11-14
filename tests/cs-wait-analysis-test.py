import importlib.util,pathlib,unittest
p=pathlib.Path(__file__).resolve().parents[1]/'tools/analyze-cs-waits.py'
spec=importlib.util.spec_from_file_location('analysis',p);analysis=importlib.util.module_from_spec(spec);spec.loader.exec_module(analysis)

def event(kind,start,end,thread=2,identity=4,flags=0):
    return dict(kind=kind,beginNs=start,endNs=end,thread=thread,identity=identity,flags=flags,captureId=1)

class Correlation(unittest.TestCase):
    def test_nested_spans_are_unioned_and_clipped(self):
        rows=[event(5,100,200,thread=1),event(13,90,160),event(13,160,210),event(14,80,140),event(15,130,170)]
        r=analysis.analyze(rows)[0]
        self.assertEqual(r['csObservedNs'],100);self.assertEqual(r['driverWithinCsNs'],70);self.assertEqual(r['unobservedNs'],0)
    def test_unrelated_threads_and_future_sequences_do_not_explain_wait(self):
        rows=[event(5,100,200,thread=1),event(13,100,200,identity=8),event(15,100,200,thread=3)]
        r=analysis.analyze(rows)[0];self.assertEqual(r['csObservedNs'],0);self.assertEqual(r['driverWithinCsNs'],0);self.assertEqual(r['unobservedNs'],100)
    def test_high_priority_can_delay_an_ordered_target_without_same_sequence(self):
        rows=[event(5,100,200,thread=1),event(13,100,150,identity=0,flags=2048),event(13,150,200)]
        r=analysis.analyze(rows)[0];self.assertEqual(r['csObservedNs'],100);self.assertEqual(r['highPriorityNs'],50)
    def test_missing_spans_remain_unknown(self):
        r=analysis.analyze([event(5,100,200,thread=1)])[0]
        self.assertEqual(r['unobservedNs'],100);self.assertFalse(r['targetObserved'])

if __name__=='__main__':unittest.main()
