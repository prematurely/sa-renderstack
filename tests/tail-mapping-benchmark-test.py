"""Reject invalid evidence before a timing comparison can be accepted."""
import copy
import importlib.util
from pathlib import Path
import unittest
import tempfile

spec = importlib.util.spec_from_file_location('benchmark', Path(__file__).with_name('tail-mapping-benchmark.py'))
benchmark = importlib.util.module_from_spec(spec)
spec.loader.exec_module(benchmark)


def runtime_row(label='A'):
    row = {metric: 10 for metric in benchmark.RUNTIME_METRICS}
    row.update(type='event-stream-benchmark', valid=True, mode='normal', operations=100,
               totalMainNs=30, startCycles=3, stopCycles=5, totalMainThreadCycles=18,
               successfulPresents=10, failedPresents=0, dropped=0, unavailableMask=18,
               reportComplete=True, reportRequested=True, workersFinished=True,
               storageMetadataPresent=True, storageComplete=True, storageErrors=0, storageUnread=0,
               snapshotEvents=10, snapshotUnread=0, snapshotReportFinished=True,
               suite='ab', case='normal', pair=0, label=label)
    return row


class EvidenceTests(unittest.TestCase):
    def test_full_main_timing_cannot_omit_completion_callback(self):
        row = runtime_row('B')
        benchmark.validate_row(row, 'runtime', 'normal', candidate=True)
        for field in ('totalMainNs', 'totalMainThreadCycles'):
            invalid = dict(row, **{field: row[field]-1})
            with self.assertRaisesRegex(ValueError, 'omits or duplicates'):
                benchmark.validate_row(invalid, 'runtime', 'normal', candidate=True)

    def test_zero_observations_remain_and_zero_denominator_has_no_percentage(self):
        a, b = runtime_row(), runtime_row('B')
        a['mainThreadCycles'] = b['mainThreadCycles'] = 0
        result = benchmark.summarize([a, b], 'runtime')
        cycles = next(s for s in result['summary'] if s['metric'] == 'mainThreadCycles')
        self.assertEqual((cycles['aMedian'], cycles['bMedian'], cycles['zeroAPairs'], cycles['zeroBPairs']), (0, 0, 1, 1))
        self.assertIsNone(cycles['changeMedianPercent'])

    def test_equal_work_is_required(self):
        a, b = runtime_row(), runtime_row('B')
        b['operations'] += 1
        with self.assertRaisesRegex(ValueError, 'different work counts'):
            benchmark.summarize([a, b], 'runtime')

    def test_pairing_rejects_missing_and_duplicate_samples(self):
        for rows in ([runtime_row()], [runtime_row(), runtime_row(), runtime_row('B')]):
            with self.assertRaisesRegex(ValueError, 'unpaired|duplicate'):
                benchmark.summarize(rows, 'runtime')

    def test_pairwise_changes_are_not_ratio_of_pooled_means(self):
        a, b = runtime_row(), runtime_row('B')
        a['workPresentDrainNs'], b['workPresentDrainNs'] = 100, 50
        a2, b2 = copy.deepcopy(a), copy.deepcopy(b)
        a2.update(pair=1, workPresentDrainNs=1000)
        b2.update(pair=1, workPresentDrainNs=1000)
        summary = benchmark.summarize([a, b, a2, b2], 'runtime')['summary']
        item = next(s for s in summary if s['metric'] == 'workPresentDrainNs')
        self.assertEqual(item['changeMedianPercent'], -25)
        self.assertEqual(item['fasterPairs'], 1)

    def test_expected_baseline_truncation_is_explicit_but_candidate_must_be_complete(self):
        row = runtime_row()
        row.update(mode='high', events=65536, dropped=123, reportComplete=True,
                   lateEvent=False, lateFrameOverlaps=0)
        benchmark.validate_row(row, 'runtime', 'high', candidate=False)
        with self.assertRaisesRegex(ValueError, 'incomplete'):
            benchmark.validate_row(row, 'runtime', 'high', candidate=True)
        row.update(events=80000, snapshotEvents=80000, dropped=0, lateEvent=True, lateFrameOverlaps=1)
        benchmark.validate_row(row, 'runtime', 'high', candidate=True)

    def test_lossless_count_without_tail_association_is_not_accepted(self):
        row = runtime_row()
        row.update(mode='high', events=80000, lateEvent=True, lateFrameOverlaps=0)
        with self.assertRaisesRegex(ValueError, 'tail and frame'):
            benchmark.validate_row(row, 'runtime', 'high', candidate=True)

    def test_invalid_finite_and_failure_values_are_rejected(self):
        for value in (float('nan'), float('inf'), -1, True):
            row = runtime_row()
            row['p99Ns'] = value
            with self.assertRaises(ValueError):
                benchmark.validate_row(row, 'runtime', 'normal', candidate=True)
        for field, value in [('valid', False), ('failedPresents', 1), ('unavailableMask', 1), ('reportComplete', False)]:
            row = runtime_row()
            row[field] = value
            with self.assertRaises(ValueError):
                benchmark.validate_row(row, 'runtime', 'normal', candidate=True)

    def test_mapping_requires_pixels_lifetime_and_same_budget(self):
        row = {metric: 1 for metric in benchmark.MAPPING_METRICS}
        row.update(type='mapping-budget-benchmark', workload='hot', iterations=64,
                   pixelsValid=True, failedPresents=0, mappedAfterReleaseBytes=0, valid=True,
                   queryValid=True, affinityScope='main-thread-only-lowest-allowed',
                   residencyPass='separate-untimed',
                   suite='ab', case='hot', pair=0, label='A', budgetMiB=100)
        benchmark.validate_row(row, 'mapping', 'hot', candidate=True)
        for field, value in [('pixelsValid', False), ('queryValid', False),
                             ('releaseSubmitNs', 2),
                             ('mappedAfterReleaseBytes', 65536), ('affinityScope', ''),
                             ('residencyPass', '')]:
            invalid = dict(row, **{field: value})
            with self.assertRaises(ValueError):
                benchmark.validate_row(invalid, 'mapping', 'hot', candidate=True)
        b = dict(row, label='B', budgetMiB=128)
        with self.assertRaisesRegex(ValueError, 'different mapping budgets'):
            benchmark.summarize([row, b], 'mapping')

    def test_claimed_complete_cannot_replace_missing_or_partial_report(self):
        row = runtime_row('B')
        row['events'] = 1
        text = ('# measurement captureId=1 status=complete unavailableMask=18\n'
                'telemetry available=1 active=0 captureId=1 recorded=1 backendDropped=0 hostDropped=0 '
                'invalidCaptureEvents=0 inFlight=0 inFlightAtStop=0 lateDroppedAtRead=0 remaining=0 error=""\n'
                'telemetryEvent index=0 captureId=1\n'
                'telemetryStorage complete=1 received=1 accepted=1 written=1 reported=1 omitted=0 unread=0 errors=0x0\n'
                'contextSummary available=0 reason=disabled\n'
                'reportEnd captureId=1 completed=1\n')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'report.log'
            path.write_text(text, encoding='utf-8')
            self.assertEqual(benchmark.validate_runtime_report(path, row, True)['verifiedReportEvents'], 1)
            invalid_reports = ['', text.replace('status=complete', 'status=pending'),
                               text.replace('telemetryEvent index=0 captureId=1\n', ''),
                               text.replace('errors=0x0', 'errors=0x4'),
                               text.replace('reportEnd captureId=1 completed=1\n', ''),
                               text.replace('reportEnd captureId=1', 'reportEnd captureId=2'),
                               text[:text.index('telemetryStorage ')]]
            for report in invalid_reports:
                path.write_text(report, encoding='utf-8')
                with self.assertRaises(ValueError):
                    benchmark.validate_runtime_report(path, row, True)

    def test_legacy_full_capture_context_tail_and_expected_missing_journal(self):
        row = runtime_row('A')
        row['events'] = 1
        text = ('# measurement captureId=2 status=complete-partial unavailableMask=16\n'
                'telemetry available=1 active=0 captureId=2 recorded=1 backendDropped=0 hostDropped=0 '
                'invalidCaptureEvents=0 inFlight=0 inFlightAtStop=0 lateDroppedAtRead=0 remaining=0 error=""\n'
                'telemetryEvent index=0 captureId=2\n'
                'contextSummary available=1\n'
                'contextCounts excludedPolls=1 candidatePolls=0\n'
                'contextRange kind=excluded-steady-state polls=1\n')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'legacy.log'
            path.write_text(text, encoding='utf-8')
            result = benchmark.validate_runtime_report(path, row, False, allowed_unavailable=16)
            self.assertEqual(result['verifiedReportStatus'], 'complete-partial')
            with self.assertRaisesRegex(ValueError, 'finalization record'):
                benchmark.validate_runtime_report(path, row, True, allowed_unavailable=16)
            for bad in (text.replace('unavailableMask=16', 'unavailableMask=18'),
                        text.replace('contextCounts excludedPolls=1 candidatePolls=0\n', ''),
                        text.replace('status=complete-partial', 'status=pending')):
                path.write_text(bad, encoding='utf-8')
                with self.assertRaises(ValueError):
                    benchmark.validate_runtime_report(path, row, False, allowed_unavailable=16)


if __name__ == '__main__':
    unittest.main()
