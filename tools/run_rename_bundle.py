#!/usr/bin/env python3
"""Check the two-wide rename planner against a sequential instruction model."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT/'out/rename_bundle'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--suite', type=Path, default=Path.home()/'tools/oss-cad-suite-20260905/oss-cad-suite')
    args = parser.parse_args()
    config = json.loads((ROOT/'config/rename_bundle.json').read_text())
    OUT.mkdir(parents=True, exist_ok=True)
    receipt = OUT/'receipt.json'
    receipt.unlink(missing_ok=True)
    paths = ['rtl/backend/rename_bundle.sv', 'verif/unit/rename_bundle_tb.cpp', 'config/rename_bundle.json',
             'tools/run_rename_bundle.py', 'config/toolchain.lock', 'config/synthesis.lock']
    inputs = {p: digest(ROOT/p) for p in paths}
    commands = []

    def run(command, name, failure=False):
        result = subprocess.run([str(p) for p in command], cwd=ROOT, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=180, env=dict(os.environ, CCACHE_DISABLE='1'))
        (OUT/name).write_text(result.stdout)
        commands.append(dict(command=[str(p) for p in command], returncode=result.returncode, log=str((OUT/name).relative_to(ROOT))))
        if (failure and (result.returncode == 0 or 'rename bundle mismatch' not in result.stdout)) or (not failure and result.returncode != 0):
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

    def build(rtl, directory, log):
        run(['verilator', '--cc', '--exe', '--build', '--build-jobs', '2', '--Wall', '--assert',
             '--top-module', 'rename_bundle', '--Mdir', directory, '-CFLAGS', '-std=c++17 -Wall -Wextra -Werror',
             rtl, ROOT/'verif/unit/rename_bundle_tb.cpp', '-o', 'rename_bundle_check'], log)
        return directory/'rename_bundle_check'

    rtl = ROOT/'rtl/backend/rename_bundle.sv'
    run(['verilator', '--lint-only', '--Wall', rtl], 'lint.log')
    binary = build(rtl, OUT/'obj_dir', 'build.log')
    results = []
    for seed in config['seeds']:
        output = run([binary, str(seed), str(config['random_cases_per_seed'])], f'seed_{seed}.log')
        match = re.fullmatch(r'RENAME BUNDLE PASS seed=(\d+) cases=(\d+) raw=(\d+) waw=(\d+) blocked=(\d+) cut=(\d+) zero=(\d+)\n', output)
        if not match or int(match[1]) != seed or int(match[2]) != 16264+config['random_cases_per_seed'] or not all(int(match[n]) > 0 for n in range(3, 8)):
            raise RuntimeError('missing completion or coverage')
        results.append(dict(zip(('seed', 'cases', 'raw', 'waw', 'blocked', 'cut', 'zero'), map(int, match.groups()))))
        print(output.strip(), flush=True)

    mutations = {
        'raw': ('source1[1] = destination[0];', 'source1[1] = rat_i[rs1_i[5 +: 5]*6 +: 6];'),
        'waw': ('stale[1] = destination[0];', 'stale[1] = rat_i[rd_i[5 +: 5]*6 +: 6];'),
        'duplicate_tag': ('available[destination[lane]] = 0;', 'available[0] = 0;'),
        'partial_resources': ('&& resources_ready_i && enough', '&& resources_ready_i && (enough || valid_i != 2\'b10)'),
        'branch_cut': ('if (checkpoint_i[0]) selected[1] = 0;', 'if (checkpoint_i[0]) selected[1] = valid_i[1];'),
        'raw_ready': ('operand_ready[1][0] = 0;', 'operand_ready[1][0] = ready_i[source1[1]];'),
    }
    original = rtl.read_text()
    for name, (old, new) in mutations.items():
        if original.count(old) != 1:
            raise RuntimeError(f'mutation no longer matches: {name}')
        directory = OUT/'mutations'/name
        directory.mkdir(parents=True, exist_ok=True)
        changed = directory/rtl.name
        changed.write_text(original.replace(old, new))
        executable = build(changed, directory/'obj_dir', name+'_build.log')
        run([executable, '1', '0'], name+'.log', failure=True)
        print(f'Rename bundle mutation {name}: PASS (detected)', flush=True)

    script = OUT/'synth.ys'
    script.write_text(f'plugin -i slang\nread_slang --top rename_bundle {rtl}\nsynth -top rename_bundle\ncheck -assert\nwrite_json {OUT/"synth.json"}\n')
    run([suite/'bin/yosys', '-s', script], 'synth.log')
    cells = json.loads((OUT/'synth.json').read_text())['modules']['rename_bundle']['cells']
    counts = {}
    for cell in cells.values():
        kind = cell['type']
        counts[kind] = counts.get(kind, 0)+1
    if not counts or any('DFF' in kind.upper() or 'LATCH' in kind.upper() or not kind.startswith('$_') for kind in counts):
        raise RuntimeError('unexpected state or unmapped synthesis cell')
    if any(digest(ROOT/p) != value for p, value in inputs.items()):
        raise RuntimeError('input changed during check')
    artifacts = {str(p.relative_to(ROOT)): digest(p) for p in OUT.rglob('*') if p.is_file() and 'obj_dir' not in p.parts}
    result = dict(schema=1, profile=config['profile'], status='pass', scope=config['scope'], inputs_sha256=inputs,
                  verilator=version, simulations=results, mutations_detected=list(mutations), synthesis_cells=counts,
                  commands=commands, artifacts_sha256=artifacts, two_wide_core_accepted=False)
    receipt.write_text(json.dumps(result, indent=2)+'\n')
    print(f'Rename bundle: PASS; {sum(counts.values())} combinational generic cells; receipt {receipt.relative_to(ROOT)}', flush=True)


if __name__ == '__main__':
    main()
