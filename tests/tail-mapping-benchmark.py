"""Repeated fixed-binary version A/B and A/A for F8 storage and mapping policy.

Each sample is a fresh process, run sequentially. Timings are fixture results;
the runner does not estimate game FPS from these workloads.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import statistics
import subprocess


def digest(path: Path) -> str:
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def write_json(path: Path, value) -> None:
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def finite_number(row, key, positive=False):
    value = row[key]
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise ValueError(f'invalid {key}: {value!r}')
    if value < 0 or (positive and value == 0):
        raise ValueError(f'invalid {key}: {value!r}')
    return value


RUNTIME_METRICS = ('workPresentDrainNs', 'mainThreadCycles', 'p95Ns', 'p99Ns', 'maxNs',
                   'maxPresentNs', 'startPresentNs', 'completionInterruptNs',
                   'totalMainNs', 'totalMainThreadCycles', 'reportWaitNs', 'events', 'dropped')
MAPPING_METRICS = ('totalNs', 'mainThreadCycles', 'p95Ns', 'p99Ns', 'maxNs',
                  'presentNs', 'drainNs', 'releaseNs', 'releaseSubmitNs', 'mapCalls', 'unmapCalls', 'mapNs', 'unmapNs',
                  'maxUnmapBurst', 'maxUnmapBurstNs', 'operationMaxUnmapBurst',
                  'operationMaxUnmapBurstNs', 'cleanupUnmapCalls', 'cleanupUnmapNs',
                  'mappedPeakBytes', 'mappedEndBytes')


def validate_row(row, kind, case, candidate):
    expected = 'event-stream-benchmark' if kind == 'runtime' else 'mapping-budget-benchmark'
    if row.get('type') != expected or row.get('valid') is not True:
        raise ValueError('fixture did not complete a valid real workload')
    if row.get('mode' if kind == 'runtime' else 'workload') != case:
        raise ValueError('fixture workload differs from requested case')
    for metric in RUNTIME_METRICS if kind == 'runtime' else MAPPING_METRICS:
        finite_number(row, metric)
    if kind == 'runtime':
        finite_number(row, 'operations', positive=True)
        finite_number(row, 'workPresentDrainNs', positive=True)
        for metric in ('startCycles', 'stopCycles'):
            finite_number(row, metric)
        if row['totalMainNs'] != row['startPresentNs'] + row['workPresentDrainNs'] + row['completionInterruptNs']:
            raise ValueError('main timing total omits or duplicates a measured callback')
        if row['totalMainThreadCycles'] != row['startCycles'] + row['mainThreadCycles'] + row['stopCycles']:
            raise ValueError('main cycle total omits or duplicates a measured callback')
        finite_number(row, 'successfulPresents', positive=True)
        if row['failedPresents'] != 0:
            raise ValueError('failed Present in measurement')
        if row.get('workersFinished') is not True:
            raise ValueError('capture workers have not finished')
        if case == 'inactive':
            if row['events'] != 0 or row['dropped'] != 0 or row.get('reportRequested') is not False:
                raise ValueError('inactive F8 unexpectedly collected events')
        elif candidate or case != 'high':
            # This fixture isolates storage: CPU sampling and journal attribution
            # are intentionally disabled (bits 2 and 16). Other failures remain fatal.
            if row['dropped'] != 0 or row['unavailableMask'] & ~18 or row['reportComplete'] is not True or row.get('reportRequested') is not True:
                raise ValueError('incomplete active F8 capture')
            if row['events'] <= 0:
                raise ValueError('active F8 report contains no events')
            if case == 'high' and (row['events'] <= 65536 or not row['lateEvent'] or row['lateFrameOverlaps'] <= 0):
                raise ValueError('candidate did not retain high-volume tail and frame overlap')
            if candidate and (row.get('storageMetadataPresent') is not True or row.get('storageComplete') is not True or
                              row.get('storageErrors') != 0 or row.get('storageUnread') != 0 or
                              row.get('snapshotEvents') != row['events'] or row.get('snapshotUnread') != 0 or
                              row.get('snapshotReportFinished') is not True):
                raise ValueError('candidate final snapshot or storage is incomplete')
    else:
        finite_number(row, 'iterations', positive=True)
        finite_number(row, 'budgetMiB', positive=True)
        finite_number(row, 'totalNs', positive=True)
        if row['pixelsValid'] is not True or row.get('queryValid') is not True or row['failedPresents'] != 0 or row['mappedAfterReleaseBytes'] != 0:
            raise ValueError('mapping pixel, Present, or lifetime evidence failed')
        if row.get('affinityScope') != 'main-thread-only-lowest-allowed' or row.get('residencyPass') != 'separate-untimed':
            raise ValueError('mapping measurement scope is missing or unexpected')
        if row['releaseSubmitNs'] > row['releaseNs']:
            raise ValueError('release completion cannot precede the submitting thread')


def validate_runtime_report(path, row, candidate, allowed_unavailable=18):
    """Check the report itself, not just a fixture's completion booleans."""
    header = telemetry = storage = None
    last_line = ''
    context_summary = context_counts = False
    records = 0
    def fields(line):
        return dict(re.findall(r'(\w+)=("[^"]*"|\S+)', line))
    with path.open(encoding='utf-8-sig') as stream:
        for line in stream:
            if line.strip():
                last_line = line.strip()
            context_summary |= line.startswith('contextSummary ')
            context_counts |= line.startswith('contextCounts ')
            if line.startswith('# measurement '):
                if header is not None:
                    raise ValueError('duplicate capture header')
                header = fields(line)
            elif line.startswith('telemetry available='):
                if telemetry is not None:
                    raise ValueError('duplicate telemetry header')
                telemetry = fields(line)
            elif line.startswith('telemetryStorage '):
                if storage is not None:
                    raise ValueError('duplicate storage footer')
                storage = fields(line)
            elif line.startswith('telemetryEvent '):
                records += 1
    if header is None or header.get('status') not in ('complete', 'complete-partial', 'interrupted') or telemetry is None:
        raise ValueError('missing or unfinished report')
    if int(telemetry['available']) != 1 or int(telemetry['active']) != 0:
        raise ValueError('report telemetry is unavailable or still active')
    if header['captureId'] != telemetry['captureId'] or int(header['captureId']) < 1:
        raise ValueError('report capture identity mismatch')
    if candidate:
        terminal = fields(last_line)
        if not last_line.startswith('reportEnd ') or terminal.get('completed') != '1' or terminal.get('captureId') != header['captureId']:
            raise ValueError('report finalization record is missing or mismatched')
    else:
        if not last_line.startswith(('contextSummary ', 'contextCounts ', 'contextRange ', 'error=', 'reportEnd ')):
            raise ValueError('legacy report terminal record is missing')
        if last_line.startswith(('contextCounts ', 'contextRange ')) and not context_summary:
            raise ValueError('legacy context summary is missing')
        if last_line.startswith('contextRange ') and not context_counts:
            raise ValueError('legacy context counts are missing')
    if records != row['events'] or records != int(telemetry['recorded']):
        raise ValueError('report event records differ from claimed counts')
    expected_complete = candidate or row['mode'] != 'high'
    if expected_complete:
        if int(header['unavailableMask']) & ~allowed_unavailable or telemetry.get('error') != '""':
            raise ValueError('report contains an unavailable/error marker')
        for key in ('backendDropped', 'hostDropped', 'invalidCaptureEvents', 'inFlight',
                    'inFlightAtStop', 'lateDroppedAtRead', 'remaining'):
            if int(telemetry[key]) != 0:
                raise ValueError(f'incomplete report: {key}')
        if candidate and storage is None:
            raise ValueError('candidate report lacks final storage footer')
        if storage is not None:
            if int(storage['complete']) != 1 or int(storage['errors'], 0) != 0:
                raise ValueError('storage footer is incomplete')
            if int(storage['omitted']) != 0 or int(storage['unread']) != 0:
                raise ValueError('storage events were omitted or not read')
            if any(int(storage[key]) != records for key in ('received', 'accepted', 'written', 'reported')):
                raise ValueError('lossless storage counters do not conserve records')
    return {'reportSha256': digest(path), 'verifiedReportEvents': records,
            'verifiedReportStatus': header['status'], 'verifiedTerminalRecord': last_line,
            'verifiedStorageComplete': int(storage['complete']) == 1 if storage else None}


