#!/usr/bin/env python3
"""Check the stateful two-wide rename against an independent instruction ledger."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT/'out/rename_state'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--suite', type=Path, default=Path.home()/'tools/oss-cad-suite-20260905/oss-cad-suite')
    args = parser.parse_args()
    config = json.loads((ROOT/'config/rename_state.json').read_text())
    OUT.mkdir(parents=True, exist_ok=True)
    receipt = OUT/'receipt.json'
    receipt.unlink(missing_ok=True)
    paths = ['rtl/backend/rename_bundle.sv', 'rtl/backend/rename_state.sv', 'verif/unit/rename_state_tb.cpp', 'config/rename_state.json',
             'tools/run_rename_state.py', 'config/toolchain.lock', 'config/synthesis.lock']
    inputs = {p: digest(ROOT/p) for p in paths}
    commands = []

    def run(command, name, failure=False):
        result = subprocess.run([str(p) for p in command], cwd=ROOT, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=300, env=dict(os.environ, CCACHE_DISABLE='1'))
        (OUT/name).write_text(result.stdout)
        commands.append(dict(command=[str(p) for p in command], returncode=result.returncode, log=str((OUT/name).relative_to(ROOT))))
        if (failure and (result.returncode == 0 or (failure if isinstance(failure, str) else 'rename state mismatch') not in result.stdout)) or (not failure and result.returncode != 0):
            raise RuntimeError(f'check failed; see {OUT/name}\n{result.stdout[-2500:]}')
        return result.stdout

    tools = json.loads((ROOT/'config/toolchain.lock').read_text())['tools']
    version = run(['verilator', '--version'], 'version.log').strip()
    if version != next(t['expected_first_line'] for t in tools if t['name'] == 'verilator'):
        raise RuntimeError('Verilator version differs')
    lock = json.loads((ROOT/'config/synthesis.lock').read_text())
    suite = args.suite.resolve()
    for p, value in lock['sha256'].items():
        if digest(suite/p) != value:
            raise RuntimeError(f'synthesis tool pin differs: {p}')

    def build(rtl, directory, log, mutated=False):
        run(['verilator', '--cc', '--exe', '--build', '--build-jobs', '2', '--Wall', '--assert',
             *(['-DSYNTHESIS'] if mutated else []), '--top-module', 'rename_state', '--Mdir', directory, '-CFLAGS', '-std=c++17 -Wall -Wextra -Werror',
             rtl, ROOT/'rtl/backend/rename_bundle.sv', ROOT/'verif/unit/rename_state_tb.cpp', '-o', 'rename_state_check'], log)
        return directory/'rename_state_check'

    rtl = ROOT/'rtl/backend/rename_state.sv'
    run(['verilator', '--lint-only', '--Wall', '--assert', '--top-module', 'rename_state', rtl, ROOT/'rtl/backend/rename_bundle.sv'], 'lint.log')
    binary = build(rtl, OUT/'obj_dir', 'build.log')
    results = []
    for seed in config['seeds']:
        output = run([binary, str(seed), str(config['random_cycles_per_seed'])], f'seed_{seed}.log')
        if not output.startswith('RENAME STATE PASS ') or len(output.splitlines()) != 1:
            raise RuntimeError('missing simulation completion')
        fields = dict((key, int(value)) for key, value in re.findall(r'(\w+)=(\d+)', output))
        required = {'wb_lane1', 'wb_zero', 'dual_rename', 'rename_waw', 'zero_destination', 'checkpoint_cut', 'exhausted',
                    'commit_rename', 'deferred_reuse', 'recover_collision', 'reset_live', 'recovery',
                    'commit_waw', 'ooo_wb', 'blocked_wb', 'wb_commit', 'wb_bypass', 'tag_reuse'}
        if fields.get('seed') != seed or fields.get('cycles') != config['directed_cycles'] + config['random_cycles_per_seed'] or not all(fields.get(k, 0) > 0 for k in required):
            raise RuntimeError('missing completion or coverage')
        results.append(fields)
        print(output.strip(), flush=True)

    negative_cases = {
        'free_wb': 'RENAME_STATE_WB_OWNER',
        'duplicate_wb': 'RENAME_STATE_DUPLICATE_WB',
        'unready_commit': 'RENAME_STATE_COMMIT_OWNER',
        'commit_prefix': 'RENAME_STATE_COMMIT_PREFIX',
        'wrong_stale': 'RENAME_STATE_COMMIT_OWNER',
        'zero_metadata': 'RENAME_STATE_ZERO_COMMIT',
    }
    for name, marker in negative_cases.items():
        output = run([binary, 'negative', name], 'negative_'+name+'.log', failure=marker)
        print(f'Rename state caller assertion {name}: PASS (rejected)', flush=True)

    mutations = {
        'allocation_ready': ('ready_d[destination_o[lane*6 +: 6]] = 0;', 'ready_d[destination_o[lane*6 +: 6]] = 1;'),
        'allocation_ownership': ('free_d[destination_o[lane*6 +: 6]] = 0;', 'free_d[destination_o[lane*6 +: 6]] = 1;'),
        'wrong_release': ('free_d[commit_stale_i[lane*6 +: 6]] = 1;', 'free_d[commit_destination_i[lane*6 +: 6]] = 1;'),
        'commit_lane_order': ('committed_d[commit_rd_i[lane*5 +: 5]*6 +: 6] = commit_destination_i[lane*6 +: 6];',
                              'if (lane == 0) committed_d[commit_rd_i[lane*5 +: 5]*6 +: 6] = commit_destination_i[lane*6 +: 6];'),
        'recovery_map': ('rat_d = committed_q;', 'rat_d = rat_q;'),
        'recovery_commit_leak': ('committed_d = committed_q;\n      free_d = recovered_free;',
                                 'committed_d = commit_i[0] ? rat_q : committed_q;\n      free_d = recovered_free;'),
        'wb_bypass': ('.ready_i(ready_view)', '.ready_i(ready_q)'),
        'retire_clobbers_speculative': ('free_d[commit_stale_i[lane*6 +: 6]] = 1;',
                                       'free_d[commit_stale_i[lane*6 +: 6]] = 1; rat_d = committed_d;'),
    }
    original = rtl.read_text()
    for name, (old, new) in mutations.items():
        if original.count(old) != 1:
            raise RuntimeError(f'mutation no longer matches: {name}')
        directory = OUT/'mutations'/name
        directory.mkdir(parents=True, exist_ok=True)
        changed = directory/rtl.name
        changed.write_text(original.replace(old, new))
        executable = build(changed, directory/'obj_dir', name+'_build.log', mutated=True)
        run([executable, '1', '0'], name+'.log', failure=True)
        print(f'Rename state mutation {name}: PASS (detected)', flush=True)

    script = OUT/'synth.ys'
    script.write_text(f'plugin -i slang\nread_slang --top rename_state {rtl} {ROOT/"rtl/backend/rename_bundle.sv"}\nsynth -top rename_state -flatten\ncheck -assert\nwrite_json {OUT/"synth.json"}\n')
    run([suite/'bin/yosys', '-s', script], 'synth.log')
    modules = json.loads((OUT/'synth.json').read_text())['modules']
    cells = {module+'__'+name: cell for module, data in modules.items() for name, cell in data['cells'].items()}
    counts = {}
    for cell in cells.values():
        kind = cell['type']
        counts[kind] = counts.get(kind, 0)+1
    if not counts or not any('DFF' in kind.upper() for kind in counts) or any('LATCH' in kind.upper() or (not kind.startswith('$_') and kind not in modules) for kind in counts):
        raise RuntimeError('missing state, latch or unmapped synthesis cell')
    if any(digest(ROOT/p) != value for p, value in inputs.items()):
        raise RuntimeError('input changed during check')
    artifacts = {str(p.relative_to(ROOT)): digest(p) for p in OUT.rglob('*') if p.is_file() and 'obj_dir' not in p.parts}
    result = dict(schema=1, profile=config['profile'], status='pass', scope=config['scope'], inputs_sha256=inputs,
                  verilator=version, simulations=results, mutations_detected=list(mutations), caller_assertions_checked=list(negative_cases), synthesis_cells=counts,
                  commands=commands, artifacts_sha256=artifacts, two_wide_core_accepted=False)
    receipt.write_text(json.dumps(result, indent=2)+'\n')
    print(f'Rename state: PASS; {sum(counts.values())} generic cells (including state); receipt {receipt.relative_to(ROOT)}', flush=True)


if __name__ == '__main__':
    main()
