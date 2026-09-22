#!/usr/bin/env python3
"""Verify CSR/MRET head execution against independent state and backend ownership."""
import argparse
from datetime import datetime, timezone
import hashlib
import io
import json
import os
from pathlib import Path
import re
import resource
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT/'out/head_system_backend'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--suite', type=Path, default=Path.home()/'tools/oss-cad-suite-20260905/oss-cad-suite')
    args = parser.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    config = json.loads((ROOT/'config/head_system_backend.json').read_text())
    OUT.mkdir(parents=True, exist_ok=True)
    retained = ROOT/'evidence/receipts/head_system_backend.json'
    retained.parent.mkdir(parents=True, exist_ok=True)
    retained.unlink(missing_ok=True)
    (OUT/'receipt.json').unlink(missing_ok=True)
    paths = [*config['sources'], 'verif/unit/head_system_backend_tb.cpp', 'verif/unit/head_system_reference.hpp',
             'config/head_system_backend.json', config['lint_config'], 'config/platform.yaml', 'tools/gen_platform.py', 'config/csr_two_wide.json', 'config/backend_two_wide.json', 'config/rob_two_wide.json',
             'config/rename_recovery.json', 'config/prf_contract.json', 'config/commit_event.yaml',
             'tools/gen_commit_event.py', 'tools/run_head_system_backend.py', 'config/toolchain.lock',
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
    run(['make', 'platform-check', 'event-check'], 'schema.log')
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
    platform = json.loads((ROOT/'config/platform.yaml').read_text())
    rows = []
    for c in platform['csrs']:
        values = [c[k]+'u' for k in ('address', 'reset', 'write_mask', 'fixed_mask', 'fixed_value')]
        rows.append('{'+', '.join([*values, 'true' if c['access']=='ro' else 'false'])+'}')
    schema = json.loads((ROOT/'config/commit_event.yaml').read_text())
    offset, offsets = 0, {}
    for field in reversed(schema['csr_fields']):
        offsets[field['name']] = offset
        offset += field['width']
    header = '#include <array>\n#include <cstdint>\n'
    header += 'struct Spec { unsigned address; uint32_t reset, write_mask, fixed_mask, fixed_value; bool readonly; };\n'
    header += f'constexpr std::array<Spec, {len(rows)}> CSR_SPEC = {{{{'+', '.join(rows)+'}};\n'
    header += f'constexpr unsigned EFFECT_BITS = {offset};\n'
    header += 'constexpr unsigned FIELD_OFFSET[] = {'+', '.join(str(offsets[f['name']]) for f in schema['csr_fields'])+'};\n'
    header += 'constexpr unsigned FIELD_WIDTH[] = {'+', '.join(str(f['width']) for f in schema['csr_fields'])+'};\n'
    (OUT/'csr_reference_layout.hpp').write_text(header)
    artifacts.add(OUT/'csr_reference_layout.hpp')
    sources = [ROOT/p for p in config['sources']]

    def build(source_list, directory, log, mutated=False):
        run(['verilator', '--cc', '--exe', '--build', '--build-jobs', '2', '--Wall', '--assert',
             *(['-DSYNTHESIS', '-Wno-UNUSEDSIGNAL', '-Wno-UNUSEDPARAM'] if mutated else []),
             '--top-module', 'head_system_backend', '--Mdir', directory,
             '-CFLAGS', f'-std=c++17 -Wall -Wextra -Werror -I{OUT}', ROOT/config['lint_config'], *source_list,
             ROOT/'verif/unit/head_system_backend_tb.cpp', '-o', 'head_system_backend_check'], log)
        return directory/'head_system_backend_check'

    run(['verilator', '--lint-only', '--Wall', '--assert', '--top-module', 'head_system_backend',
         ROOT/config['lint_config'], *sources], 'lint.log')
    binary = build(sources, OUT/'obj_dir', 'build.log')
    required = {'serial', 'illegal', 'trap', 'held_serial', 'held_illegal', 'held_trap',
                'dual_retire', 'mret', 'stale_descriptor', 'nonhead_descriptor', 'cancel_pending',
                'address_sweep', 'cancel_1', 'cancel_2', 'older_dependency', 'branch_kill',
                'raw_trap_0', 'raw_trap_1', 'raw_trap_2', 'held_input_change', 'drain', 'serial_bypass', *(f'kind_{n}' for n in (1,2,3,5,6,7))}
    results = []
    for seed in config['seeds']:
        output = run([binary, str(seed)], f'seed_{seed}.log')
        if not output.startswith('HEAD SYSTEM PASS ') or len(output.splitlines()) != 1:
            raise RuntimeError('missing simulation completion')
        fields = {key: int(value) for key, value in re.findall(r'(\w+)=(\d+)', output)}
        if (fields.get('seed') != seed or fields.get('address_sweep') != 4096 or fields.get('cycles', 0) < 10000
                or not all(fields.get(k, 0) > 0 for k in required)):
            raise RuntimeError('missing completion or coverage: '+output)
        results.append(fields)
        print(output.strip(), flush=True)
    mutations = {
        'early_state': ('head_system_controller.sv', '.response_ready_i(accepted_serial || accepted_illegal || accepted_trap)', '.response_ready_i(serial_offer_o || accepted_illegal || accepted_trap)'),
        'lost_serial_accept': ('head_system_controller.sv', '.response_ready_i(accepted_serial || accepted_illegal || accepted_trap)', '.response_ready_i(accepted_illegal || accepted_trap)'),
        'wrong_result': ('head_system_controller.sv', "saved_q.rd_addr == 0 ? 32'd0 : value", "32'd0"),
        'lost_effects': ('head_system_controller.sv', 'serial_event_o.csr_effects = effects;', "serial_event_o.csr_effects = '0;"),
        'stale_descriptor': ('head_system_controller.sv', 'system_id_i == head_id_i', 'system_id_i[4:0] == head_id_i[4:0]'),
        'early_redirect': ('head_system_controller.sv', '(accepted_serial && mret_q)', '(serial_offer_o && mret_q)'),
        'wrong_redirect': ('head_system_controller.sv', "redirect_o ? next_pc : 32'd0", "redirect_o ? saved_q.pc_before : 32'd0"),
        'lost_source': ('head_system_controller.sv', '.source_i,', ".source_i(32'd0),"),
        'lost_dual_count': ('head_system_controller.sv', "2'($countones(retire_accept_i & ~{1'b0, serial_accept_i}))", "2'(|(retire_accept_i & ~{1'b0, serial_accept_i}))"),
        'wrong_illegal_value': ('head_system_controller.sv', 'illegal_event_o.trap_value = saved_q.instruction;', "illegal_event_o.trap_value = 0;"),
        'illegal_retires': ('head_system_controller.sv', '&& !trap_q && legal;', '&& !trap_q;'),
        'cancel_keeps_response': ('csr_two_wide.sv', 'assign response_valid_o = !rst_i && !cancel_i && pending_q;', 'assign response_valid_o = !rst_i && pending_q;'),
        'trap_retires': ('csr_two_wide.sv', 'assign retired_o = commit_state && !trap_q;', 'assign retired_o = commit_state;'),
        'mret_status': ('csr_two_wide.sv', "{28'd0, csr_q[I_MSTATUS][7], 3'd0}", "32'd0"),
        'lost_trap_accept': ('head_system_backend.sv', '.trap_ready_i(trap_accept_o)', ".trap_ready_i(1'b0)"),
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
        run([executable, '1'], name+'.log', failure='head system mismatch')
        print(f'Head system mutation {name}: PASS (detected)', flush=True)
    script = OUT/'synth.ys'
    script.write_text('plugin -i slang\nread_slang --top head_system_backend '+' '.join(str(p) for p in sources)+'\n'
                      f'synth -top head_system_backend -flatten\ncheck -assert\nwrite_json {OUT/"synth.json"}\n')
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
    archive = ROOT/f'evidence/head_system_backend/{run_id}.tar.gz'
    archive.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive, 'w:gz') as tar:
        for name, data in sorted(files.items()):
            info = tarfile.TarInfo(name); info.size = len(data); info.mode = 0o644
            tar.addfile(info, io.BytesIO(data))
    result['archive'] = str(archive.relative_to(ROOT)); result['archive_sha256'] = digest(archive)
    (OUT/'receipt.json').write_text(json.dumps(result, indent=2)+'\n')
    retained.write_text((OUT/'receipt.json').read_text())
    print(f'Head system: PASS; {sum(counts.values())} generic cells; receipt {retained.relative_to(ROOT)}', flush=True)


if __name__ == '__main__':
    main()
