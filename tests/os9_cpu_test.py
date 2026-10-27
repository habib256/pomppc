#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""Profils OS 9 : témoin inchangé, dépendances et refus des sondes incomplètes."""
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
script = '''
qemu_cpu_has_prop() { [ "$4" != "${MISSING:-}" ]; }
qemu_tcg_has_prop() { [ "$3" != "${MISSING:-}" ]; }
QEMU_BIN=qemu MACHINE=mac99
source "$PROFILE_SCRIPT"
printf '%s\n%s\n' "$CPU_SPEC" "$TCG_ACCEL"
'''

def run(profile=None, missing=''):
    env = {k:v for k,v in os.environ.items() if k != 'OS9_PROFILE'}
    env.update(PROFILE_SCRIPT=str(ROOT / 'scripts/os9_cpu.sh'), MISSING=missing)
    if profile is not None:
        env['OS9_PROFILE'] = profile
    return subprocess.run(['bash','-eu','-c',script], env=env, capture_output=True, text=True)

assert run().stdout == 'g4\ntcg\n'
previous = set()
for profile in ('reference','memoire','flottant','complet','verifie'):
    p = run(profile)
    assert p.returncode == 0, p
    cpu,tcg = p.stdout.splitlines()
    props = set(cpu.split(',')[1:])
    assert previous <= props, (profile, previous - props)
    assert 'x-tier2' not in cpu and 'split-wx' not in tcg
    assert tcg.split(',')[0] == 'tcg' and 'thread=' not in tcg
    previous = props
assert 'x-fast-fp=on' not in run('memoire').stdout
assert 'x-fast-fp=on' in run('flottant').stdout
assert 'x-jc-bits=14' in run('complet').stdout
assert 'x-sr-tlb-verify=64' in run('verifie').stdout
assert run('typo').returncode == 2
assert run('memoire', 'x-tlb-precise=on').returncode == 2
assert run('complet', 'x-jc-word=on').returncode == 2
assert 'cpu "$CPU_SPEC"' in (ROOT / 'run_os9.sh').read_text()
assert '-smp 1' in (ROOT / 'run_os9.sh').read_text()
print('OS 9 : profils cumulatifs, témoin, dépendances et sondes strictes OK')