def summarize(rows, kind):
    metrics = RUNTIME_METRICS if kind == 'runtime' else MAPPING_METRICS
    grouped = {}
    for row in rows:
        key = (row['suite'], row['case'], row['pair'])
        pair = grouped.setdefault(key, {})
        if row['label'] in pair:
            raise ValueError(f'duplicate sample: {key}/{row["label"]}')
        pair[row['label']] = row
    paired, summary = [], []
    for (suite, case, index), pair in sorted(grouped.items()):
        if set(pair) != {'A', 'B'}:
            raise ValueError(f'unpaired sample: {suite}/{case}/{index}')
        a, b = pair['A'], pair['B']
        count = 'operations' if kind == 'runtime' else 'iterations'
        if a[count] != b[count]:
            raise ValueError('A/B generated different work counts')
        if kind == 'mapping' and a['budgetMiB'] != b['budgetMiB']:
            raise ValueError('A/B used different mapping budgets')
        for metric in metrics:
            av, bv = finite_number(a, metric), finite_number(b, metric)
            paired.append({'suite': suite, 'case': case, 'pair': index, 'metric': metric,
                           'a': av, 'b': bv, 'delta': bv-av,
                           'changePercent': 100*(bv/av-1) if av else None})
    keys = sorted({(p['suite'], p['case'], p['metric']) for p in paired})
    for suite, case, metric in keys:
        values = [p for p in paired if (p['suite'], p['case'], p['metric']) == (suite, case, metric)]
        changes = [p['changePercent'] for p in values if p['changePercent'] is not None]
        summary.append({'suite': suite, 'case': case, 'metric': metric, 'pairs': len(values),
                        'aMedian': statistics.median(p['a'] for p in values),
                        'bMedian': statistics.median(p['b'] for p in values),
                        'deltaMedian': statistics.median(p['delta'] for p in values),
                        'changeMedianPercent': statistics.median(changes) if len(changes) == len(values) else None,
                        'changeMinPercent': min(changes) if changes else None,
                        'changeMaxPercent': max(changes) if changes else None,
                        'fasterPairs': sum(p['b'] < p['a'] for p in values),
                        'zeroAPairs': sum(p['a'] == 0 for p in values),
                        'zeroBPairs': sum(p['b'] == 0 for p in values)})
    return {'kind': kind, 'rows': len(rows), 'summary': summary, 'paired': paired,
            'interpretation': 'Sequential fresh-process fixture version comparison; A/A measures noise. '
                              'Totals include final GPU drain. Report writing is reported separately. '
                              'Quantiles are per-process workload samples; no game FPS claim.'}


