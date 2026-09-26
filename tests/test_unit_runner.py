"""Failure-path checks for the shared verification runner."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from tools.run_unit import ROOT, UnitRun, digest, local_headers, mutate, simulation_result
from tools.unit_profiles import PROFILES


class UnitRunnerTests(unittest.TestCase):
    def test_profiles_preserve_accepted_results_and_require_coverage(self):
        for name, profile in PROFILES.items():
            with self.subTest(profile=name):
                config = json.loads((ROOT/f'config/{name}.json').read_text())
                receipt = ROOT/f'evidence/receipts/{name}.json'
                if not receipt.exists():
                    receipt = ROOT/f'out/{name}/receipt.json'
                if not receipt.exists():
                    self.skipTest(f'Local receipt absent; run tools/run_unit.py {name}')
                results = json.loads(receipt.read_text())['simulations']
                for fields in results:
                    output = profile['prefix']+' '.join(f'{k}={v}' for k, v in fields.items())+'\n'
                    with self.subTest(profile=name, seed=fields['seed']):
                        self.assertEqual(simulation_result(output, profile, config, fields['seed']), fields)
                        with self.assertRaises(RuntimeError):
                            simulation_result(output, profile, config, fields['seed']+1)
                        for key in profile['required']:
                            broken = dict(fields, **{key: 0})
                            text = profile['prefix']+' '.join(f'{k}={v}' for k, v in broken.items())+'\n'
                            with self.assertRaises(RuntimeError):
                                simulation_result(text, profile, config, fields['seed'])

    def test_all_mutation_selectors_still_match(self):
        for name, profile in PROFILES.items():
            for mutation, (path, old, new, count) in profile['mutations'].items():
                with self.subTest(profile=name, mutation=mutation):
                    original = (ROOT/path).read_text()
                    self.assertNotEqual(mutate(original, old, new, count), original)
                    with self.assertRaises(RuntimeError):
                        mutate(original, old, new, count+1)

    def test_header_inventory_is_transitive_and_handles_cycles(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root/'test.cpp').write_text('#include "first.hpp"\n#include "generated.hpp"\n')
            (root/'first.hpp').write_text('#include "second.hpp"\n')
            (root/'second.hpp').write_text('#include "first.hpp"\n')
            self.assertEqual(local_headers(root, root/'test.cpp'), {root/'first.hpp', root/'second.hpp'})

    def test_failed_commands_need_nonzero_exit_and_the_expected_marker(self):
        with tempfile.TemporaryDirectory() as directory:
            run = object.__new__(UnitRun)
            run.root = run.out = Path(directory)
            run.profile = {'timeout': 1}
            run.commands, run.artifacts = [], set()
            for code, text, accepted in [(0,'detected',False), (1,'compiler error',False), (1,'detected',True)]:
                with patch('tools.run_unit.subprocess.run', return_value=subprocess.CompletedProcess([],code,text)):
                    if accepted:
                        self.assertEqual(run.run(['unused'], 'test.log', failure='detected'), text)
                    else:
                        with self.assertRaises(RuntimeError):
                            run.run(['unused'], 'test.log', failure='detected')
            with patch('tools.run_unit.subprocess.run', side_effect=subprocess.TimeoutExpired('unused',1)):
                with self.assertRaises(subprocess.TimeoutExpired):
                    run.run(['unused'], 'timeout.log', failure='detected')

    def test_changed_shared_input_cannot_publish_a_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            run = object.__new__(UnitRun)
            run.root = run.out = Path(directory)
            source = run.root/'shared.hpp'
            source.write_text('before')
            run.inputs = {'shared.hpp': digest(source)}
            source.write_text('after')
            with self.assertRaisesRegex(RuntimeError, 'input changed'):
                run.publish({})
            self.assertFalse((run.out/'receipt.json').exists())


if __name__ == '__main__':
    unittest.main()
