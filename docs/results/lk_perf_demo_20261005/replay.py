#!/usr/bin/env python3
"""Replay this archived hardware demo offline; never opens a serial port."""
import argparse
from collections import Counter
import gzip
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--out', type=Path, required=True, help='new output directory')
ap.add_argument('--trace-processor', type=Path, required=True)
ap.add_argument('--flamegraph', type=Path, required=True)
args = ap.parse_args()
archive = Path(__file__).resolve().parent
repo = archive.parents[2]
args.out.mkdir(parents=True, exist_ok=False)
meta = json.loads((archive / 'demo.perf.metadata.json').read_text())
for name, kind in [('capture.log', 'log'), ('lk.elf', 'elf'), ('lk.bin', 'image')]:
    data = gzip.decompress((archive / (name + '.gz')).read_bytes())
    if hashlib.sha256(data).hexdigest() != meta['artifacts'][kind]['sha256']:
        raise SystemExit('Archived identity mismatch: ' + name)
    (args.out / name).write_bytes(data)
perf = args.out / 'demo.perf'
subprocess.run([sys.executable, str(repo / 'scripts/pi4_perf_export.py'),
                str(args.out / 'capture.log'), str(args.out / 'lk.elf'),
                '--image', str(args.out / 'lk.bin'), '--output', str(perf),
                '--mode', 'pmu', '--event', 'cpu-cycles', '--period', '1000000',
                '--run-id', 'pi4-smp-demo-20261005'], check=True)
if perf.read_bytes() != (archive / 'demo.perf').read_bytes():
    raise SystemExit('Export differs from archived perf text')
counts = Counter()
for record in perf.read_text().strip().split('\n\n'):
    frames = [line.split()[1] for line in record.splitlines()[1:]]
    counts[';'.join(reversed(frames))] += 1
folded = args.out / 'demo.folded'
folded.write_text(''.join(f'{stack} {n}\n' for stack, n in sorted(counts.items())))
if folded.read_bytes() != (archive / 'demo.folded').read_bytes():
    raise SystemExit('Folded stacks differ from archived data')
with (args.out / 'flamegraph.svg').open('wb') as svg:
    subprocess.run(['perl', str(args.flamegraph), '--hash', '--title',
                    'lk-perf Pi4: four-core nested workload', '--countname',
                    'samples', '--nametype', 'Function:', str(folded)],
                   stdout=svg, check=True)
for sql in sorted(archive.glob('*.sql')):
    result = subprocess.run([sys.executable, str(args.trace_processor), 'query',
                             '-f', str(sql), str(perf)], text=True,
                            capture_output=True, check=True)
    (args.out / (sql.stem + '.csv')).write_text(result.stdout)
    if result.stdout != sql.with_suffix('.csv').read_text():
        raise SystemExit('Perfetto result differs: ' + sql.name +
                         ' (check importer version and rounding)')
print('PASS: archive hashes, identical perf/folded export, SVG generation, '
      'and all seven Perfetto SQL result sets; no hardware accessed')
