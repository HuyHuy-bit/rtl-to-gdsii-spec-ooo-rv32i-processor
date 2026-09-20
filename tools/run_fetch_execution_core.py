#!/usr/bin/env python3
"""Check instruction-memory execution against an independent architectural interpreter."""
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
OUT = ROOT/'out/fetch_execution_core'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--suite', type=Path, default=Path.home()/'tools/oss-cad-suite-20260905/oss-cad-suite')
    args = parser.parse_args()
    config = json.loads((ROOT/'config/fetch_execution_core.json').read_text())
    OUT.mkdir(parents=True, exist_ok=True)
    retained = ROOT/'evidence/receipts/fetch_execution_core.json'
    retained.parent.mkdir(parents=True, exist_ok=True)
    retained.unlink(missing_ok=True)
    (OUT/'receipt.json').unlink(missing_ok=True)
    paths = [*config['sources'], 'verif/unit/fetch_execution_core_tb.cpp', 'config/fetch_execution_core.json',
             config['lint_config'], 'config/platform.yaml', 'config/memory_protocol.yaml', 'config/commit_event.yaml',
             'config/control_flow_backend.json', 'config/fetch_two_wide.json', 'config/issue_backend.json',
             'config/backend_two_wide.json', 'config/issue_queue.json', 'config/rob_two_wide.json', 'config/prf_contract.json',
             'tools/gen_platform.py', 'tools/gen_memory_protocol.py', 'tools/gen_commit_event.py', 'tools/run_fetch_execution_core.py',
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
    run(['make', 'platform-check', 'memory-check', 'event-check'], 'schema.log')
    schema = json.loads((ROOT/'config/memory_protocol.yaml').read_text())
    fields = []
    for kind in ('request', 'response'):
        offset = 0
        for field in reversed(schema[kind+'_fields']):
            fields.append(f'constexpr unsigned {kind.upper()}_{field["name"].upper()}_OFFSET = {offset};')
            offset += field['width']
        fields.append(f'constexpr unsigned {kind.upper()}_BITS = {offset};')
    (OUT/'fetch_memory_layout.hpp').write_text('\n'.join(fields)+'\n')
    artifacts.add(OUT/'fetch_memory_layout.hpp')
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
             '--top-module', 'fetch_execution_core', '--Mdir', directory,
             '-CFLAGS', f'-std=c++17 -Wall -Wextra -Werror -I{OUT}', lint_config, *source_list,
             ROOT/'verif/unit/fetch_execution_core_tb.cpp', '-o', 'fetch_execution_core_check'], log)
        return directory/'fetch_execution_core_check'

    run(['verilator', '--lint-only', '--Wall', '--assert', '--top-module', 'fetch_execution_core',
         lint_config, *sources], 'lint.log')
    binary = build(sources, OUT/'obj_dir', 'build.log')
    required = {'backend_fault', 'branch_redirect', 'completion_stall', 'dispatch_stall', 'drain',
                'dual_dispatch', 'dual_retire', 'external_flush', 'fatal_injected', 'frontend_alignment',
                'frontend_bus_fault', 'frontend_pma', 'not_taken', 'redirect_pending', 'redirect_request_stall',
                'request_stall', 'reset', 'retire_stall', 'rob_full', 'taken', 'unsupported',
                'wrong_path_fault_recovered', *(f'op_{n}' for n in range(29))}
    results = []
    for seed in config['seeds']:
        output = run([binary, str(seed)], f'seed_{seed}.log')
        if not output.startswith('FETCH EXECUTION CORE PASS ') or len(output.splitlines()) != 1:
            raise RuntimeError('missing simulation completion')
        fields = {key: int(value) for key, value in re.findall(r'(\w+)=(\d+)', output)}
        if (fields.get('seed') != seed or fields.get('retired', 0) < 5000 or fields.get('cycles', 0) < 10000
                or not all(fields.get(k, 0) > 0 for k in required)):
            raise RuntimeError('missing completion or coverage: '+output)
        results.append(fields)
        print(output.strip(), flush=True)
    run([binary, 'negative'], 'negative_drain.log', failure='FETCH_CORE_DRAIN')
    print('Fetch core caller assertion: PASS (active frontend drain rejected)', flush=True)
    mutations = {
        'lost_fetch_consume': ('.take_i(dispatch_o)', ".take_i(2'b00)"),
        'consume_before_dispatch': ('.take_i(dispatch_o)', '.take_i(fetch_valid_o & {2{!fetch_fault_o}})'),
        'wrong_prediction_pc': ("fetch_pc_o[63:32] + 32'd4", 'fetch_pc_o[63:32]'),
        'taken_prediction': (".predicted_taken_i(2'b00)", ".predicted_taken_i(2'b11)"),
        'lost_branch_redirect': ('(flush_i || branch_redirect)', '(flush_i)'),
        'wrong_branch_target': ('flush_pc_i : branch_pc', "flush_pc_i : 32'd0"),
        'lost_backend_flush': ('.flush_i(flush_i && running)', ".flush_i(1'b0)"),
        'lost_external_restart': ('(flush_i || branch_redirect)', '(branch_redirect)'),
        'ignored_resolution_grant': ('.resolve_grant_i(resolve_grant_i && running)', '.resolve_grant_i(running)'),
        'fatal_retirement': ('.retire_ready_i(retire_ready_i & {2{running}})', '.retire_ready_i(retire_ready_i)'),
        'swapped_pc_lanes': ('.pc_i(fetch_pc_o)', '.pc_i({fetch_pc_o[31:0], fetch_pc_o[63:32]})'),
    }
    rtl = ROOT/'rtl/core/fetch_execution_core.sv'
    original = rtl.read_text()
    for name, (old, new) in mutations.items():
        if original.count(old) != 1:
            raise RuntimeError(f'mutation no longer matches: {name}')
        directory = OUT/'mutations'/name
        directory.mkdir(parents=True, exist_ok=True)
        changed = directory/rtl.name
        changed.write_text(original.replace(old, new)); artifacts.add(changed)
        executable = build([changed if p == rtl else p for p in sources], directory/'obj_dir', name+'_build.log', True)
        run([executable, '1'], name+'.log', failure='fetch execution core mismatch')
        print(f'Fetch mutation {name}: PASS (detected)', flush=True)
    script = OUT/'synth.ys'
    script.write_text('plugin -i slang\nread_slang --top fetch_execution_core '+' '.join(str(p) for p in sources)+'\n'
                      f'synth -top fetch_execution_core -flatten\ncheck -assert\nwrite_json {OUT/"synth.json"}\n')
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
                  mutations_detected=list(mutations), caller_assertions_checked=['active_frontend_drain'], synthesis_cells=counts,
                  commands=commands, artifacts_sha256={p: hashlib.sha256(b).hexdigest() for p, b in files.items()},
                  two_wide_core_accepted=False)
    files['manifest.json'] = (json.dumps(result, indent=2)+'\n').encode()
    run_id = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    archive = ROOT/f'evidence/fetch_execution_core/{run_id}.tar.gz'
    archive.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive, 'w:gz') as tar:
        for name, data in sorted(files.items()):
            info = tarfile.TarInfo(name); info.size = len(data); info.mode = 0o644
            tar.addfile(info, io.BytesIO(data))
    result['archive'] = str(archive.relative_to(ROOT)); result['archive_sha256'] = digest(archive)
    (OUT/'receipt.json').write_text(json.dumps(result, indent=2)+'\n')
    retained.write_text((OUT/'receipt.json').read_text())
    print(f'Fetch execution core: PASS; {sum(counts.values())} generic cells; receipt {retained.relative_to(ROOT)}', flush=True)


if __name__ == '__main__':
    main()
