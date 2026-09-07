import copy
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch

from tools.check_architectural_slice import CONTRACT, ROOT, evaluate, sha, verify


def fixture():
    files = {p: (ROOT/p).read_bytes() for p in (CONTRACT, 'config/single_lane.json', 'config/act4_single_lane.json',
             'config/sail_differential.json', 'config/platform.yaml', 'tools/check_architectural_slice.py')}
    def put(path, value):
        files[path] = json.dumps(value).encode()
    contract = json.loads(files[CONTRACT])
    config = json.loads(files['config/single_lane.json'])
    act_config = json.loads(files['config/act4_single_lane.json'])
    sail_config = json.loads(files['config/sail_differential.json'])
    for path in config['sources']:
        files[path] = b'fixture source'
    inputs = {p: sha(files[p]) for p in config['sources']}
    cases = [(seed, 'normal', 1600, False) for seed in contract['seeds']]
    cases += [(42, 'stall_store', 300, False)]
    cases += [(42, mode, 80, False) for mode in ('reset_fetch', 'reset_data', 'reset_commit', 'load_fault', 'fetch_fault')]
    cases += [(42, mode, 0, True) for mode in ('wrong_id', 'protocol_error', 'reserved_status', 'unsolicited', 'store_fault', 'payload_error', 'fetch_wrong_id', 'fetch_payload')]
    core = dict(schema=1, inputs_sha256=inputs, architectural_slice_accepted=False, cases=[], mutations_detected=['wrong_alu', 'early_store'], spike_events=1056, synthesis={'$_AND_': 1})
    for seed, mode, count, fatal in cases:
        core['cases'].append(dict(seed=seed, mode=mode, checked_events=count, expected_fatal=fatal))
        files[f'out/single_lane/{mode}_{seed}.log'] = ('EVENT x\n'*count+('FATAL x' if fatal else 'CORE PASS')).encode()
    for name in core['mutations_detected']:
        files[f'out/single_lane/mutations/{name}/check.log'] = b'detected'
    for name, text in [('backend_single_tb', 'BACKEND PASS'), ('csr_single_tb', 'CSR PASS'), ('build', 'built'), ('synth', 'mapped'), ('lint', '')]:
        files[f'out/single_lane/{name}.log'] = text.encode()
    files['out/single_lane/spike_core.log'] = b'RESET'+b'\nEVENT x'*1056
    files['out/single_lane/obj_dir/core_check'] = b'executable'
    put('out/single_lane/synth.json', {'modules': {'single_lane_core': {'cells': {'a': {'type': '$_AND_'}}}}})
    act = dict(schema=1, inputs=inputs, architectural_slice_accepted=False, selected_tests=19, unexpected_skips=0,
               upstream={}, deferred_tests=act_config['deferred_tests'], runs=[],
               sail_sha256=sail_config['sail_binary_sha256'], negative_checks=['corrupt_signature', 'fail', 'timeout'])
    for test in act_config['selected_tests']:
        elf = str(Path(test).with_suffix('.elf'))
        files['external/act4/tests/'+test] = b'upstream test'
        act['upstream'][test] = sha(b'upstream test')
        files['out/act/work/spec_ooo_rv32i/elfs/'+elf] = b'ELF'
        for seed in contract['seeds']:
            act['runs'].append(dict(test=elf, seed=seed, events=100, cycles=1500, elf_sha256=sha(b'ELF')))
            files[f'out/act/{Path(test).stem}_{seed}.log'] = b'ACT4 PASS events=100 cycles=1500'
    for name in act['negative_checks']:
        files[f'out/act/{name}.log'] = b'ACT4 instruction limit without tohost completion' if name == 'timeout' else b'ACT4 FAIL tohost=3'
    sail = dict(schema=1, inputs=inputs, architectural_slice_accepted=False, sail_sha256=act['sail_sha256'],
                limitations=sail_config['limitations'], sail_override=sail_config['sail_override'],
                core_binary_sha256=sha(b'executable'), runs=[], mutations_detected=['load_sign', 'trap_value'])
    sail_log = ''
    for seed in contract['seeds']:
        for mode in ['normal']+(['reset_fetch', 'reset_data', 'reset_commit', 'stall_store'] if seed == 42 else []):
            for path in (f'{seed}.elf', f'{seed}.trace', f'core_{seed}_{mode}.log'):
                files['out/sail/'+path] = b'artifact'
            sail['runs'].append(dict(seed=seed, mode=mode, compared_events=1200, bootstrap_events_per_reset=6,
                                    coverage=contract['coverage'], elf_sha256=sha(b'artifact'),
                                    sail_trace_sha256=sha(b'artifact'), rtl_log_sha256=sha(b'artifact')))
            sail_log += f'Sail differential seed={seed} {mode}: PASS (1200 events)\n'
    for name in sail['mutations_detected']:
        files[f'out/sail/{name}.log'] = b'mutation trace'
        sail_log += f'Sail RTL mutation {name}: PASS (detected)\n'
    data = dict(core=core, act4=act, sail=sail, a1=dict(schema=1, inputs_sha256=inputs, status='pass', physical_gate='PHY0'))
    receipts = dict(core='out/single_lane/receipt.json', act4='out/act/receipt.json', sail='out/sail/receipt.json', a1='out/a1/acceptance.json')
    for name, value in data.items():
        put(receipts[name], value)
    logs = ['S0 contracts: PASS\nreference: 25/25 tests passed', 'A1 evidence controls: PASS (16/16 rejected)',
            'Core Spike differential: PASS (1056 events)\nCore mutation wrong_alu: PASS (detected)\nCore mutation early_store: PASS (detected)', 'ACT4 passed', sail_log]
    steps = []
    for n, command in enumerate(contract['commands']):
        path = f'commands/{n}.log'
        files[path] = logs[n].encode()
        steps.append(dict(command=command, log=path, duration_seconds=1.0, returncode=0))
    files['source.patch'] = b'patch'
    manifest = dict(schema=1, gate='Architectural Slice', status='pass', profile=contract['profile'], claim_class=contract['claim_class'],
                    limitations=contract['limitations'], source_revision='a'*40, dirty_tree=True, patch_sha256=sha(b'patch'),
                    platform_sha256=sha(files['config/platform.yaml']), command=['make', 'architectural-slice-check'],
                    started_utc='2026-09-07T00:00:00+00:00', finished_utc='2026-09-07T00:01:00+00:00', steps=steps, receipts=receipts)
    return manifest, files


