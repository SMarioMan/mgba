#!/usr/bin/env python3
"""Run the actual homebrew on BIOS-backed cores; never inject player IDs/RAM."""
import argparse
import hashlib
import json
import pathlib
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--runner', required=True, type=pathlib.Path)
p.add_argument('--rom', required=True, type=pathlib.Path)
p.add_argument('--bios', required=True, type=pathlib.Path)
p.add_argument('--results', required=True, type=pathlib.Path)
p.add_argument('--case', choices=['all', 'recovery'], default='all')
a = p.parse_args()
if hashlib.sha1(a.bios.read_bytes()).hexdigest() != '300c20df6731a33952ded8c436f7f186d25d3492':
    p.error('Use the supplied standard GBA BIOS (SHA-1 300c20df6731a33952ded8c436f7f186d25d3492)')
a.results.mkdir(parents=True, exist_ok=True)

def run(name, n, host=0, frames=550, fault=False):
    csv = a.results / f'{name}.csv'
    rows = [(400, host, 8), (405, host, 0)] + [(450, i, 1 << i) for i in range(n)]
    csv.write_text(''.join(f'{frame},{i},{keys:x}\n' for frame, i, keys in rows))
    cmd = [str(a.runner), '--ring', '--rom', str(a.rom), '--bios', str(a.bios),
           '--players', str(n), '--frames', str(frames), '--input', str(csv)]
    if fault:
        cmd += ['--break-frame', '480', '--restore-frame', '510']
    else:
        cmd += ['--expect-poc', '--expect-host', str(host)]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    (a.results / f'{name}.stderr').write_text(proc.stderr)
    (a.results / f'{name}.json').write_text(proc.stdout)
    if proc.returncode:
        raise RuntimeError(f'{name}: runner exited {proc.returncode}: {proc.stderr}')
    result = json.loads(proc.stdout)
    expected = [1 << ((host + i) % n) for i in range(n)] + [0] * (8 - n)
    rounds = []
    for node in result['nodes']:
        status = node['poc']
        assert status['id'] == (node['slot'] + n - host) % n, (name, node)
        if not fault or frames >= 510:
            assert status['count'] == n, (name, node)
        assert status['inputs'] == expected, (name, node)
        rounds.append(status['rounds'])
        if not fault:
            assert status['errors'] == status['timeouts'] == 0, (name, node)
    assert result['missedReceivers'] == result['collisions'] == 0, result
    assert max(rounds) - min(rounds) <= 1, rounds
    host_status = result['nodes'][host]['poc']
    if fault:
        assert host_status['errors'] > 0 and host_status['timeouts'] > 0, result
    else:
        assert min(rounds) >= 100, rounds
        assert 0 < host_status['frameCycles'] < 280896, host_status
    print(f'{name}: {min(rounds)} committed input frames; {host_status["frameCycles"]} CPU clocks/frame', flush=True)
    return min(rounds)

if a.case == 'all':
    for n in range(2, 9):
        run(f'ring-{n}', n)
    run('ring-8-owner-3', 8, host=3)
blocked = run('wire-open', 3, frames=505, fault=True)
recovered = run('wire-restored', 3, frames=650, fault=True)
assert recovered > blocked + 100, (blocked, recovered)
print('PASS: ' + ('2–8 players, independent inputs, nonzero host, ' if a.case == 'all' else '') + 'bounded recovery after reconnect', flush=True)
