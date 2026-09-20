#!/usr/bin/env python3
"""Verify fetch ordering, held memory ownership and redirect discard behavior."""
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
OUT = ROOT/'out/fetch_two_wide'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--suite', type=Path, default=Path.home()/'tools/oss-cad-suite-20260905/oss-cad-suite')
    args = parser.parse_args()
    config = json.loads((ROOT/'config/fetch_two_wide.json').read_text())
    if (config['line_bytes'], config['outstanding'], config['lanes']) != (32, 1, 2):
        raise RuntimeError('unsupported fetch geometry')
    OUT.mkdir(parents=True, exist_ok=True)
    retained = ROOT/'evidence/receipts/fetch_two_wide.json'
    retained.parent.mkdir(parents=True, exist_ok=True)
    retained.unlink(missing_ok=True)
    (OUT/'receipt.json').unlink(missing_ok=True)
    paths = [*config['sources'], 'verif/unit/fetch_two_wide_tb.cpp', 'config/fetch_two_wide.json',
             config['lint_config'], 'config/platform.yaml', 'config/memory_protocol.yaml',
             'tools/gen_platform.py', 'tools/gen_memory_protocol.py', 'tools/run_fetch_two_wide.py',
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
    run(['make', 'platform-check', 'memory-check'], 'schema.log')
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
    sources = [ROOT/p for p in config['sources']]
    lint_config = ROOT/config['lint_config']

    def build(source_list, directory, log, mutated=False):
        run(['verilator', '--cc', '--exe', '--build', '--build-jobs', '2', '--Wall', '--assert',
             *(['-DSYNTHESIS', '-Wno-UNUSEDSIGNAL', '-Wno-UNUSEDPARAM'] if mutated else []),
             '--top-module', 'fetch_two_wide', '--Mdir', directory,
             '-CFLAGS', f'-std=c++17 -Wall -Wextra -Werror -I{OUT}', lint_config, *source_list,
             ROOT/'verif/unit/fetch_two_wide_tb.cpp', '-o', 'fetch_two_wide_check'], log)
        return directory/'fetch_two_wide_check'

    run(['verilator', '--lint-only', '--Wall', '--assert', '--top-module', 'fetch_two_wide',
         lint_config, *sources], 'lint.log')
    binary = build(sources, OUT/'obj_dir', 'build.log')
    required = {'alignment_fault', 'bus_fault', 'consume', 'disabled_buffer', 'disabled_empty',
                'discard_data', 'discard_fault', 'dual_take', 'fatal_hold', 'fault_take', 'fill',
                'id_wrap', 'line_tail', 'malformed', 'output_stall', 'partial_pair', 'pma_fault',
                'redirect_buffered', 'redirect_empty', 'redirect_handshake', 'redirect_offered',
                'redirect_pending', 'redirect_response', 'redirect_response_fault', 'redirect_stopped', 'repeated_redirect',
                'request', 'request_stall', 'reset_buffered', 'reset_idle', 'reset_offered', 'reset_pending',
                *(f'offset_{n}' for n in range(8)), *(f'malformed_{n}' for n in range(8))}
    results = []
    for seed in config['seeds']:
        output = run([binary, str(seed), str(config['random_cycles_per_seed'])], f'seed_{seed}.log')
        if not output.startswith('FETCH TWO WIDE PASS ') or len(output.splitlines()) != 1:
            raise RuntimeError('missing simulation completion')
        fields = {key: int(value) for key, value in re.findall(r'(\w+)=(\d+)', output)}
        if (fields.get('seed') != seed or fields.get('random') != config['random_cycles_per_seed']
                or fields.get('directed', 0) < 500 or fields.get('cycles') != fields['directed']+fields['random']+8
                or not all(fields.get(k, 0) > 0 for k in required)):
            raise RuntimeError('missing completion or coverage: '+output)
        results.append(fields)
        print(output.strip(), flush=True)
    run([binary, 'negative'], 'negative_prefix.log', failure='FETCH_PREFIX')
    run([binary, 'negative', 'empty'], 'negative_empty.log', failure='FETCH_PREFIX')
    print('Fetch caller assertions: PASS (non-prefix and empty takes rejected)', flush=True)
    mutations = {
        'disabled_request': ('enable_i && !redirect_i', '!redirect_i'),
        'withdraw_request': ('active && state_q == REQUEST;', 'active && state_q == REQUEST && !redirect_i;'),
        'live_request_pc': ("{request_line_q, 5'd0}", "{pc_q[31:5], 5'd0}"),
        'discard_lost': ('if (discard_q || redirect_i)', 'if (redirect_i)'),
        'redirect_response_lost': ('if (discard_q || redirect_i)', 'if (discard_q)'),
        'first_redirect_only': ('pc_q <= redirect_pc_i;', 'if (!discard_q) pc_q <= redirect_pc_i;'),
        'redirect_output_visible': ('if (active && !redirect_i)', 'if (active)'),
        'partial_pair_skipped': ("take_i[1] ? 32'd8 : 32'd4", "32'd8"),
        'line_tail_second_lane': ("pc_q[4:2] == 3'd7 ? 2'b01 : 2'b11", "2'b11"),
        'word_reversed': ('line_q[pc_q[4:2]*32 +: 32]', "line_q[(7-int'(pc_q[4:2]))*32 +: 32]"),
        'id_not_advanced': ("id_q <= id_q + 1'b1;", 'id_q <= id_q;'),
        'wrong_id_accepted': ('|| response_i.transaction_id != id_q', ''),
        'uncached_data_accepted': ('|| response_i.uncached_read_data != 0', ''),
        'fault_data_accepted': ('|| (response_i.status != MEM_STATUS_OK && response_i.line_read_data != 0)', ''),
        'reserved_status_accepted': ('MEM_STATUS_OK, MEM_STATUS_ACCESS_FAULT}', 'MEM_STATUS_OK, MEM_STATUS_ACCESS_FAULT, MEM_STATUS_PROTOCOL_ERROR}'),
        'stale_protocol_unchecked': ('if (malformed) fatal_o <= 1;', 'if (malformed && !discard_q) fatal_o <= 1;'),
        'fault_repeated': ('state_q <= STOP;', 'state_q <= FAULT;'),
        'alignment_cause_lost': ('assign fault_cause_o = misaligned_q ?', "assign fault_cause_o = 1'b0 ?"),
        'fatal_clear_redirect': ('end else if (!fatal_o) begin', 'end else if (!fatal_o || redirect_i) begin\n      fatal_o <= 0;'),
    }
    rtl = ROOT/'rtl/frontend/fetch_two_wide.sv'
    original = rtl.read_text()
    for name, (old, new) in mutations.items():
        if original.count(old) != 1:
            raise RuntimeError(f'mutation no longer matches: {name}')
        directory = OUT/'mutations'/name
        directory.mkdir(parents=True, exist_ok=True)
        changed = directory/rtl.name
        changed.write_text(original.replace(old, new)); artifacts.add(changed)
        executable = build([changed if p == rtl else p for p in sources], directory/'obj_dir', name+'_build.log', True)
        run([executable, '1', '0'], name+'.log', failure='fetch two wide mismatch')
        print(f'Fetch mutation {name}: PASS (detected)', flush=True)
    script = OUT/'synth.ys'
    script.write_text('plugin -i slang\nread_slang --top fetch_two_wide '+' '.join(str(p) for p in sources)+'\n'
                      f'synth -top fetch_two_wide -flatten\ncheck -assert\nwrite_json {OUT/"synth.json"}\n')
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
                  mutations_detected=list(mutations), caller_assertions_checked=['prefix', 'empty'], synthesis_cells=counts,
                  commands=commands, artifacts_sha256={p: hashlib.sha256(b).hexdigest() for p, b in files.items()},
                  two_wide_core_accepted=False)
    files['manifest.json'] = (json.dumps(result, indent=2)+'\n').encode()
    run_id = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    archive = ROOT/f'evidence/fetch_two_wide/{run_id}.tar.gz'
    archive.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive, 'w:gz') as tar:
        for name, data in sorted(files.items()):
            info = tarfile.TarInfo(name); info.size = len(data); info.mode = 0o644
            tar.addfile(info, io.BytesIO(data))
    result['archive'] = str(archive.relative_to(ROOT)); result['archive_sha256'] = digest(archive)
    (OUT/'receipt.json').write_text(json.dumps(result, indent=2)+'\n')
    retained.write_text((OUT/'receipt.json').read_text())
    print(f'Fetch two wide: PASS; {sum(counts.values())} generic cells; receipt {retained.relative_to(ROOT)}', flush=True)


if __name__ == '__main__':
    main()