class AcceptanceTests(unittest.TestCase):
    def setUp(self):
        self.manifest, self.files = fixture()

    def test_complete_matrix(self):
        self.assertEqual(evaluate(self.manifest, self.files.__getitem__)['act4_runs'], 57)

    def test_child_evidence_controls(self):
        mutations = [
            ('core', lambda d: d['cases'].pop()),
            ('core', lambda d: d['cases'][0].update(checked_events=99999)),
            ('core', lambda d: d['cases'][-1].update(expected_fatal=False)),
            ('core', lambda d: d['mutations_detected'].pop()),
            ('core', lambda d: d.update(synthesis={'$_DLATCH_': 1})),
            ('core', lambda d: d['inputs_sha256'].update({next(iter(d['inputs_sha256'])): '0'*64})),
            ('core', lambda d: d['inputs_sha256'].pop(next(iter(d['inputs_sha256'])))),
            ('act4', lambda d: d['runs'].pop()),
            ('act4', lambda d: d.update(unexpected_skips=1)),
            ('act4', lambda d: d.update(deferred_tests=[])),
            ('act4', lambda d: d['negative_checks'].pop()),
            ('act4', lambda d: d['runs'][0].update(elf_sha256='0'*64)),
            ('sail', lambda d: d['runs'].pop()),
            ('sail', lambda d: d['runs'][0]['coverage']['branch_forms'].pop()),
            ('sail', lambda d: d['runs'][0].update(compared_events=1)),
            ('sail', lambda d: d['runs'][0].update(rtl_log_sha256='0'*64)),
            ('sail', lambda d: d['runs'][0].update(bootstrap_events_per_reset=0)),
            ('sail', lambda d: d.update(limitations=[])),
            ('sail', lambda d: d['mutations_detected'].pop()),
            ('a1', lambda d: d.update(physical_gate='closed')),
        ]
        for index, (name, mutate) in enumerate(mutations):
            with self.subTest(control=index):
                files = dict(self.files)
                path = self.manifest['receipts'][name]
                data = json.loads(files[path]); mutate(data)
                files[path] = json.dumps(data).encode()
                with self.assertRaises(ValueError):
                    evaluate(self.manifest, files.__getitem__)

    def test_failed_or_missing_command(self):
        for mutate in (lambda m: m['steps'].pop(), lambda m: m['steps'][0].update(returncode=1),
                       lambda m: m['steps'][0].update(duration_seconds=float('nan')),
                       lambda m: m.update(claim_class='physical_closure')):
            manifest = copy.deepcopy(self.manifest); mutate(manifest)
            with self.assertRaises(ValueError):
                evaluate(manifest, self.files.__getitem__)

    def test_archive_and_source_integrity(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sources = [CONTRACT, 'tools/check_architectural_slice.py']
            for name in sources:
                (root/name).parent.mkdir(parents=True, exist_ok=True)
                (root/name).write_bytes(self.files[name])
            manifest = dict(self.manifest, inputs_sha256={p: sha(self.files[p]) for p in sources},
                            artifacts_sha256={p: sha(b) for p, b in self.files.items()})
            raw = json.dumps(manifest).encode()
            archive = root/'raw.tar.gz'
            with tarfile.open(archive, 'w:gz') as tar:
                for name, data in {**self.files, 'manifest.json': raw}.items():
                    info = tarfile.TarInfo(name); info.size = len(data)
                    tar.addfile(info, io.BytesIO(data))
            receipt = {k: v for k, v in self.manifest.items() if k not in ('steps', 'receipts')}
            receipt.update(archive=archive.name, archive_sha256=sha(archive.read_bytes()), manifest_sha256=sha(raw),
                           metrics=evaluate(self.manifest, self.files.__getitem__))
            path = root/'receipt.json'
            path.write_text(json.dumps(receipt))
            with patch('tools.check_architectural_slice.source_files', return_value=sources):
                verify(path, root)
                altered = dict(receipt, claim_class='physical_closure')
                path.write_text(json.dumps(altered))
                with self.assertRaisesRegex(ValueError, 'metadata mismatch'):
                    verify(path, root)
                path.write_text(json.dumps(receipt))
                (root/CONTRACT).write_text('changed')
                with self.assertRaisesRegex(ValueError, 'stale source'):
                    verify(path, root)
                (root/CONTRACT).write_bytes(self.files[CONTRACT])
                archive.write_bytes(archive.read_bytes()+b'corruption')
                with self.assertRaisesRegex(ValueError, 'archive hash mismatch'):
                    verify(path, root)


if __name__ == '__main__':
    unittest.main()
