#!/usr/bin/env python3
"""Check full-size issue selection with an independent program-order ledger."""
import argparse
from datetime import datetime, timezone
import hashlib
import io
import json
import os
from pathlib import Path
import re
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT/'out/issue_queue'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--suite', type=Path, default=Path.home()/'tools/oss-cad-suite-20260905/oss-cad-suite')
    args = parser.parse_args()
    config = json.loads((ROOT/'config/issue_queue.json').read_text())
    if (config['entries'], config['rob_entries'], config['identity_bits'], config['payload_bits']) != (16, 32, 13, 64):
        raise RuntimeError('unsupported IQ geometry')
    OUT.mkdir(parents=True, exist_ok=True)
    retained = ROOT/'evidence/receipts/issue_queue.json'
    retained.parent.mkdir(parents=True, exist_ok=True)
    retained.unlink(missing_ok=True)
    receipt = OUT/'receipt.json'
    receipt.unlink(missing_ok=True)
    paths = [*config['sources'], 'verif/unit/issue_queue_tb.cpp', 'config/issue_queue.json',
             'config/backend_two_wide.json', 'config/rob_two_wide.json', 'config/prf_contract.json',
             'tools/run_issue_queue.py', 'config/toolchain.lock', 'config/synthesis.lock', 'Makefile']
    inputs = {p: digest(ROOT/p) for p in paths}
    commands = []
    artifacts = set()

    def run(command, name, failure=None):
        result = subprocess.run([str(p) for p in command], cwd=ROOT, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=600, env=dict(os.environ, CCACHE_DISABLE='1'))
        (OUT/name).write_text(result.stdout)
        artifacts.add(OUT/name)
        commands.append(dict(command=[str(p) for p in command], returncode=result.returncode, log=name))
        if (failure and (result.returncode == 0 or failure not in result.stdout)) or (not failure and result.returncode != 0):
            raise RuntimeError(f'check failed; see {OUT/name}\n{result.stdout[-2500:]}')
        return result.stdout

    version = run(['verilator', '--version'], 'version.log').strip()
    toolchain = json.loads((ROOT/'config/toolchain.lock').read_text())['tools']
    if version != next(t['expected_first_line'] for t in toolchain if t['name'] == 'verilator'):
        raise RuntimeError('Verilator version differs')
    lock = json.loads((ROOT/'config/synthesis.lock').read_text())
    suite = args.suite.resolve()
    for p, value in lock['sha256'].items():
        if digest(suite/p) != value:
            raise RuntimeError(f'synthesis tool pin differs: {p}')
    rtl = ROOT/config['sources'][0]

    def build(source, directory, log, mutated=False):
        run(['verilator', '--cc', '--exe', '--build', '--build-jobs', '2', '--Wall', '--assert',
             *(['-DSYNTHESIS', '-Wno-UNUSEDSIGNAL', '-Wno-UNUSEDPARAM'] if mutated else []),
             '--top-module', 'issue_queue', '--Mdir', directory,
             '-CFLAGS', '-std=c++17 -Wall -Wextra -Werror', source,
             ROOT/'verif/unit/issue_queue_tb.cpp', '-o', 'issue_queue_check'], log)
        return directory/'issue_queue_check'

    run(['verilator', '--lint-only', '--Wall', '--assert', '--top-module', 'issue_queue', rtl], 'lint.log')
    binary = build(rtl, OUT/'obj_dir', 'build.log')
    results = []
    required = {'dual_dispatch', 'dual_issue', 'dispatch_issue', 'full', 'one_credit', 'backpressure',
                'stalled_wakeup', 'wakeup_issue', 'retained_wakeup', 'unaccepted_broadcast', 'dual_wakeup',
                'wrapped_window', 'reset_live', 'flush_live', 'recovery', 'recovery_wrap', 'recovery_wakeup',
                'recovery_killed', 'recovery_survivor', 'dispatch_wb', 'zero_source', 'same_source',
                'port0_only', 'port1_only', 'generation_forwarded'}
    for seed in config['seeds']:
        output = run([binary, str(seed), str(config['random_cycles_per_seed'])], f'seed_{seed}.log')
        if not output.startswith('ISSUE QUEUE PASS ') or len(output.splitlines()) != 1:
            raise RuntimeError('missing simulation completion')
        fields = {key: int(value) for key, value in re.findall(r'(\w+)=(\d+)', output)}
        if (fields.get('seed') != seed or fields.get('directed') != config['directed_cycles']
                or fields.get('cycles') != fields['directed']+config['random_cycles_per_seed']
                or not all(fields.get(k, 0) > 0 for k in required)):
            raise RuntimeError('missing completion or coverage: '+output)
        results.append(fields)
        print(output.strip(), flush=True)

    negatives = {'prefix': 'IQ_DISPATCH', 'overflow': 'IQ_DISPATCH',
                 'eligibility': 'IQ_ELIGIBILITY', 'identity': 'IQ_IDENTITY'}
    for name, marker in negatives.items():
        run([binary, 'negative', name], 'negative_'+name+'.log', failure=marker)
        print(f'IQ caller assertion {name}: PASS (rejected)', flush=True)

    mutations = {
        'raw_broadcast': ('wb_accept_i[lane] &&', "1'b1 &&"),
        'lost_second_wakeup': ('wb_accept_i[lane] &&', 'wb_accept_i[lane] && lane == 0 &&'),
        'lost_wakeup_retention': ('if (valid_q[slot]) ready_q[slot] <= awake[slot];', ''),
        'raw_slot_age': ("5'(id_q[slot][4:0] - head_slot_i)", 'id_q[slot][4:0]'),
        'youngest_first': ('< best_age[port_id]', '> best_age[port_id]'),
        'duplicate_issue': ('(port_id == 0 || !chosen[0][slot])', "1'b1"),
        'ignored_backpressure': ('&& port_ready_i[port_id]', ''),
        'ignored_eligibility': ('&& eligible_q[slot][port_id]', ''),
        'lost_second_dispatch': ('if (!stop && dispatch_i[lane])', 'if (!stop && dispatch_i[lane] && lane == 0)'),
        'lost_recovery_kill': ("valid_q[slot] <= 0;\n        if (chosen", "valid_q[slot] <= valid_q[slot];\n        if (chosen"),
        'unsigned_recovery_cut': ("5'(id_q[slot][4:0] - head_slot_i) > 5'(recover_slot_i - head_slot_i)", 'id_q[slot][4:0] > recover_slot_i'),
        'recovery_launch': ('rst_i || flush_i || recover_i', 'rst_i || flush_i'),
        'lost_generation': ('= id_q[slot];', "= {8'b0, id_q[slot][4:0]};"),
        'lost_payload': ('= payload_q[slot];', "= payload_q[slot] ^ 64'd1;"),
    }
    original = rtl.read_text()
    for name, (old, new) in mutations.items():
        expected = 3 if name == 'raw_slot_age' else 1
        if original.count(old) != expected:
            raise RuntimeError(f'mutation no longer matches: {name}')
        directory = OUT/'mutations'/name
        directory.mkdir(parents=True, exist_ok=True)
        changed = directory/rtl.name
        changed.write_text(original.replace(old, new))
        artifacts.add(changed)
        executable = build(changed, directory/'obj_dir', name+'_build.log', mutated=True)
        run([executable, '1', '0'], name+'.log', failure='issue queue mismatch')
        print(f'IQ mutation {name}: PASS (detected)', flush=True)

    script = OUT/'synth.ys'
    script.write_text(f'plugin -i slang\nread_slang --top issue_queue {rtl}\n'
                      f'synth -top issue_queue -flatten\ncheck -assert\nwrite_json {OUT/"synth.json"}\n')
    artifacts.update([script, OUT/'synth.json'])
    run([suite/'bin/yosys', '-s', script], 'synth.log')
    modules = json.loads((OUT/'synth.json').read_text())['modules']
    counts = {}
    for data in modules.values():
        for cell in data['cells'].values():
            kind = cell['type']; counts[kind] = counts.get(kind, 0)+1
    if not counts or not any('DFF' in k.upper() for k in counts) or any('LATCH' in k.upper() or (not k.startswith('$_') and k not in modules) for k in counts):
        raise RuntimeError('missing state, latch or unmapped synthesis cell')
    if any(digest(ROOT/p) != value for p, value in inputs.items()):
        raise RuntimeError('input changed during check')
    files = {p: (ROOT/p).read_bytes() for p in paths}
    files.update({str(p.relative_to(ROOT)): p.read_bytes() for p in artifacts})
    result = dict(schema=1, profile=config['profile'], status='pass', scope=config['scope'], inputs_sha256=inputs,
                  verilator=version, simulations=results, required_counters=sorted(required),
                  mutations_detected=list(mutations), caller_assertions_checked=list(negatives),
                  synthesis_cells=counts, commands=commands, artifacts_sha256={p: hashlib.sha256(b).hexdigest() for p, b in files.items()},
                  two_wide_core_accepted=False)
    files['manifest.json'] = (json.dumps(result, indent=2)+'\n').encode()
    run_id = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    archive = ROOT/f'evidence/issue_queue/{run_id}.tar.gz'
    archive.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive, 'w:gz') as tar:
        for name, data in sorted(files.items()):
            info = tarfile.TarInfo(name); info.size = len(data); info.mode = 0o644
            tar.addfile(info, io.BytesIO(data))
    result['archive'] = str(archive.relative_to(ROOT)); result['archive_sha256'] = digest(archive)
    receipt.write_text(json.dumps(result, indent=2)+'\n')
    retained.write_text(receipt.read_text())
    print(f'Issue queue: PASS; {sum(counts.values())} generic cells; receipt {retained.relative_to(ROOT)}', flush=True)


if __name__ == '__main__':
    main()