def run(args):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    paths = {name: getattr(args, name).resolve() for name in ('a_probe', 'b_probe', 'a_backend', 'b_backend')}
    binaries = {name: {'path': str(path), 'size': path.stat().st_size, 'sha256': digest(path)}
                for name, path in paths.items()}
    if args.kind == 'runtime' and binaries['a_backend']['sha256'] != binaries['b_backend']['sha256']:
        raise ValueError('runtime storage comparison must use the same backend')
    if args.kind == 'mapping' and binaries['a_probe']['sha256'] != binaries['b_probe']['sha256']:
        raise ValueError('mapping comparison must use the same fixture executable')
    cases = args.cases or (['inactive', 'normal', 'high'] if args.kind == 'runtime'
                            else ['below', 'hot', 'cold', 'over', 'create', 'unused'])
    metadata = {'kind': args.kind, 'binaries': binaries, 'cases': cases, 'pairs': args.pairs,
                'aaPairs': args.aa_pairs, 'budgetMiB': args.budget, 'iterations': args.iterations,
                'startedUtc': datetime.now(timezone.utc).isoformat(), 'status': 'running',
                'ordering': 'alternating AB/BA, rotating cases; interleaved baseline A/A',
                'scheduling': 'sequential processes; runner does not change CPU affinity or priority'}
    write_json(output / 'metadata.json', metadata)
    rows = []
    try:
        for index in range(max(args.pairs, args.aa_pairs)):
            suites = ['ab', 'aa'] if index % 2 == 0 else ['aa', 'ab']
            for suite in suites:
                if index >= (args.pairs if suite == 'ab' else args.aa_pairs):
                    continue
                order = 'AB' if index % 2 == 0 else 'BA'
                for shift in range(len(cases)):
                    case = cases[(shift+index) % len(cases)]
                    for label in order:
                        candidate = suite == 'ab' and label == 'B'
                        variant = 'b' if candidate else 'a'
                        name = f'{suite}-{index:02}-{case}-{label}'
                        working = output / name
                        working.mkdir()
                        command = [str(paths[variant+'_probe'])]
                        if args.kind == 'runtime':
                            command += ['--event-stream-benchmark', str(paths[variant+'_backend']), case]
                        else:
                            command += [str(paths[variant+'_backend']), 'bench', str(args.budget), case, str(args.iterations)]
                        env = os.environ.copy()
                        env.update({'DXVK_LOG_PATH': str(working), 'TEMP': str(working), 'TMP': str(working)})
                        write_json(working / 'command.json', command)
                        with (working/'stdout.log').open('w', encoding='utf-8') as stdout, (working/'stderr.log').open('w', encoding='utf-8') as stderr:
                            completed = subprocess.run(command, cwd=working, env=env, stdout=stdout, stderr=stderr,
                                                       timeout=args.timeout, check=False)
                        if completed.returncode:
                            raise RuntimeError(f'{name} exited {completed.returncode}; see raw logs')
                        parsed = []
                        for line in (working/'stdout.log').read_text(encoding='utf-8', errors='replace').splitlines():
                            if line.startswith('{'):
                                parsed.append(json.loads(line))
                        expected = 'event-stream-benchmark' if args.kind == 'runtime' else 'mapping-budget-benchmark'
                        selected = [row for row in parsed if row.get('type') == expected]
                        if len(selected) != 1:
                            raise ValueError(f'{name}: expected one {expected} row, got {len(selected)}')
                        row = selected[0]
                        validate_row(row, args.kind, case, candidate)
                        if args.kind == 'mapping' and (row['iterations'] != args.iterations or row['budgetMiB'] != args.budget):
                            raise ValueError('mapping fixture did not use the requested work count and budget')
                        if args.kind == 'runtime' and case != 'inactive':
                            report = Path(row['reportPath'])
                            if not report.is_absolute():
                                report = working / report
                            row['reportPath'] = str(report.resolve())
                            row.update(validate_runtime_report(report, row, candidate))
                        row.update({'suite': suite, 'case': case, 'pair': index, 'label': label,
                                    'order': order, 'candidate': candidate, 'directory': str(working)})
                        rows.append(row)
                        with (output/'samples.jsonl').open('a', encoding='utf-8') as stream:
                            stream.write(json.dumps(row, ensure_ascii=False) + '\n')
                        print(f'{name}: complete', flush=True)
        for name, path in paths.items():
            if digest(path) != binaries[name]['sha256']:
                raise RuntimeError(f'binary changed during benchmark: {path}')
        write_json(output/'summary.json', summarize(rows, args.kind))
        metadata['status'] = 'passed'
    except BaseException as error:
        metadata['status'], metadata['error'] = 'failed', str(error)
        raise
    finally:
        metadata['rows'] = len(rows)
        metadata['endedUtc'] = datetime.now(timezone.utc).isoformat()
        write_json(output/'metadata.json', metadata)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kind', required=True, choices=('runtime', 'mapping'))
    for option in ('a-probe', 'b-probe', 'a-backend', 'b-backend', 'output'):
        parser.add_argument('--'+option, required=True, type=Path)
    parser.add_argument('--pairs', type=int, default=6)
    parser.add_argument('--aa-pairs', type=int, default=3)
    parser.add_argument('--cases', nargs='+')
    parser.add_argument('--budget', type=int, default=100)
    parser.add_argument('--iterations', type=int, default=512)
    parser.add_argument('--timeout', type=int, default=300)
    parsed = parser.parse_args()
    if not (1 <= parsed.pairs <= 30 and 1 <= parsed.aa_pairs <= 30):
        parser.error('pair counts must be between 1 and 30')
    run(parsed)
