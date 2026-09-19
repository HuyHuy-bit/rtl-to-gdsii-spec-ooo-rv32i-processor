#!/usr/bin/env python3
"""Check the connected backend and issue queue against an independent program-order ledger."""
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
OUT = ROOT/'out/issue_backend'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--suite', type=Path, default=Path.home()/'tools/oss-cad-suite-20260905/oss-cad-suite')
    args = parser.parse_args()
    config = json.loads((ROOT/'config/issue_backend.json').read_text())
    if (config['queue_entries'], config['rob_entries'], config['identity_bits'], config['payload_bits']) != (16, 32, 13, 64):
        raise RuntimeError('unsupported backend geometry')
    OUT.mkdir(parents=True, exist_ok=True)
    retained = ROOT/'evidence/receipts/issue_backend.json'
    retained.parent.mkdir(parents=True, exist_ok=True)
    retained.unlink(missing_ok=True)
    receipt = OUT/'receipt.json'
    receipt.unlink(missing_ok=True)
    paths = [*config['sources'], 'verif/unit/issue_backend_tb.cpp', 'config/issue_backend.json',
             'config/backend_two_wide.json', 'config/issue_queue.json', 'config/rob_two_wide.json',
             'config/prf_contract.json', 'config/commit_event.yaml', 'tools/gen_commit_event.py',
             'tools/run_issue_backend.py', 'config/toolchain.lock', 'config/synthesis.lock', 'Makefile']
    inputs = {p: digest(ROOT/p) for p in paths}
    commands = []

    def run(command, name, failure=None):
        result = subprocess.run([str(p) for p in command], cwd=ROOT, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=900, env=dict(os.environ, CCACHE_DISABLE='1'))
        (OUT/name).write_text(result.stdout)
        commands.append(dict(command=[str(p) for p in command], returncode=result.returncode, log=name))
        if (failure and (result.returncode == 0 or failure not in result.stdout)) or (not failure and result.returncode != 0):
            raise RuntimeError(f'check failed; see {OUT/name}\n{result.stdout[-2500:]}')
        return result.stdout

    version = run(['verilator', '--version'], 'version.log').strip()
    tools = json.loads((ROOT/'config/toolchain.lock').read_text())['tools']
    if version != next(t['expected_first_line'] for t in tools if t['name'] == 'verilator'):
        raise RuntimeError('Verilator version differs')
    lock = json.loads((ROOT/'config/synthesis.lock').read_text())
    suite = args.suite.resolve()
    for p, value in lock['sha256'].items():
        if digest(suite/p) != value:
            raise RuntimeError(f'synthesis tool pin differs: {p}')
    run(['python3', 'tools/gen_commit_event.py', '--input', 'config/commit_event.yaml', '--check'], 'schema.log')
    schema = json.loads((ROOT/'config/commit_event.yaml').read_text())
    offset = sum(f['width'] for f in schema['csr_fields'])*schema['max_csr_effects']
    fields = []
    for field in reversed(schema['slot_fields']):
        fields.append(f'constexpr unsigned {field["name"].upper()}_OFFSET = {offset};')
        offset += field['width']
    (OUT/'backend_event_layout.hpp').write_text('\n'.join([*fields, f'constexpr unsigned EVENT_BITS = {offset};'])+'\n')
    dependencies = [ROOT/p for p in config['sources'][:-1]]
    rtl = ROOT/config['sources'][-1]

    def build(source, directory, log, mutated=False):
        run(['verilator', '--cc', '--exe', '--build', '--build-jobs', '2', '--Wall', '--assert',
             *(['-DSYNTHESIS', '-Wno-UNUSEDSIGNAL', '-Wno-UNUSEDPARAM'] if mutated else []),
             '--top-module', 'issue_backend', '--Mdir', directory,
             '-CFLAGS', f'-std=c++17 -Wall -Wextra -Werror -I{OUT}', *dependencies, source,
             ROOT/'verif/unit/issue_backend_tb.cpp', '-o', 'issue_backend_check'], log)
        return directory/'issue_backend_check'

    run(['verilator', '--lint-only', '--Wall', '--assert', '--top-module', 'issue_backend', *dependencies, rtl], 'lint.log')
    binary = build(rtl, OUT/'obj_dir', 'build.log')
    results = []
    required = {'dual_allocate', 'dual_complete', 'dual_retire', 'dual_issue', 'dispatch_issue', 'simultaneous',
                'queue_full', 'queue_stall', 'rob_full', 'prf_full', 'backpressure', 'stalled_wakeup',
                'retained_wakeup', 'wakeup_issue', 'prf_bypass', 'unaccepted_broadcast', 'zero_source',
                'port0_only', 'port1_only', 'wrapped_window', 'recovery', 'recovery_killed', 'recovery_survivor',
                'nested_recovery', 'surviving_wb', 'simultaneous_checkpoint_create_resolve',
                'branch_issue', 'resolution_after_issue', 'trap', 'trap_live', 'fault_no_write',
                'resultless', 'flush_live', 'reset_live', 'drain'}
    for seed in config['seeds']:
        output = run([binary, str(seed), str(config['random_cycles_per_seed'])], f'seed_{seed}.log')
        if not output.startswith('ISSUE BACKEND PASS ') or len(output.splitlines()) != 1:
            raise RuntimeError('missing simulation completion')
        fields = {key: int(value) for key, value in re.findall(r'(\w+)=(\d+)', output)}
        if (fields.get('seed') != seed or fields.get('directed') != config['directed_cycles']
                or fields.get('cycles') != fields['directed']+config['random_cycles_per_seed']
                or not all(fields.get(k, 0) > 0 for k in required)):
            raise RuntimeError('missing completion or coverage: '+output)
        results.append(fields)
        print(output.strip(), flush=True)

    negatives = {'eligibility': 'IQ_ELIGIBILITY', 'offered_drain': 'ISSUE_BACKEND_DRAIN'}
    for name, marker in negatives.items():
        run([binary, 'negative', name], 'negative_'+name+'.log', failure=marker)
        print(f'Issue backend caller assertion {name}: PASS (rejected)', flush=True)

    stimulus_negatives = {'branch_before_issue': 'stimulus resolution before issue', 'branch_port': 'stimulus branch port'}
    for name, marker in stimulus_negatives.items():
        run([binary, 'negative', name], 'stimulus_'+name+'.log', failure=marker)
        print(f'Issue backend stimulus guard {name}: PASS (rejected)', flush=True)

    mutations = {
        'lost_queue_credit': (' && (selected & ~dispatch_ready) == 0', ''),
        'lost_second_credit': ('(selected & ~dispatch_ready) == 0', '(selected[0] & ~dispatch_ready[0]) == 0'),
        'unselected_prefix': ('valid_i[1] && !cfi_i[0]', 'valid_i[1]'),
        'unaccepted_dispatch': ('.dispatch_i(allocate_accept_o)', '.dispatch_i(valid_i)'),
        'lost_dispatch_readiness': ('.dispatch_ready_i(source_ready_o)', ".dispatch_ready_i(4'b1111)"),
        'lost_issue_destination': ('.dispatch_destination_i(destination_o)', ".dispatch_destination_i(12'd0)"),
        'lost_payload': ('.dispatch_payload_i(payload_i)', ".dispatch_payload_i(128'd0)"),
        'swapped_sources': ('{source2_o[11:6], source1_o[11:6], source2_o[5:0], source1_o[5:0]}',
                            '{source1_o[11:6], source2_o[11:6], source1_o[5:0], source2_o[5:0]}'),
        'swapped_dispatch_identity': ('.dispatch_id_i(allocate_id_o)', '.dispatch_id_i({allocate_id_o[12:0], allocate_id_o[25:13]})'),
        'swapped_eligibility': ('.dispatch_eligible_i(eligible_i)', '.dispatch_eligible_i({eligible_i[1:0], eligible_i[3:2]})'),
        'renamed_wakeup_tag': ('.wb_destination_i(wb_destination_o)', '.wb_destination_i(destination_o)'),
        'raw_wakeup': ('.wb_accept_i(wb_accept_o)', '.wb_accept_i(complete_offer_i)'),
        'lost_wakeup': ('.wb_accept_i(wb_accept_o)', ".wb_accept_i(2'b00)"),
        'unconnected_read': ('.read_address_i(issue_source_o)', ".read_address_i(24'd0)"),
        'wrong_head_slot': ('.head_slot_i(head_id_o[4:0])', ".head_slot_i(5'd0)"),
        'wrong_recover_slot': ('.recover_slot_i(resolve_id_i[4:0])', '.recover_slot_i(head_id_o[4:0])'),
        'lost_branch_kill': ('.recover_i(branch_recover_o)', ".recover_i(1'b0)"),
        'lost_trap_flush': ('.flush_i(recover)', '.flush_i(flush_i)'),
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
        run([executable, '1', '0'], name+'.log', failure='issue backend mismatch')
        print(f'Issue backend mutation {name}: PASS (detected)', flush=True)

    script = OUT/'synth.ys'
    script.write_text('plugin -i slang\nread_slang --top issue_backend '+' '.join(str(p) for p in [*dependencies, rtl])+'\n'
                      f'synth -top issue_backend -flatten\ncheck -assert\nwrite_json {OUT/"synth.json"}\n')
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
    for p in OUT.rglob('*'):
        if p.is_file() and 'obj_dir' not in p.parts and p != receipt:
            files[str(p.relative_to(ROOT))] = p.read_bytes()
    result = dict(schema=1, profile=config['profile'], status='pass', scope=config['scope'], inputs_sha256=inputs,
                  verilator=version, simulations=results, required_counters=sorted(required),
                  mutations_detected=list(mutations), caller_assertions_checked=list(negatives),
                  stimulus_guards_checked=list(stimulus_negatives),
                  synthesis_cells=counts, commands=commands,
                  artifacts_sha256={p: hashlib.sha256(b).hexdigest() for p, b in files.items()},
                  two_wide_core_accepted=False)
    files['manifest.json'] = (json.dumps(result, indent=2)+'\n').encode()
    run_id = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    archive = ROOT/f'evidence/issue_backend/{run_id}.tar.gz'
    archive.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive, 'w:gz') as tar:
        for name, data in sorted(files.items()):
            info = tarfile.TarInfo(name); info.size = len(data); info.mode = 0o644
            tar.addfile(info, io.BytesIO(data))
    result['archive'] = str(archive.relative_to(ROOT)); result['archive_sha256'] = digest(archive)
    receipt.write_text(json.dumps(result, indent=2)+'\n')
    retained.write_text(receipt.read_text())
    print(f'Issue backend: PASS; {sum(counts.values())} generic cells; receipt {retained.relative_to(ROOT)}', flush=True)


if __name__ == '__main__':
    main()
