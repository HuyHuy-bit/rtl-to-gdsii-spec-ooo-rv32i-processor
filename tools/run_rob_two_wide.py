#!/usr/bin/env python3
"""Check the full-size ROB against an independent ordered instruction ledger."""
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
OUT = ROOT/'out/rob_two_wide'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--suite', type=Path, default=Path.home()/'tools/oss-cad-suite-20260905/oss-cad-suite')
    args = parser.parse_args()
    config = json.loads((ROOT/'config/rob_two_wide.json').read_text())
    if (config['entries'], config['identity_bits']) != (32, 13):
        raise RuntimeError('unsupported ROB geometry')
    OUT.mkdir(parents=True, exist_ok=True)
    retained = ROOT/'evidence/receipts/rob_two_wide.json'
    retained.parent.mkdir(parents=True, exist_ok=True)
    retained.unlink(missing_ok=True)
    receipt = OUT/'receipt.json'
    receipt.unlink(missing_ok=True)
    paths = ['rtl/backend/rob_two_wide.sv', 'rtl/generated/commit_event_pkg.sv', 'verif/unit/rob_two_wide_tb.cpp',
             'config/rob_two_wide.json', 'config/commit_event.yaml', 'tools/gen_commit_event.py',
             'tools/run_rob_two_wide.py', 'config/toolchain.lock', 'config/synthesis.lock', 'Makefile']
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
    (OUT/'rob_event_layout.hpp').write_text('\n'.join([*fields, f'constexpr unsigned EVENT_BITS = {offset};'])+'\n')
    package = ROOT/'rtl/generated/commit_event_pkg.sv'
    rtl = ROOT/'rtl/backend/rob_two_wide.sv'

    def build(source, directory, log, mutated=False):
        run(['verilator', '--cc', '--exe', '--build', '--build-jobs', '2', '--Wall', '--assert',
             *(['-DSYNTHESIS', '-Wno-UNUSEDSIGNAL', '-Wno-UNUSEDPARAM'] if mutated else []), '--top-module', 'rob_two_wide', '--Mdir', directory,
             '-CFLAGS', f'-std=c++17 -Wall -Wextra -Werror -I{OUT}', package, source,
             ROOT/'verif/unit/rob_two_wide_tb.cpp', '-o', 'rob_two_wide_check'], log)
        return directory/'rob_two_wide_check'

    run(['verilator', '--lint-only', '--Wall', '--assert', '--top-module', 'rob_two_wide', package, rtl], 'lint.log')
    binary = build(rtl, OUT/'obj_dir', 'build.log')
    results = []
    required = {'full', 'dual_allocate', 'dual_retire', 'dual_complete', 'simultaneous', 'backpressure',
                'unresolved', 'solo', 'trap', 'flush_live', 'reset_live', 'recovery', 'recovery_wrap',
                'recovery_complete', 'recovery_suppression', 'resolution_complete', 'out_of_order',
                'duplicate', 'stale', 'stale_resolution', 'wrap_block', 'drain', 'trap_priority', 'slot_wrap'}
    for seed in config['seeds']:
        output = run([binary, str(seed), str(config['random_cycles_per_seed'])], f'seed_{seed}.log')
        if not output.startswith('ROB TWO WIDE PASS ') or len(output.splitlines()) != 1:
            raise RuntimeError('missing simulation completion')
        fields = {key: int(value) for key, value in re.findall(r'(\w+)=(\d+)', output)}
        if (fields.get('seed') != seed or fields.get('directed') != config['directed_cycles']
                or fields.get('cycles') != fields['directed']+config['random_cycles_per_seed']
                or not all(fields.get(k, 0) > 0 for k in required)):
            raise RuntimeError('missing completion or coverage: '+output)
        results.append(fields)
        print(output.strip(), flush=True)

    negatives = {'drain_live': 'ROB2_DRAIN', 'complete_stale': 'ROB2_COMPLETE', 'resolve_stale': 'ROB2_RESOLVE',
                 'retire_unready': 'ROB2_RETIRE', 'trap_unready': 'ROB2_TRAP'}
    for name, marker in negatives.items():
        run([binary, 'negative', name], 'negative_'+name+'.log', failure=marker)
        print(f'ROB caller assertion {name}: PASS (rejected)', flush=True)

    mutations = {
        'generation_alias': ('generation_q[complete_id_i[lane*13 +: 5]] == complete_id_i[lane*13+5 +: 8]', "1'b1"),
        'resolve_generation_alias': ('generation_q[resolve_slot] == resolve_id_i[12:5]', "1'b1"),
        'numeric_age': ("5'(complete_id_i[lane*13 +: 5] - head_q) <= resolve_age", 'complete_id_i[lane*13 +: 5] <= resolve_slot'),
        'numeric_squash': ("5'(5'(slot) - head_q) > resolve_age", "5'(slot) > resolve_slot"),
        'duplicate_completion': ('complete_ready_o[1] = 0;', 'complete_ready_o[1] = complete_ready_o[1];'),
        'unresolved_retirement': ('(!cfi_q[head_q] || resolved_q[head_q])', "1'b1"),
        'solo_pair': ('&& !solo_q[head_q] && !solo_q[second_head]', ''),
        'wrong_event': ('<= complete_event_i[lane];', '<= complete_event_i[1-lane];'),
        'wrong_order': ("order_q + 64'(lane)", "order_q + 64'(lane) + 64'd1"),
        'generation_wrap': ('count_q < 32 && !exhausted_q[tail_q]', 'count_q < 32'),
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
        run([executable, '1', '0'], name+'.log', failure='rob two wide mismatch')
        print(f'ROB mutation {name}: PASS (detected)', flush=True)

    script = OUT/'synth.ys'
    script.write_text(f'plugin -i slang\nread_slang --top rob_two_wide {package} {rtl}\n'
                      f'synth -top rob_two_wide -flatten\ncheck -assert\nwrite_json {OUT/"synth.json"}\n')
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
    archive = ROOT/f'evidence/rob_two_wide/{run_id}.tar.gz'
    archive.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive, 'w:gz') as tar:
        for name, data in sorted(files.items()):
            info = tarfile.TarInfo(name); info.size = len(data); info.mode = 0o644
            tar.addfile(info, io.BytesIO(data))
    result['archive'] = str(archive.relative_to(ROOT)); result['archive_sha256'] = digest(archive)
    receipt.write_text(json.dumps(result, indent=2)+'\n')
    retained.write_text(receipt.read_text())
    print(f'ROB two wide: PASS; {sum(counts.values())} generic cells; receipt {retained.relative_to(ROOT)}', flush=True)


if __name__ == '__main__':
    main()
