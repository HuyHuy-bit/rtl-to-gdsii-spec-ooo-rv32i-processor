#!/usr/bin/env python3
"""Validate a retained Architectural Slice run against its source and acceptance contract."""
import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
CONTRACT = 'config/architectural_slice_acceptance.json'
RECEIPT = 'evidence/receipts/architectural_slice.json'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def safe_path(name):
    return bool(name) and not Path(name).is_absolute() and '..' not in Path(name).parts


def source_files(root=ROOT):
    output = subprocess.check_output(['git', 'ls-files', '--cached', '--others', '--exclude-standard', '-z'], cwd=root)
    return sorted({n for n in output.decode().split('\0') if n and not n.startswith('evidence/') and (root/n).is_file()})


def evaluate(manifest, read):
    contract = json.loads(read(CONTRACT))
    config = json.loads(read('config/single_lane.json'))
    act_config = json.loads(read('config/act4_single_lane.json'))
    sail_config = json.loads(read('config/sail_differential.json'))
    require(manifest['schema'] == contract['schema'] == 1, 'unsupported schema')
    require(manifest['status'] == 'pass' and manifest['gate'] == 'Architectural Slice', 'gate did not pass')
    require(manifest['profile'] == contract['profile'], 'profile mismatch')
    require(manifest['claim_class'] == contract['claim_class'], 'claim scope mismatch')
    require(manifest['limitations'] == contract['limitations'], 'missing limitations')
    require(manifest['platform_sha256'] == sha(read('config/platform.yaml')), 'platform mismatch')
    require(re.fullmatch(r'[0-9a-f]{40}', manifest['source_revision']), 'missing source revision')
    require(type(manifest['dirty_tree']) is bool, 'missing dirty marker')
    require(manifest['patch_sha256'] == sha(read('source.patch')), 'source patch mismatch')
    require(manifest['started_utc'] < manifest['finished_utc'], 'invalid run interval')
    shape = [config[k] for k in ('physical_registers', 'rob_entries', 'maximum_inflight_instructions', 'retirement_slots_used')]
    require(shape == [64, 32, 1, 1], 'unsupported core configuration')
    require(config['seeds'] == act_config['seeds'] == sail_config['seeds'] == contract['seeds'], 'seed configuration differs')
    steps = manifest['steps']
    require([s['command'] for s in steps] == contract['commands'], 'missing or changed command')
    for step in steps:
        require(step['returncode'] == 0, 'failed command')
        require(math.isfinite(step['duration_seconds']) and 0 < step['duration_seconds'] <= contract['command_timeout_seconds'], 'invalid command duration')
        require(read(step['log']), 'missing command log')

    data = {k: json.loads(read(v)) for k, v in manifest['receipts'].items()}
    require(set(data) == {'core', 'act4', 'sail', 'a1'}, 'missing evidence family')
    for name, receipt in data.items():
        require(receipt['schema'] == 1, 'unsupported child receipt')
        hashes = receipt.get('inputs', receipt.get('inputs_sha256', {}))
        require(hashes, 'missing child inputs')
        for path, value in hashes.items():
            require(sha(read(path)) == value, f'stale {name} input: {path}')
        if name != 'a1':
            require(set(config['sources']) <= hashes.keys(), 'missing RTL fingerprints')
            require(receipt['architectural_slice_accepted'] is False, 'child cannot self-promote')
    require(data['a1']['status'] == 'pass' and data['a1']['physical_gate'] == 'PHY0', 'missing A1 feasibility gate')
    require('A1 evidence controls: PASS (16/16 rejected)' in read(steps[1]['log']).decode(), 'missing A1 evidence controls')
    require('S0 contracts: PASS' in read(steps[0]['log']).decode(), 'missing S0 checks')
    require('reference: 25/25 tests passed' in read(steps[0]['log']).decode(), 'incomplete nightly reference regression')

    core = data['core']
    expected = {(seed, 'normal'): (1600, False) for seed in contract['seeds']}
    expected[42, 'stall_store'] = (300, False)
    for mode in ('reset_fetch', 'reset_data', 'reset_commit', 'load_fault', 'fetch_fault'):
        expected[42, mode] = (80, False)
    for mode in ('wrong_id', 'protocol_error', 'reserved_status', 'unsolicited', 'store_fault', 'payload_error', 'fetch_wrong_id', 'fetch_payload'):
        expected[42, mode] = (0, True)
    require(len(core['cases']) == len(expected), 'wrong core case count')
    require({(r['seed'], r['mode']) for r in core['cases']} == expected.keys(), 'missing core scenario')
    for run in core['cases']:
        minimum, fatal = expected[run['seed'], run['mode']]
        log = read(f"out/single_lane/{run['mode']}_{run['seed']}.log").decode()
        require(run['expected_fatal'] is fatal, 'wrong fault disposition')
        require(run['checked_events'] == sum(line.startswith('EVENT ') for line in log.splitlines()), 'core event count mismatch')
        require(run['checked_events'] >= minimum, 'short core run')
        require(('FATAL ' if fatal else 'CORE PASS') in log, 'missing core completion')
    require(set(core['mutations_detected']) == {'wrong_alu', 'early_store'}, 'missing core mutations')
    core_step = read(steps[2]['log']).decode()
    for name in core['mutations_detected']:
        require(f'Core mutation {name}: PASS (detected)' in core_step, 'missing mutation outcome')
        require(read(f'out/single_lane/mutations/{name}/check.log'), 'missing mutation log')
    for bench, marker in [('backend_single_tb', 'BACKEND PASS'), ('csr_single_tb', 'CSR PASS')]:
        require(marker in read(f'out/single_lane/{bench}.log').decode(), 'missing directed unit test')
    require(core['spike_events'] >= contract['minimum_spike_events'], 'short Spike differential')
    require(f"Core Spike differential: PASS ({core['spike_events']} events)" in core_step, 'missing Spike comparison')
    require(core['spike_events'] == read('out/single_lane/spike_core.log').decode().count('\nEVENT '), 'Spike event count mismatch')
    net = json.loads(read('out/single_lane/synth.json'))['modules']['single_lane_core']
    cells = dict(Counter(c['type'] for c in net['cells'].values()))
    require(cells and cells == core['synthesis'], 'synthesis count mismatch')
    require(all(c.startswith('$_') and 'LATCH' not in c for c in cells), 'latch or hierarchy in synthesis')
    read('out/single_lane/lint.log')
    require(read('out/single_lane/build.log') and read('out/single_lane/synth.log'), 'missing build/synthesis logs')

    act = data['act4']
    prefix = str(Path(manifest['receipts']['act4']).parent)
    selected = act_config['selected_tests']
    require(len(selected) == len(set(selected)) == act['selected_tests'] == contract['selected_act4_tests'], 'ACT4 selection mismatch')
    require(act['unexpected_skips'] == act_config['unexpected_skips_allowed'] == contract['unexpected_skips_allowed'] == 0, 'unexpected ACT4 skip')
    require(act['deferred_tests'] == act_config['deferred_tests'], 'unaccounted ACT4 deferral')
    require(set(act['upstream']) == set(selected), 'missing ACT4 source hashes')
    for name, value in act['upstream'].items():
        require(sha(read('external/act4/tests/'+name)) == value, 'ACT4 source mismatch')
    wanted = {(str(Path(t).with_suffix('.elf')), seed) for t in selected for seed in contract['seeds']}
    require(len(act['runs']) == len(wanted) and {(r['test'], r['seed']) for r in act['runs']} == wanted, 'missing ACT4 test/seed')
    for run in act['runs']:
        log = read(f"{prefix}/{Path(run['test']).stem}_{run['seed']}.log").decode()
        require(0 < run['events'] <= act_config['instruction_limit'] and run['cycles'] > 0, 'invalid ACT4 budget')
        require(f"ACT4 PASS events={run['events']} cycles={run['cycles']}" in log, 'ACT4 completion mismatch')
        require(sha(read(f"{prefix}/work/spec_ooo_rv32i/elfs/{run['test']}")) == run['elf_sha256'], 'ACT4 ELF mismatch')
    require(set(act['negative_checks']) == {'corrupt_signature', 'fail', 'timeout'}, 'missing ACT4 controls')
    for name, error in [('corrupt_signature', 'ACT4 FAIL tohost=3'), ('fail', 'ACT4 FAIL tohost=3'), ('timeout', 'ACT4 instruction limit without tohost completion')]:
        require(error in read(f'{prefix}/{name}.log').decode(), 'missing ACT4 negative outcome')

    sail = data['sail']
    prefix = str(Path(manifest['receipts']['sail']).parent)
    require(sail['sail_sha256'] == act['sail_sha256'] == sail_config['sail_binary_sha256'], 'Sail pin mismatch')
    require(sail['limitations'] == sail_config['limitations'] and sail['sail_override'] == sail_config['sail_override'], 'Sail scope mismatch')
    require(sail['core_binary_sha256'] == sha(read('out/single_lane/obj_dir/core_check')), 'RTL executable mismatch')
    wanted = {(s, 'normal') for s in contract['seeds']} | {(42, m) for m in ('reset_fetch', 'reset_data', 'reset_commit', 'stall_store')}
    require(len(sail['runs']) == len(wanted) and {(r['seed'], r['mode']) for r in sail['runs']} == wanted, 'missing Sail scenario')
    sail_step = read(steps[4]['log']).decode()
    for run in sail['runs']:
        require(run['coverage'] == contract['coverage'], 'missing architectural coverage')
        require(contract['minimum_sail_events_per_run'] <= run['compared_events'] < 2*sail_config['instruction_limit'], 'short or inflated Sail comparison')
        require(run['bootstrap_events_per_reset'] == 6, 'comparison boundary differs')
        for path, field in [(f"{run['seed']}.elf", 'elf_sha256'), (f"{run['seed']}.trace", 'sail_trace_sha256'), (f"core_{run['seed']}_{run['mode']}.log", 'rtl_log_sha256')]:
            require(sha(read(f'{prefix}/{path}')) == run[field], 'Sail artifact mismatch')
        marker = f"Sail differential seed={run['seed']} {run['mode']}: PASS ({run['compared_events']} events)"
        require(marker in sail_step, 'missing Sail comparison outcome')
    require(set(sail['mutations_detected']) == {'load_sign', 'trap_value'}, 'missing Sail mutations')
    for name in sail['mutations_detected']:
        require(f'Sail RTL mutation {name}: PASS (detected)' in sail_step and read(f'{prefix}/{name}.log'), 'missing Sail mutation outcome')
    return {'act4_tests': len(selected), 'act4_runs': len(act['runs']),
            'act4_events': sum(r['events'] for r in act['runs']),
            'sail_runs': len(sail['runs']), 'sail_events': sum(r['compared_events'] for r in sail['runs']),
            'core_events_including_spike': sum(r['checked_events'] for r in core['cases']) + core['spike_events'],
            'generic_cells': sum(cells.values()), 'unexpected_skips': 0}


