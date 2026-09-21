#!/usr/bin/env python3
"""Check computed branch resolution and selective producer cancellation against an independent ledger."""
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
OUT = ROOT/'out/control_flow_backend'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--suite', type=Path, default=Path.home()/'tools/oss-cad-suite-20260905/oss-cad-suite')
    args = parser.parse_args()
    config = json.loads((ROOT/'config/control_flow_backend.json').read_text())
    if (config['queue_entries'], config['rob_entries'], config['identity_bits'], config['physical_registers'], config['producer_slots'], config['checkpoints']) != (16, 32, 13, 64, 2, 8):
        raise RuntimeError('unsupported backend geometry')
    OUT.mkdir(parents=True, exist_ok=True)
    retained = ROOT/'evidence/receipts/control_flow_backend.json'
    retained.parent.mkdir(parents=True, exist_ok=True)
    retained.unlink(missing_ok=True)
    receipt = OUT/'receipt.json'
    receipt.unlink(missing_ok=True)
    paths = [*config['sources'], 'verif/unit/control_flow_backend_tb.cpp', 'config/control_flow_backend.json',
             config['lint_config'], 'config/issue_backend.json', 'config/backend_two_wide.json', 'config/issue_queue.json', 'config/rob_two_wide.json',
             'config/prf_contract.json', 'config/commit_event.yaml', 'tools/gen_commit_event.py',
             'tools/run_control_flow_backend.py', 'config/toolchain.lock', 'config/synthesis.lock', 'Makefile']
    inputs = {p: digest(ROOT/p) for p in paths}
    commands = []
    artifacts = set()

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
    artifacts.add(OUT/'backend_event_layout.hpp')
    sources = [ROOT/p for p in config['sources']]
    lint_config = ROOT/config['lint_config']

    def build(source_list, directory, log, mutated=False):
        run(['verilator', '--cc', '--exe', '--build', '--build-jobs', '2', '--Wall', '--assert',
             *(['-DSYNTHESIS', '-Wno-UNUSEDSIGNAL', '-Wno-UNUSEDPARAM'] if mutated else []),
             '--top-module', 'control_flow_backend', '--Mdir', directory,
             '-CFLAGS', f'-std=c++17 -Wall -Wextra -Werror -I{OUT}', lint_config, *source_list,
             ROOT/'verif/unit/control_flow_backend_tb.cpp', '-o', 'control_flow_backend_check'], log)
        return directory/'control_flow_backend_check'

    run(['verilator', '--lint-only', '--Wall', '--assert', '--top-module', 'control_flow_backend',
         lint_config, *sources], 'lint.log')
    binary = build(sources, OUT/'obj_dir', 'build.log')
    results = []
    required = {'dual_allocate', 'dual_complete', 'dual_retire', 'dual_issue', 'queue_full', 'rob_full',
                'completion_stall', 'retire_stall', 'unsupported', 'flush_held', 'reset_held',
                'drain', 'generation_stall', 'wrapped_window', 'out_of_order_issue', 'out_of_order_complete',
                'wakeup_bypass', 'consume_refill', 'bundle_raw', 'bundle_waw', 'resultless', 'retired',
                'branch_launch', 'cfi_lane1', 'cfi_prefix', 'checkpoint_full', 'checkpoint_reuse',
                'direction_only_prediction', 'fault_complete', 'fault_op_21', 'fault_op_27', 'fault_op_28',
                'head_fault', 'ignored_tail_unsupported', 'jalr_lsb_cleared', 'kill_completed', 'kill_fault',
                'kill_held', 'kill_queued', 'kill_resolved_branch', 'nested_resolve', 'not_taken',
                'prediction_right', 'prediction_wrong', 'resolution_blocks_completion', 'resolution_stall',
                'resolve_allocate', 'resolve_completion_stall', 'surviving_held', 'surviving_wb', 'taken',
                'wrapped_recovery', *(f'op_{n}' for n in range(29))}
    for seed in config['seeds']:
        output = run([binary, str(seed), str(config['random_cycles_per_seed'])], f'seed_{seed}.log')
        if not output.startswith('CONTROL FLOW BACKEND PASS ') or len(output.splitlines()) != 1:
            raise RuntimeError('missing simulation completion')
        fields = {key: int(value) for key, value in re.findall(r'(\w+)=(\d+)', output)}
        if (fields.get('seed') != seed or fields.get('directed') != config['directed_cycles']
                or fields.get('cycles') != fields['directed']+config['random_cycles_per_seed']+fields.get('final_drain', 0)
                or fields.get('random') != config['random_cycles_per_seed'] or fields.get('final_drain', 0) < 1
                or not all(fields.get(k, 0) > 0 for k in required)):
            raise RuntimeError('missing completion or coverage: '+output)
        results.append(fields)
        print(output.strip(), flush=True)

    negatives = {'prefix': 'CF_BACKEND_PREFIX', 'unresolved_drain': 'CF_BACKEND_DRAIN'}
    for name, marker in negatives.items():
        run([binary, 'negative', name], 'negative_'+name+'.log', failure=marker)
        print(f'Control flow backend caller assertion {name}: PASS (rejected)', flush=True)

    mutations = {
        'early_completion': ('control_flow_pipeline.sv', '(!cfi_q || event_o.trap || resolved_q)', "1'b1"),
        'repeat_resolution': ('control_flow_pipeline.sv', '&& !event_o.trap && !resolved_q', '&& !event_o.trap'),
        'ignored_resolution_grant': ('control_flow_backend.sv', '.resolve_grant_i,', ".resolve_grant_i(1'b1),"),
        'live_prediction_pc': ('control_flow_backend.sv', '.predicted_pc_i(prediction_pc_q[issue_id_o[4:0]])', '.predicted_pc_i(predicted_pc_i[31:0])'),
        'live_prediction_taken': ('control_flow_backend.sv', '.predicted_taken_i(prediction_taken_q[issue_id_o[4:0]])', '.predicted_taken_i(predicted_taken_i[0])'),
        'lost_lane1_prediction': ('control_flow_backend.sv', 'allocate_accept_o[lane] && cfi[lane]', 'allocate_accept_o[lane] && cfi[lane] && lane == 0'),
        'lost_direction_mismatch': ('control_flow_pipeline.sv', 'predicted_taken_i != taken || predicted_pc_i != next_pc', 'predicted_pc_i != next_pc'),
        'wrong_next_pc': ('control_flow_pipeline.sv', 'built.pc_after = next_pc;', "built.pc_after = pc_i + 32'd4;"),
        'unsigned_branch_direction': ('control_flow_pipeline.sv', 'taken = $signed(source1_i) < $signed(source2_i);', 'taken = source1_i < source2_i;'),
        'fault_link_write': ('control_flow_pipeline.sv', '!execute_trap && decoded.rd != 0', 'decoded.rd != 0'),
        'lost_fault_target': ('control_flow_pipeline.sv', 'built.trap_value = trap_value;', "built.trap_value = 32'd0;"),
        'lost_result_hold': ('control_flow_pipeline.sv', 'if (take_i) pending_q <= 0;', 'pending_q <= 0;'),
        'lost_full_flush': ('control_flow_pipeline.sv', 'if (rst_i || flush_i) begin', 'if (rst_i) begin'),
        'lost_selective_cancel': ('control_flow_backend.sv', '.flush_i(producer_flush || cancel_alu1)', '.flush_i(producer_flush)'),
        'raw_slot_age': ('control_flow_backend.sv', "5'(completion_id_o[17:13] - head_id_o[4:0])\n      > 5'(resolve_id_o[4:0] - head_id_o[4:0])", 'completion_id_o[17:13] > resolve_id_o[4:0]'),
        'cancel_before_grant': ('control_flow_backend.sv', 'wire cancel_alu1 = redirect_o &&', 'wire cancel_alu1 = resolve_valid_o && resolve_mispredict_o &&'),
        'cancel_older': ('control_flow_backend.sv', "> 5'(resolve_id_o[4:0] - head_id_o[4:0])", "< 5'(resolve_id_o[4:0] - head_id_o[4:0])"),
        'lost_cfi_eligibility': ('control_flow_backend.sv', "cfi[lane] ? 2'b01 : 2'b11", "2'b11"),
        'lost_cfi_allocation': ('control_flow_backend.sv', '.cfi_i(cfi)', ".cfi_i(2'b00)"),
        'wrong_resolver_identity': ('control_flow_backend.sv', 'assign resolve_id_o = completion_id_o[12:0];', 'assign resolve_id_o = completion_id_o[25:13];'),
        'release_on_resolution': ('control_flow_backend.sv', '.take_i(completion_accept_o[0])', '.take_i(resolve_accept_o)'),
    }
    for name, (filename, old, new) in mutations.items():
        rtl = next(p for p in sources if p.name == filename)
        original = rtl.read_text()
        if original.count(old) != 1:
            raise RuntimeError(f'mutation no longer matches: {name}')
        directory = OUT/'mutations'/name
        directory.mkdir(parents=True, exist_ok=True)
        changed = directory/rtl.name
        changed.write_text(original.replace(old, new))
        artifacts.add(changed)
        executable = build([changed if p == rtl else p for p in sources], directory/'obj_dir', name+'_build.log', mutated=True)
        run([executable, '1', '0'], name+'.log', failure='control flow backend mismatch')
        print(f'Control flow backend mutation {name}: PASS (detected)', flush=True)

    script = OUT/'synth.ys'
    script.write_text('plugin -i slang\nread_slang --top control_flow_backend '+' '.join(str(p) for p in sources)+'\n'
                      f'synth -top control_flow_backend -flatten\ncheck -assert\nwrite_json {OUT/"synth.json"}\n')
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
                  mutations_detected=list(mutations), caller_assertions_checked=list(negatives),
                  synthesis_cells=counts, commands=commands,
                  artifacts_sha256={p: hashlib.sha256(b).hexdigest() for p, b in files.items()},
                  two_wide_core_accepted=False)
    files['manifest.json'] = (json.dumps(result, indent=2)+'\n').encode()
    run_id = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    archive = ROOT/f'evidence/control_flow_backend/{run_id}.tar.gz'
    archive.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive, 'w:gz') as tar:
        for name, data in sorted(files.items()):
            info = tarfile.TarInfo(name); info.size = len(data); info.mode = 0o644
            tar.addfile(info, io.BytesIO(data))
    result['archive'] = str(archive.relative_to(ROOT)); result['archive_sha256'] = digest(archive)
    receipt.write_text(json.dumps(result, indent=2)+'\n')
    retained.write_text(receipt.read_text())
    print(f'Control flow backend: PASS; {sum(counts.values())} generic cells; receipt {retained.relative_to(ROOT)}', flush=True)


if __name__ == '__main__':
    main()
