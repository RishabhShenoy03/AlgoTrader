"""Offline integration test: only synthetic bars, never opens a broker connection."""
import csv
from datetime import date, timedelta
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile

binary, root = sys.argv[1:]
with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    data = tmp / 'bars.csv'
    with data.open('w', newline='') as f:
        out = csv.writer(f)
        out.writerow(['date', 'ticker', 'open', 'close'])
        day = date(2011, 1, 3)
        for i in range(1200):
            if day.weekday() < 5:
                out.writerow([str(day), 1, 100 + i / 100 + 5 * math.sin(i / 30),
                              100 + i / 100 + 5 * math.sin((i + 1) / 30)])
            day += timedelta(days=1)
    output = tmp / 'research'
    cmd = [sys.executable, str(Path(root) / 'scripts/research.py'), '--binary', binary,
           '--instruments', str(Path(root) / 'examples/ibkr-aapl.csv'), '--data', str(data),
           '--source', 'SYNTHETIC TEST ONLY', '--output', str(output)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    rows = list(csv.DictReader((output / 'comparison.csv').open()))
    assert len(rows) == 8
    assert not any('holdout' in r['run'] or 'validate' in r['run'] for r in rows)
    for row in rows:
        assert math.isfinite(float(row['net_pnl']))
    manifest = json.loads((output / 'manifest.json').read_text())
    assert manifest['stage'] == 'develop' and len(manifest['commands']) == 8
    assert all(command[1] == 'backtest' for command in manifest['commands'])
    old = (output / 'manifest.json').read_bytes()
    assert subprocess.run(cmd, capture_output=True).returncode != 0
    assert (output / 'manifest.json').read_bytes() == old, 'must not overwrite results'
    assert subprocess.run(cmd + ['--stage', 'holdout'], capture_output=True).returncode != 0
    # Deliberately insert future-date ordering error; refuse before launching the engine.
    with data.open('a') as f:
        f.write('2011-01-03,1,100,101\n')
    assert subprocess.run(cmd + ['--output', str(tmp/'bad')], capture_output=True).returncode != 0
    assert not (tmp / 'bad').exists()
print('Research runner: isolated offline stages, 8 runs, immutable output, validation and data ordering passed')
