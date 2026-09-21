#!/usr/bin/env python3
"""Verify atomic backend head retirement against independent ownership and payload state."""
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
OUT = ROOT/'out/serial_retirement'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--suite', type=Path, default=Path.home()/'tools/oss-cad-suite-20260905/oss-cad-suite')
    args = parser.parse_args()
    config = json.loads((ROOT/'config/serial_retirement.json').read_text())
    OUT.mkdir(parents=True, exist_ok=True)
    retained = ROOT/'evidence/receipts/serial_retirement.json'
    retained.parent.mkdir(parents=True, exist_ok=True)
    retained.unlink(missing_ok=True)
    (OUT/'receipt.json').unlink(missing_ok=True)
    paths = [*config['sources'], 'verif/unit/serial_retirement_tb.cpp', 'verif/unit/backend_reference.hpp',
             'config/serial_retirement.json', 'config/backend_two_wide.json', 'config/rob_two_wide.json',
             'config/rename_recovery.json', 'config/prf_contract.json', 'config/commit_event.yaml',
             'tools/gen_commit_event.py', 'tools/run_serial_retirement.py', 'config/toolchain.lock',
             'config/synthesis.lock', 'Makefile']
    inputs = {p: digest(ROOT/p) for p in paths}
    commands, artifacts = [], set()

    def run(command, name, failure=None):
        result = subprocess.run([str(p) for p in command], cwd=ROOT, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=900, env=dict(os.environ, CCACHE_DISABLE='1'))
        (OUT/name).write_text(result.stdout)
        artifacts.add(OUT/name)
        commands.append(dict(command=[str(p) for p in command], returncode=result.returncode, log=name))
        if (failure and (result.returncode == 0 or failure not in result.stdout)) or (not failure and result.returncode != 0):
            raise RuntimeError(f'check failed; see {OUT/name}\n{result.stdout[-2500:]}')
        return result.stdout

    version = run(['verilator', '--version'], 'version.log').strip()
    tools = json.loads((ROOT/'config/toolchain.lock').read_text())['tools']
    if version != next(t['expected_first_line'] for t in tools if t['name'] == 'verilator'):
        raise RuntimeError('Verilator version differs')
    suite = args.suite.resolve()
    for p, value in json.loads((ROOT/'config/synthesis.lock').read_text())['sha256'].items():
        if digest(suite/p) != value:
            raise RuntimeError(f'synthesis tool pin differs: {p}')
    run(['make', 'event-check'], 'schema.log')
    schema = json.loads((ROOT/'config/commit_event.yaml').read_text())
    offset = 0
    fields = []
    for field in reversed(schema['csr_fields']):
        fields.append(f'constexpr unsigned CSR_{field["name"].upper()}_OFFSET = {offset};')
        offset += field['width']
    fields.append(f'constexpr unsigned CSR_EFFECT_BITS = {offset};')
    offset *= schema['max_csr_effects']
    for field in reversed(schema['slot_fields']):
        fields.append(f'constexpr unsigned {field["name"].upper()}_OFFSET = {offset};')
        offset += field['width']
    (OUT/'backend_event_layout.hpp').write_text('\n'.join([*fields, f'constexpr unsigned EVENT_BITS = {offset};'])+'\n')
    artifacts.add(OUT/'backend_event_layout.hpp')
    sources = [ROOT/p for p in config['sources']]

    def build(source_list, directory, log, mutated=False):
        run(['verilator', '--cc', '--exe', '--build', '--build-jobs', '2', '--Wall', '--assert',
             *(['-DSYNTHESIS', '-Wno-UNUSEDSIGNAL', '-Wno-UNUSEDPARAM'] if mutated else []),
             '--top-module', 'backend_two_wide', '--Mdir', directory,
             '-CFLAGS', f'-std=c++17 -Wall -Wextra -Werror -I{OUT}', *source_list,
             ROOT/'verif/unit/serial_retirement_tb.cpp', '-o', 'serial_retirement_check'], log)
        return directory/'serial_retirement_check'

    run(['verilator', '--lint-only', '--Wall', '--assert', '--top-module', 'backend_two_wide',
         *sources], 'lint.log')
    binary = build(sources, OUT/'obj_dir', 'build.log')
    required = {'serial_accept', 'serial_stall', 'serial_rejected', 'serial_no_destination',
                'serial_blocks_completion', 'serial_blocks_allocation', 'prf_bypass', 'repeated_offer',
                'older_writer', 'flush_offer', 'reset_offer', 'recovery_priority', 'trap_priority',
                'older_branch_kill', 'full_window', 'random_serial', 'random_cancel', 'drain',
                *(f'rd_{n}' for n in range(32)), *(f'reject_{n}' for n in range(10))}
    results = []
    for seed in config['seeds']:
        output = run([binary, str(seed)], f'seed_{seed}.log')
        if not output.startswith('SERIAL RETIREMENT PASS ') or len(output.splitlines()) != 1:
            raise RuntimeError('missing simulation completion')
        fields = {key: int(value) for key, value in re.findall(r'(\w+)=(\d+)', output)}
        if (fields.get('seed') != seed or fields.get('serial_accept', 0) < 1800 or fields.get('cycles', 0) < 10000
                or not all(fields.get(k, 0) > 0 for k in required)):
            raise RuntimeError('missing completion or coverage: '+output)
        results.append(fields)
        print(output.strip(), flush=True)
    mutations = {
        'early_write': ('backend_two_wide.sv', 'lane == 0 && retire_ready_i[0] && serial_event_i.rd_write_mask != 0', 'lane == 0 && serial_event_i.rd_write_mask != 0'),
        'no_serial_write': ('backend_two_wide.sv', 'lane == 0 && retire_ready_i[0] && serial_event_i.rd_write_mask != 0', "1'b0"),
        'wrong_payload': ('backend_two_wide.sv', "{32'd0, serial_event_i.rd_value}", "64'd0"),
        'lost_commit': ('backend_two_wide.sv', '.commit_i(commit_request)', ".commit_i(serial_selected ? 2'b00 : commit_request)"),
        'stale_identity': ('rob_two_wide.sv', 'serial_id_i == head_id_o', 'serial_id_i[4:0] == head_id_o[4:0]'),
        'not_solo': ('rob_two_wide.sv', '&& solo_q[head_q] && !cfi_q[head_q]', '&& !cfi_q[head_q]'),
        'cfi_allowed': ('rob_two_wide.sv', '&& !cfi_q[head_q] && serial_id_i', '&& serial_id_i'),
        'bad_pc_allowed': ('rob_two_wide.sv', 'serial_event_i.pc_before == pc_q[head_q]', "1'b1"),
        'bad_rd_allowed': ('rob_two_wide.sv', 'serial_event_i.rd_addr == rd_q[head_q]', "1'b1"),
        'bad_mask_allowed': ('rob_two_wide.sv', "serial_event_i.rd_write_mask == (rd_q[head_q] == 0 ? 32'd0 : 32'hffffffff)", "1'b1"),
        'ordinary_completion_leak': ('rob_two_wide.sv', '!clear_window && !serial_selected && valid_q', '!clear_window && valid_q'),
        'allocation_leak': ('rob_two_wide.sv', '&& !drained_i && !serial_selected) begin', '&& !drained_i) begin'),
        'wrong_event': ('rob_two_wide.sv', 'serial_selected ? serial_event_i : event_q[head_q]', 'event_q[head_q]'),
        'ignored_sink': ('backend_two_wide.sv', 'retire_valid_o[0] && retire_ready_i[0]', 'retire_valid_o[0]'),
    }
    for name, (filename, old, new) in mutations.items():
        rtl = next(p for p in sources if p.name == filename)
        original = rtl.read_text()
        if original.count(old) != 1:
            raise RuntimeError(f'mutation no longer matches: {name}')
        directory = OUT/'mutations'/name
        directory.mkdir(parents=True, exist_ok=True)
        changed = directory/rtl.name
        changed.write_text(original.replace(old, new)); artifacts.add(changed)
        executable = build([changed if p == rtl else p for p in sources], directory/'obj_dir', name+'_build.log', True)
        run([executable, '1'], name+'.log', failure='backend two wide mismatch')
        print(f'Serial retirement mutation {name}: PASS (detected)', flush=True)
    script = OUT/'synth.ys'
    script.write_text('plugin -i slang\nread_slang --top backend_two_wide '+' '.join(str(p) for p in sources)+'\n'
                      f'synth -top backend_two_wide -flatten\ncheck -assert\nwrite_json {OUT/"synth.json"}\n')
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
    for p in artifacts:
        files[str(p.relative_to(ROOT))] = p.read_bytes()
    result = dict(schema=1, profile=config['profile'], status='pass', scope=config['scope'], inputs_sha256=inputs,
                  verilator=version, simulations=results, required_counters=sorted(required),
                  mutations_detected=list(mutations), caller_assertions_checked=[], synthesis_cells=counts,
                  commands=commands, artifacts_sha256={p: hashlib.sha256(b).hexdigest() for p, b in files.items()},
                  two_wide_core_accepted=False)
    files['manifest.json'] = (json.dumps(result, indent=2)+'\n').encode()
    run_id = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    archive = ROOT/f'evidence/serial_retirement/{run_id}.tar.gz'
    archive.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive, 'w:gz') as tar:
        for name, data in sorted(files.items()):
            info = tarfile.TarInfo(name); info.size = len(data); info.mode = 0o644
            tar.addfile(info, io.BytesIO(data))
    result['archive'] = str(archive.relative_to(ROOT)); result['archive_sha256'] = digest(archive)
    (OUT/'receipt.json').write_text(json.dumps(result, indent=2)+'\n')
    retained.write_text((OUT/'receipt.json').read_text())
    print(f'Serial retirement: PASS; {sum(counts.values())} generic cells; receipt {retained.relative_to(ROOT)}', flush=True)


if __name__ == '__main__':
    main()
