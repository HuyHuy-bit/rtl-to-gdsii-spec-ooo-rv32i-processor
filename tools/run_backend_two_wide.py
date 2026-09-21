#!/usr/bin/env python3
"""Check the connected backend against an independent instruction and ownership ledger."""
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
OUT = ROOT/'out/backend_two_wide'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--suite', type=Path, default=Path.home()/'tools/oss-cad-suite-20260905/oss-cad-suite')
    args = parser.parse_args()
    config = json.loads((ROOT/'config/backend_two_wide.json').read_text())
    if (config['entries'], config['identity_bits']) != (32, 13):
        raise RuntimeError('unsupported ROB geometry')
    OUT.mkdir(parents=True, exist_ok=True)
    retained = ROOT/'evidence/receipts/backend_two_wide.json'
    retained.parent.mkdir(parents=True, exist_ok=True)
    retained.unlink(missing_ok=True)
    receipt = OUT/'receipt.json'
    receipt.unlink(missing_ok=True)
    paths = [*config['sources'], 'verif/unit/backend_two_wide_tb.cpp', 'verif/unit/backend_reference.hpp', 'config/backend_two_wide.json',
             'config/rob_two_wide.json', 'config/rename_recovery.json', 'config/prf_contract.json',
             'config/commit_event.yaml', 'tools/gen_commit_event.py', 'tools/run_backend_two_wide.py',
             'config/toolchain.lock', 'config/synthesis.lock', 'Makefile']
    inputs = {p: digest(ROOT/p) for p in paths}
    commands = []

    def run(command, name, failure=None):
        result = subprocess.run([str(p) for p in command], cwd=ROOT, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=600, env=dict(os.environ, CCACHE_DISABLE='1'))
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
    rtl = ROOT/'rtl/backend/backend_two_wide.sv'

    def build(source, directory, log, mutated=False):
        run(['verilator', '--cc', '--exe', '--build', '--build-jobs', '2', '--Wall', '--assert',
             *(['-DSYNTHESIS', '-Wno-UNUSEDSIGNAL', '-Wno-UNUSEDPARAM'] if mutated else []), '--top-module', 'backend_two_wide', '--Mdir', directory,
             '-CFLAGS', f'-std=c++17 -Wall -Wextra -Werror -I{OUT}', *dependencies, source,
             ROOT/'verif/unit/backend_two_wide_tb.cpp', '-o', 'backend_two_wide_check'], log)
        return directory/'backend_two_wide_check'

    run(['verilator', '--lint-only', '--Wall', '--assert', '--top-module', 'backend_two_wide', *dependencies, rtl], 'lint.log')
    binary = build(rtl, OUT/'obj_dir', 'build.log')
    results = []
    required = {'dual_allocate', 'dual_complete', 'dual_retire', 'simultaneous', 'stalled_retire', 'raw', 'waw',
                'rob_full', 'prf_full', 'checkpoint_full', 'checkpoint_reuse', 'recovery', 'nested_recovery',
                'surviving_wb', 'recovery_wrap', 'trap', 'fault_no_write', 'stale', 'stale_resolve', 'duplicate',
                'flush_live', 'reset_live', 'drain', 'generation_stall', 'prf_bypass',
                'read_reclaim', 'resultless', 'late_solo', 'solo_retire'}
    for seed in config['seeds']:
        output = run([binary, str(seed), str(config['random_cycles_per_seed'])], f'seed_{seed}.log')
        if not output.startswith('BACKEND TWO WIDE PASS ') or len(output.splitlines()) != 1:
            raise RuntimeError('missing simulation completion')
        fields = {key: int(value) for key, value in re.findall(r'(\w+)=(\d+)', output)}
        if (fields.get('seed') != seed or fields.get('directed') != config['directed_cycles']
                or fields.get('cycles') != fields['directed']+config['random_cycles_per_seed']
                or not all(fields.get(k, 0) > 0 for k in required)):
            raise RuntimeError('missing completion or coverage: '+output)
        results.append(fields)
        print(output.strip(), flush=True)

    negatives = {'offered_drain': 'BACKEND_DRAIN'}
    for name, marker in negatives.items():
        run([binary, 'negative', name], 'negative_'+name+'.log', failure=marker)
        print(f'Backend caller assertion {name}: PASS (rejected)', flush=True)

    mutations = {
        'lost_late_solo': ('.complete_solo_i,', ".complete_solo_i(2'b00),"),
        'reclaimed_read_ready': ('&& !reclaim[read_address_i[port_id*6 +: 6]]', ''),
        'stale_write': ('.wb_live_i(wb_live)', ".wb_live_i(2'b11)"),
        'unqualified_prf': ('.wb_accept_i(wb_accept_o)', '.wb_accept_i(complete_offer_i)'),
        'lost_second_write': ('.wb_accept_i(wb_accept_o)', ".wb_accept_i(wb_accept_o & 2'b01)"),
        'swapped_payload': ('{complete_event_i[1].rd_value, complete_event_i[0].rd_value}', '{complete_event_i[0].rd_value, complete_event_i[1].rd_value}'),
        'wrong_checkpoint': ('<= checkpoint_id_o;', "<= checkpoint_id_o ^ 3'd1;"),
        'ignored_sink': ('retire_valid_o[0] && retire_ready_i[0]', 'retire_valid_o[0]'),
        'lost_trap_restore': ('wire recover = flush_i || trap_accept_o;', 'wire recover = flush_i;'),
        'lost_second_commit': ('.commit_i(commit_request)', ".commit_i(commit_request & 2'b01)"),
        'ignored_rob_credits': ('&& (selected & ~rob_allocate_ready) == 0', ''),
        'stale_resolution': ('.resolve_live_i(rob_resolve_ready)', ".resolve_live_i(1'b1)"),
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
        run([executable, '1', '0'], name+'.log', failure='backend two wide mismatch')
        print(f'Backend mutation {name}: PASS (detected)', flush=True)

    script = OUT/'synth.ys'
    script.write_text(f'plugin -i slang\nread_slang --top backend_two_wide '+ ' '.join(str(p) for p in [*dependencies, rtl])+'\n'
                      f'synth -top backend_two_wide -flatten\ncheck -assert\nwrite_json {OUT/"synth.json"}\n')
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
                  verilator=version, simulations=results, mutations_detected=list(mutations), caller_assertions_checked=list(negatives),
                  synthesis_cells=counts, commands=commands, artifacts_sha256={p: hashlib.sha256(b).hexdigest() for p, b in files.items()},
                  two_wide_core_accepted=False)
    files['manifest.json'] = (json.dumps(result, indent=2)+'\n').encode()
    run_id = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    archive = ROOT/f'evidence/backend_two_wide/{run_id}.tar.gz'
    archive.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive, 'w:gz') as tar:
        for name, data in sorted(files.items()):
            info = tarfile.TarInfo(name); info.size = len(data); info.mode = 0o644
            tar.addfile(info, io.BytesIO(data))
    result['archive'] = str(archive.relative_to(ROOT)); result['archive_sha256'] = digest(archive)
    receipt.write_text(json.dumps(result, indent=2)+'\n')
    retained.write_text(receipt.read_text())
    print(f'Backend two wide: PASS; {sum(counts.values())} generic cells; receipt {retained.relative_to(ROOT)}', flush=True)


if __name__ == '__main__':
    main()
