#!/usr/bin/env python3
"""Exercise the original AMKE menus and four-controller Vs./Battle gameplay."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--runner', required=True, type=Path)
p.add_argument('--rom', required=True, type=Path)
p.add_argument('--bios', required=True, type=Path)
p.add_argument('--results', required=True, type=Path)
a = p.parse_args()
for path, expected in [(a.rom, '9d327c030c3e2d9007990518594f70c3340ac56f'),
                       (a.bios, '300c20df6731a33952ded8c436f7f186d25d3492')]:
    if hashlib.sha1(path.read_bytes()).hexdigest() != expected:
        p.error(f'Wrong stock ROM/BIOS hash: {path}')
a.results.mkdir(parents=True, exist_ok=True)
for mode, flag in [('vs', 0x400), ('battle', 0x800)]:
    inputs = Path(__file__).parent / 'inputs' / f'stock-{mode}.csv'
    proc = subprocess.run([str(a.runner), '--rom', str(a.rom), '--bios', str(a.bios),
                           '--players', '4', '--frames', '4700', '--input', str(inputs),
                           '--screens', str(a.results / mode)],
                          capture_output=True, text=True, timeout=180)
    (a.results / f'{mode}.json').write_text(proc.stdout)
    (a.results / f'{mode}.stderr').write_text(proc.stderr)
    if proc.returncode:
        raise RuntimeError(f'{mode}: runner exited {proc.returncode}: {proc.stderr}')
    result = json.loads(proc.stdout)
    positions = []
    for node in result['nodes']:
        game = node['game']
        assert game['sceneMain'] == 0x0805167D, (mode, node)
        assert game['humanCount'] == 4 and game['localId'] == node['slot'], (mode, node)
        assert game['raceFlags'] & flag, (mode, node)
        assert node['menu']['heldKeys'] == [1, 17, 0, 33], (mode, node)
        racers = game['drivers'][:4]
        assert all(r['address'] for r in racers), (mode, node)
        positions.append([r['position'] for r in racers])
    assert all(state == positions[0] for state in positions), (mode, positions)
    assert len({tuple(pos) for pos in positions[0]}) == 4, (mode, positions)
    print(f'{mode}: four local IDs, distinct inputs, matching four-racer positions', flush=True)
print('PASS: original four-player Vs. and Battle menu/gameplay baseline', flush=True)