def verify(receipt_path, root=ROOT):
    receipt = json.loads(receipt_path.read_text())
    require(receipt['schema'] == 1 and receipt['status'] == 'pass', 'invalid acceptance receipt')
    require(safe_path(receipt['archive']), 'invalid archive path')
    archive = root / receipt['archive']
    require(sha(archive.read_bytes()) == receipt['archive_sha256'], 'archive hash mismatch')
    with tarfile.open(archive, 'r:gz') as tar:
        members = tar.getmembers()
        names = [m.name for m in members]
        require(len(names) == len(set(names)) and all(m.isfile() and safe_path(m.name) for m in members), 'unsafe or duplicate archive member')
        def read(name):
            require(safe_path(name), 'invalid artifact path')
            try:
                return tar.extractfile(name).read()
            except KeyError as error:
                raise ValueError(f'missing artifact: {name}') from error
        raw = read('manifest.json')
        require(sha(raw) == receipt['manifest_sha256'], 'manifest mismatch')
        manifest = json.loads(raw)
        for field in ('gate', 'status', 'profile', 'claim_class', 'source_revision', 'dirty_tree', 'patch_sha256',
                      'platform_sha256', 'command', 'started_utc', 'finished_utc', 'limitations'):
            require(receipt[field] == manifest[field], f'receipt metadata mismatch: {field}')
        require(set(source_files(root)) == set(manifest['inputs_sha256']), 'source inventory changed')
        require(set(names) == set(manifest['artifacts_sha256']) | {'manifest.json'}, 'artifact inventory mismatch')
        for name, value in manifest['artifacts_sha256'].items():
            require(sha(read(name)) == value, f'corrupt artifact: {name}')
        for name, value in manifest['inputs_sha256'].items():
            require(safe_path(name) and (root/name).is_file() and sha((root/name).read_bytes()) == value, f'stale source: {name}')
            require(sha(read(name)) == value, f'archived source mismatch: {name}')
        require(CONTRACT in manifest['inputs_sha256'] and 'tools/check_architectural_slice.py' in manifest['inputs_sha256'], 'missing gate fingerprints')
        metrics = evaluate(manifest, read)
        require(metrics == receipt['metrics'], 'acceptance metrics mismatch')
    return metrics


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--receipt', type=Path, default=ROOT/RECEIPT)
    args = parser.parse_args()
    metrics = verify(args.receipt)
    print(f'Architectural Slice evidence: PASS ({metrics["act4_runs"]} ACT4 runs; {metrics["sail_events"]} Sail events)')


if __name__ == '__main__':
    main()
