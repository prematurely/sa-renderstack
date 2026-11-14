"""Correlate recorded CS waits with CS chunks and driver spans, without adding nested time.

Intervals are wall-clock observations. Missing/filtered spans stay unobserved;
driver time may include CPU work, scheduling and driver-internal waits.
"""
import argparse
import bisect
import hashlib
import json
from pathlib import Path
import re

HIGH_PRIORITY = 1 << 11
DRIVER_KINDS = {14: 'command-allocate', 15: 'command-begin', 16: 'command-reset'}


def duration_union(intervals):
    end = total = 0
    for lo, hi in sorted(intervals):
        total += max(0, hi-max(lo, end))
        end = max(end, hi)
    return total


class Intervals:
    def __init__(self, rows):
        self.rows = sorted(rows, key=lambda e: e['beginNs'])
        self.starts = [e['beginNs'] for e in self.rows]
        self.ends = []
        end = 0
        for e in self.rows:
            end = max(end, e['endNs'])
            self.ends.append(end)

    def overlap(self, lo, hi):
        first = bisect.bisect_right(self.ends, lo)
        last = bisect.bisect_left(self.starts, hi)
        return (e for e in self.rows[first:last] if e['endNs'] > lo)


def analyze(events, minimum_ns=0):
    chunks = Intervals(e for e in events if e['kind'] == 13)
    drivers = Intervals(e for e in events if e['kind'] in DRIVER_KINDS)
    result = []
    for wait in events:
        if wait['kind'] != 5 or wait['endNs']-wait['beginNs'] < minimum_ns:
            continue
        lo, hi = wait['beginNs'], wait['endNs']
        work, high, driver, target = [], [], [], False
        driver_kinds = {name: [] for name in DRIVER_KINDS.values()}
        sequence_rows = []
        for chunk in chunks.overlap(lo, hi):
            if chunk.get('captureId') != wait.get('captureId') or chunk['thread'] == wait['thread']:
                continue
            same_queue = bool(chunk['flags'] & HIGH_PRIORITY) == bool(wait['flags'] & HIGH_PRIORITY)
            if same_queue and chunk['identity'] > wait['identity']:
                continue
            a, b = max(lo, chunk['beginNs']), min(hi, chunk['endNs'])
            work.append((a, b))
            if chunk['flags'] & HIGH_PRIORITY:
                high.append((a, b))
            target |= same_queue and chunk['identity'] == wait['identity'] and bool(wait['identity'])
            sequence_rows.append(dict(sequence=chunk['identity'], highPriority=bool(chunk['flags'] & HIGH_PRIORITY), thread=chunk['thread'], overlapNs=b-a))
            for call in drivers.overlap(a, b):
                if call.get('captureId') != wait.get('captureId') or call['thread'] != chunk['thread']:
                    continue
                span = max(a, call['beginNs']), min(b, call['endNs'])
                driver.append(span)
                driver_kinds[DRIVER_KINDS[call['kind']]].append(span)
        observed = duration_union(work)
        result.append(dict(beginNs=lo, endNs=hi, durationNs=hi-lo, thread=wait['thread'],
                           captureId=wait.get('captureId'), targetSequence=wait['identity'],
                           highPriority=bool(wait['flags'] & HIGH_PRIORITY), targetObserved=target,
                           csObservedNs=observed, highPriorityNs=duration_union(high),
                           driverWithinCsNs=duration_union(driver),
                           driverKindsNs={k: duration_union(v) for k, v in driver_kinds.items()},
                           unobservedNs=hi-lo-observed, chunks=sequence_rows))
    return sorted(result, key=lambda e: e['durationNs'], reverse=True)


def read_report(path):
    events, frames, metadata = [], [], {}
    fields = re.compile(r'(\w+)=([^\s]+)')
    with path.open(encoding='utf-8-sig', errors='strict') as stream:
        for line in stream:
            if line.startswith('telemetryEvent '):
                row = dict(fields.findall(line))
                event = {k: int(row[k], 0) for k in ('kind', 'beginNs', 'endNs', 'identity', 'flags', 'captureId')}
                event['thread'] = int(row.get('thread', row.get('threadId')), 0)
                if event['endNs'] < event['beginNs']:
                    raise ValueError('reversed telemetry interval')
                events.append(event)
            elif line.startswith('frame '):
                row = dict(fields.findall(line))
                frames.append({k: int(row[k], 0) for k in ('id', 'beginNs', 'endNs')})
            elif line.startswith(('telemetry available=', 'telemetryStorage ', 'reportEnd ')):
                metadata[line.split()[0]] = dict(fields.findall(line))
    return events, frames, metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--minimum-ms', type=float, default=1)
    args = parser.parse_args()
    events, frames, metadata = read_report(args.report)
    rows = analyze(events, int(args.minimum_ms*1e6))
    frame_index = Intervals(frames)
    for row in rows:
        row['frames'] = [f['id'] for f in frame_index.overlap(row['beginNs'], row['endNs'])]
    with args.report.open('rb') as stream:
        sha = hashlib.file_digest(stream, 'sha256').hexdigest()
    output = dict(source=str(args.report.resolve()), sha256=sha,
                  semantics='clipped wall-clock unions; nested spans are not additive; missing spans remain unobserved',
                  metadata=metadata, events=len(events), waits=rows)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(output, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(dict(events=len(events), waits=len(rows), output=str(args.output))))


if __name__ == '__main__':
    main()
