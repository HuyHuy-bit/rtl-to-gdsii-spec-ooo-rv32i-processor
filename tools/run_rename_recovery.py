#!/usr/bin/env python3
"""Check the atomic rename and checkpoint recovery against an instruction ledger."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT/'out/rename_recovery'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--suite', type=Path, default=Path.home()/'tools/oss-cad-suite-20260905/oss-cad-suite')
    args = parser.parse_args()
    config = json.loads((ROOT/'config/rename_recovery.json').read_text())
    OUT.mkdir(parents=True, exist_ok=True)
    receipt = OUT/'receipt.json'
    receipt.unlink(missing_ok=True)
    paths = [*config['sources'], 'verif/unit/rename_recovery_tb.cpp', 'config/rename_recovery.json',
             'config/rename_state.json', 'config/rename_bundle.json', 'config/rename_checkpoints.json',
             'tools/run_rename_recovery.py', 'config/toolchain.lock', 'config/synthesis.lock']
    inputs = {p: digest(ROOT/p) for p in paths}
    commands = []

    def run(command, name, failure=False):
        result = subprocess.run([str(p) for p in command], cwd=ROOT, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=300, env=dict(os.environ, CCACHE_DISABLE='1'))
        (OUT/name).write_text(result.stdout)
        commands.append(dict(command=[str(p) for p in command], returncode=result.returncode, log=str((OUT/name).relative_to(ROOT))))
        if (failure and (result.returncode == 0 or (failure if isinstance(failure, str) else 'rename recovery mismatch') not in result.stdout)) or (not failure and result.returncode != 0):
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

    def build(sources, directory, log, mutated=False):
        run(['verilator', '--cc', '--exe', '--build', '--build-jobs', '2', '--Wall', '--assert',
             *(['-DSYNTHESIS'] if mutated else []), '--top-module', 'rename_recovery', '--Mdir', directory, '-CFLAGS', '-std=c++17 -Wall -Wextra -Werror',
             *sources, ROOT/'verif/unit/rename_recovery_tb.cpp', '-o', 'rename_recovery_check'], log)
        return directory/'rename_recovery_check'

    sources = [ROOT/p for p in config['sources']]
    run(['verilator', '--lint-only', '--Wall', '--assert', '--top-module', 'rename_recovery', *sources], 'lint.log')
    binary = build(sources, OUT/'obj_dir', 'build.log')
    results = []
    for seed in config['seeds']:
        output = run([binary, str(seed), str(config['random_cycles_per_seed'])], f'seed_{seed}.log')
        if not output.startswith('RENAME RECOVERY PASS ') or len(output.splitlines()) != 1:
            raise RuntimeError('missing simulation completion')
        fields = dict((key, int(value)) for key, value in re.findall(r'(\w+)=(\d+)', output))
        required = {'resource_stall', 'physical_stall', 'deferred_tag_reuse', 'flush_collision', 'reject_zero', 'reject_free', 'reject_ready', 'created', 'lane1_cfi', 'cut', 'waw', 'full_stall', 'full_non_cfi', 'recovery', 'nested',
                    'younger_kill', 'commit_suppressed', 'retained_wb', 'killed_wb', 'resolve_create', 'live_reset',
                    'full_flush', 'reject_resolve', 'reject_stale', 'commit_rename'}
        if fields.get('seed') != seed or fields.get('cycles') != config['directed_cycles'] + config['random_cycles_per_seed'] or not all(fields.get(k, 0) > 0 for k in required):
            raise RuntimeError('missing completion or coverage')
        results.append(fields)
        print(output.strip(), flush=True)

    run([binary, '1', '0'], 'directed.log')
    negative_cases = {'commit_prefix': 'RENAME_STATE_COMMIT_PREFIX', 'wrong_stale': 'RENAME_STATE_COMMIT_OWNER'}
    for name, marker in negative_cases.items():
        output = run([binary, 'negative', name], 'negative_'+name+'.log', failure=marker)
        print(f'Rename recovery caller assertion {name}: PASS (rejected)', flush=True)

    mutations = {
        'pre_cfi_snapshot': (0, 'snapshot[rd_i[lane*5 +: 5]*6 +: 6] = destination_o[lane*6 +: 6];',
                            'snapshot[rd_i[lane*5 +: 5]*6 +: 6] = rat_o[rd_i[lane*5 +: 5]*6 +: 6];'),
        'ignore_checkpoint_credit': (0, '(!needs_checkpoint || checkpoint_ready)', '(needs_checkpoint || checkpoint_ready || !needs_checkpoint)'),
        'commit_during_recovery': (0, 'rst_i || recover_i || branch_recover_o ?', 'rst_i || recover_i ?'),
        'accept_killed_writeback': (0, '&& !reclaim_o[wb_destination_i[lane*6 +: 6]]', '&& (reclaim_o[0] == 0)'),
        'drop_surviving_writeback': (0, 'if (!rst_i && !recover_i && wb_offer_i[lane]', 'if (!rst_i && !recover_i && !branch_recover_o && wb_offer_i[lane]'),
        'ignore_wb_identity': (0, 'wb_offer_i[lane] && wb_live_i[lane]', 'wb_offer_i[lane] && (wb_live_i[lane] || wb_offer_i[lane])'),
        'ignore_resolve_identity': (0, 'resolve_i && resolve_live_i &&', 'resolve_i && (resolve_live_i || resolve_i) &&'),
        'replace_current_free': (1, 'free_d = free_q | branch_reclaim_i;', 'free_d = branch_reclaim_i;'),
        'mark_survivors_ready': (1, 'ready_d = ready_view & ~branch_reclaim_i;', 'ready_d = ~free_q & ~branch_reclaim_i;'),
        'restore_committed_instead': (1, 'rat_d = branch_rat_i;', "rat_d = branch_rat_i[5:0] == 0 ? committed_q : branch_rat_i;"),
    }
    for name, (index, old, new) in mutations.items():
        rtl = sources[index]
        original = rtl.read_text()
        if original.count(old) != 1:
            raise RuntimeError(f'mutation no longer matches: {name}')
        directory = OUT/'mutations'/name
        directory.mkdir(parents=True, exist_ok=True)
        changed = directory/rtl.name
        changed.write_text(original.replace(old, new))
        altered = list(sources)
        altered[index] = changed
        executable = build(altered, directory/'obj_dir', name+'_build.log', mutated=True)
        run([executable, '1', '0'], name+'.log', failure=True)
        print(f'Rename recovery mutation {name}: PASS (detected)', flush=True)

    script = OUT/'synth.ys'
    source_list = ' '.join(str(p) for p in sources)
    script.write_text(f'plugin -i slang\nread_slang --top rename_recovery {source_list}\nsynth -top rename_recovery -flatten\ncheck -assert\nwrite_json {OUT/"synth.json"}\n')
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
    print(f'Rename recovery: PASS; {sum(counts.values())} generic cells (including state); receipt {receipt.relative_to(ROOT)}', flush=True)


if __name__ == '__main__':
    main()
