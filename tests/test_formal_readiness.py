import copy
import json
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch

from tools.check_formal_readiness import CONTRACT, RECEIPT, ROOT, evaluate, sha, verify


def retained():
    receipt = json.loads((ROOT/RECEIPT).read_text())
    with tarfile.open(ROOT/receipt['archive'], 'r:gz') as tar:
        files = {m.name: tar.extractfile(m).read() for m in tar.getmembers()}
    manifest = json.loads(files.pop('manifest.json'))
    return receipt, manifest, files


class FormalReadinessTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.receipt, cls.manifest, cls.files = retained()

    def test_complete_evidence(self):
        metrics = evaluate(self.manifest, self.files.__getitem__)
        self.assertEqual(metrics['rename_bmc_depth'], 12)
        self.assertEqual(metrics['portability_covers'] + metrics['rename_covers'], 12)
        self.assertEqual(metrics['measured_commands'], 29)

    def test_child_rejection_controls(self):
        controls = [
            ('portability', lambda d: d.update(status='fail')),
            ('portability', lambda d: d.update(depth=1)),
            ('portability', lambda d: d['inputs_sha256'].pop('config/formal_tools.lock')),
            ('portability', lambda d: d['versions'].update(verilator='unpinned')),
            ('portability', lambda d: d['formal'][0].update(mode='bmc')),
            ('portability', lambda d: d['formal'][1].update(status='PASS')),
            ('portability', lambda d: d['simulation'][1].update(result='pass')),
            ('portability', lambda d: d['commands'].pop()),
            ('portability', lambda d: d['commands'][1].update(returncode=1)),
            ('portability', lambda d: d['commands'][4].update(returncode=0)),
            ('portability', lambda d: d['commands'][0].update(duration_seconds=float('nan'))),
            ('portability', lambda d: d['commands'][0].update(max_rss_kib=0)),
            ('portability', lambda d: d['covers'].pop()),
            ('rename', lambda d: d.update(formal_readiness_accepted=True)),
            ('rename', lambda d: d['jobs'].pop()),
            ('rename', lambda d: d['jobs'][-1].update(depth=1)),
            ('rename', lambda d: d['jobs'][-1].update(status='UNKNOWN')),
            ('rename', lambda d: d['jobs'][1].update(dut_assertions_enabled=True)),
            ('rename', lambda d: d['jobs'][1].update(assertions_failed=['wrong_property'])),
            ('rename', lambda d: d['jobs'][1].update(traces=[])),
            ('rename', lambda d: d['jobs'][0].update(covers_reached=[])),
            ('rename', lambda d: d.update(assumptions=['Assume the DUT is correct.'])),
            ('rename', lambda d: d['inputs_sha256'].update({'rtl/backend/rename_single.sv': '0'*64})),
        ]
        for kind, mutate in controls:
            with self.subTest(kind=kind, control=controls.index((kind, mutate))):
                files = dict(self.files)
                path = self.manifest['receipts'][kind]
                data = json.loads(files[path])
                mutate(data)
                files[path] = json.dumps(data).encode()
                with self.assertRaises(ValueError):
                    evaluate(self.manifest, files.__getitem__)

    def test_raw_evidence_rejection_controls(self):
        rb = str(Path(self.manifest['receipts']['rename']).parent)
        pb = str(Path(self.manifest['receipts']['portability']).parent)
        controls = [
            ('rename', rb+'/baseline/check.sby', lambda b: b.replace(b'depth 12', b'depth 1')),
            ('rename', rb+'/baseline/check.sby', lambda b: b.replace(b'--no-synthesis-define ', b'')),
            ('rename', rb+'/baseline/proof/src/rename_ownership.sv', lambda b: b.replace(b'assert', b'assume')),
            ('rename', rb+'/covers/proof/PASS', lambda b: b.replace(b'reached cover statement', b'omitted cover statement', 1)),
            ('rename', rb+'/premature_ready/proof/FAIL', lambda b: b.replace(b'failed assertion', b'unrelated failure')),
            ('rename', rb+'/premature_ready/rename_single.sv', lambda b: b.replace(b'`define SYNTHESIS', b'')),
            ('rename', rb+'/live_register_free/proof/engine_0/trace.vcd', lambda b: b''),
            ('rename', rb+'/baseline/proof/status', lambda b: b'ERROR\n'),
            ('portability', pb+'/case_0/formal.log', lambda b: b.replace(b'successful proof by k-induction', b'bounded pass only')),
            ('portability', pb+'/case_1/simulation.log', lambda b: b.replace(b'PORT_DUPLICATE', b'OTHER_ERROR')),
        ]
        for kind, path, mutate in controls:
            with self.subTest(path=path):
                files = dict(self.files)
                files[path] = mutate(files[path])
                child_path = self.manifest['receipts'][kind]
                child = json.loads(files[child_path])
                child['artifacts_sha256'][path] = sha(files[path])
                files[child_path] = json.dumps(child).encode()
                with self.assertRaises(ValueError):
                    evaluate(self.manifest, files.__getitem__)
        for path in (rb+'/baseline/proof/status', rb+'/live_register_free/proof/engine_0/trace.vcd'):
            with self.subTest(missing=path):
                files = dict(self.files)
                child_path = self.manifest['receipts']['rename']
                child = json.loads(files[child_path])
                child['artifacts_sha256'].pop(path)
                del files[path]
                files[child_path] = json.dumps(child).encode()
                with self.assertRaises(ValueError):
                    evaluate(self.manifest, files.__getitem__)

    def test_gate_rejection_controls(self):
        for mutate in (lambda m: m['steps'].pop(), lambda m: m['steps'][0].update(returncode=1),
                       lambda m: m.update(claim='Unbounded dual-lane processor proof'),
                       lambda m: m.update(patch_sha256='0'*64)):
            manifest = copy.deepcopy(self.manifest)
            mutate(manifest)
            with self.assertRaises(ValueError):
                evaluate(manifest, self.files.__getitem__)

    def test_archive_without_disposable_outputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            receipt_path = root/'receipt.json'
            receipt_path.write_text(json.dumps(self.receipt))
            archive = root/self.receipt['archive']
            archive.parent.mkdir(parents=True)
            archive.write_bytes((ROOT/self.receipt['archive']).read_bytes())
            for p in self.manifest['inputs_sha256']:
                (root/p).parent.mkdir(parents=True, exist_ok=True)
                (root/p).write_bytes(self.files[p])
            with patch('tools.check_formal_readiness.source_files', return_value=sorted(self.manifest['inputs_sha256'])):
                self.assertEqual(verify(receipt_path, root), self.receipt['metrics'])
                self.assertFalse((root/'out').exists())
                (root/CONTRACT).write_bytes(b'stale input')
                with self.assertRaisesRegex(ValueError, 'stale source'):
                    verify(receipt_path, root)
                (root/CONTRACT).write_bytes(self.files[CONTRACT])
                data = archive.read_bytes()
                archive.write_bytes(data+b'corruption')
                with self.assertRaisesRegex(ValueError, 'archive hash'):
                    verify(receipt_path, root)
                archive.write_bytes(data)
                receipt = dict(self.receipt, metrics={})
                receipt_path.write_text(json.dumps(receipt))
                with self.assertRaisesRegex(ValueError, 'metrics'):
                    verify(receipt_path, root)


if __name__ == '__main__':
    unittest.main()
