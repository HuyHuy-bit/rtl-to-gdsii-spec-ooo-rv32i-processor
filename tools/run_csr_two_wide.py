#!/usr/bin/env python3
"""Verify held two-wide machine CSR state against an independent architectural model."""
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
OUT = ROOT/'out/csr_two_wide'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--suite', type=Path, default=Path.home()/'tools/oss-cad-suite-20260905/oss-cad-suite')
    args = parser.parse_args()
    config = json.loads((ROOT/'config/csr_two_wide.json').read_text())
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    OUT.mkdir(parents=True, exist_ok=True)
    retained = ROOT/'evidence/receipts/csr_two_wide.json'
    retained.parent.mkdir(parents=True, exist_ok=True)
    retained.unlink(missing_ok=True)
    (OUT/'receipt.json').unlink(missing_ok=True)
    paths = [*config['sources'], 'verif/unit/csr_two_wide_tb.cpp', 'config/csr_two_wide.json',
             config['lint_config'], 'config/platform.yaml', 'config/commit_event.yaml',
             'tools/gen_platform.py', 'tools/gen_commit_event.py', 'tools/run_csr_two_wide.py',
             'config/toolchain.lock', 'config/synthesis.lock', 'Makefile']
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
    lint_config = ROOT/config['lint_config']

    def build(source_list, directory, log, mutated=False):
        run(['verilator', '--cc', '--exe', '--build', '--build-jobs', '2', '--Wall', '--assert',
             *(['-DSYNTHESIS', '-Wno-UNUSEDSIGNAL', '-Wno-UNUSEDPARAM'] if mutated else []),
             '--top-module', 'csr_two_wide', '--Mdir', directory,
             '-CFLAGS', f'-std=c++17 -Wall -Wextra -Werror -I{OUT}', lint_config, *source_list,
             ROOT/'verif/unit/csr_two_wide_tb.cpp', '-o', 'csr_two_wide_check'], log)
        return directory/'csr_two_wide_check'

    run(['verilator', '--lint-only', '--Wall', '--assert', '--top-module', 'csr_two_wide',
         lint_config, *sources], 'lint.log')
    binary = build(sources, OUT/'obj_dir', 'build.log')
    required = {'address_sweep', 'cancel', 'cancel_csr', 'cancel_reset_mret', 'cancel_reset_trap',
                'csr', 'csr_read', 'cycle_carry', 'cycle_inhibit', 'cycle_wrap', 'cycle_write_priority',
                'dual_retire', 'held', 'illegal', 'instret_carry', 'instret_inhibit', 'instret_wrap',
                'instret_write_priority', 'mret', 'no_effective_write', 'prepare', 'read_suppressed',
                'reset', 'reset_csr', 'single_retire', 'trap', 'unsupported_instruction',
                *(f'kind_{n}' for n in (1,2,3,5,6,7)), *(f'cause_{n}' for n in (0,1,2,3,4,5,6,7,11))}
    results = []
    for seed in config['seeds']:
        output = run([binary, str(seed)], f'seed_{seed}.log')
        if not output.startswith('CSR TWO WIDE PASS ') or len(output.splitlines()) != 1:
            raise RuntimeError('missing simulation completion')
        fields = {key: int(value) for key, value in re.findall(r'(\w+)=(\d+)', output)}
        if (fields.get('seed') != seed or fields.get('address_sweep') != 4096 or fields.get('cycles', 0) < 80000
                or not all(fields.get(k, 0) > 0 for k in required)):
            raise RuntimeError('missing completion or coverage: '+output)
        results.append(fields)
        print(output.strip(), flush=True)
    negatives = {'bad-count': 'CSR2_RETIRE_COUNT', 'bad-serialization': 'CSR2_SERIALIZE'}
    for name, expected in negatives.items():
        run([binary, '--'+name], name+'.log', failure=expected)
    mutations = {
        'dual_count_lost': ('csr_two_wide.sv', "64'(increment)", "64'(increment != 0)"),
        'trap_retires': ('csr_two_wide.sv', 'assign retired_o = commit_state && !trap_q;', 'assign retired_o = commit_state;'),
        'readonly_writable': ('csr_two_wide.sv', '!(write_query && READ_ONLY[query_index])', "1'b1"),
        'zero_value_suppresses_write': ('csr_two_wide.sv', 'decoded.zimm != 0;', 'operand != 0;'),
        'read_suppression_lost': ('csr_two_wide.sv', '&& !(decoded.funct3[1:0] == 1 && decoded.rd == 0)', ''),
        'x0_uses_payload': ('csr_two_wide.sv', "(decoded.rs1 == 0 ? 32'd0 : source_i)", 'source_i'),
        'counter_write_priority_lost': ('csr_two_wide.sv', '&& !cycle_write)', ')'),
        'instret_write_priority_lost': ('csr_two_wide.sv', '&& !instret_write)', ')'),
        'inhibit_lost': ('csr_two_wide.sv', '!csr_q[I_MCOUNTINHIBIT][2]', "1'b1"),
        'trap_status_lost': ('csr_two_wide.sv', "{24'd0, csr_q[I_MSTATUS][3], 7'd0}", "32'd0"),
        'mret_status_lost': ('csr_two_wide.sv', "{28'd0, csr_q[I_MSTATUS][7], 3'd0}", "32'd0"),
        'mepc_alignment_lost': ('csr_two_wide.sv', "pc_i & 32'hfffffffc", 'pc_i'),
        'live_counter_response': ('csr_two_wide.sv', "assign value_o = response_valid_o ? value_q : 32'd0;", "assign value_o = response_valid_o ? old_value : 32'd0;"),
        'cancel_still_commits': ('csr_two_wide.sv', 'assign response_valid_o = !rst_i && !cancel_i && pending_q;', 'assign response_valid_o = !rst_i && pending_q;'),
        'updates_while_held': ('csr_two_wide.sv', 'wire commit_state = accept_o && legal_q;', 'wire commit_state = response_valid_o && legal_q;'),
        'trap_target_wrong': ('csr_two_wide.sv', 'next_pc = mtvec_o;', 'next_pc = mepc_o;'),
        'trap_value_lost': ('csr_two_wide.sv', 'effect(CSR_MTVAL, trap_value_i,', "effect(CSR_MTVAL, 32'd0,"),
        'effect_mask_wrong': ('csr_two_wide.sv', 'result.read_mask = WRITE_MASK[i] | FIXED_MASK[i];', 'result.read_mask = WRITE_MASK[i];'),
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
        run([executable, '1'], name+'.log', failure='CSR two-wide mismatch')
        print(f'CSR mutation {name}: PASS (detected)', flush=True)
    script = OUT/'synth.ys'
    script.write_text('plugin -i slang\nread_slang --top csr_two_wide '+' '.join(str(p) for p in sources)+'\n'
                      f'synth -top csr_two_wide -flatten\ncheck -assert\nwrite_json {OUT/"synth.json"}\n')
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
                  mutations_detected=list(mutations), caller_assertions_checked=list(negatives), synthesis_cells=counts,
                  commands=commands, artifacts_sha256={p: hashlib.sha256(b).hexdigest() for p, b in files.items()},
                  two_wide_core_accepted=False)
    files['manifest.json'] = (json.dumps(result, indent=2)+'\n').encode()
    run_id = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    archive = ROOT/f'evidence/csr_two_wide/{run_id}.tar.gz'
    archive.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive, 'w:gz') as tar:
        for name, data in sorted(files.items()):
            info = tarfile.TarInfo(name); info.size = len(data); info.mode = 0o644
            tar.addfile(info, io.BytesIO(data))
    result['archive'] = str(archive.relative_to(ROOT)); result['archive_sha256'] = digest(archive)
    (OUT/'receipt.json').write_text(json.dumps(result, indent=2)+'\n')
    retained.write_text((OUT/'receipt.json').read_text())
    print(f'CSR two-wide: PASS; {sum(counts.values())} generic cells; receipt {retained.relative_to(ROOT)}', flush=True)


if __name__ == '__main__':
    main()
