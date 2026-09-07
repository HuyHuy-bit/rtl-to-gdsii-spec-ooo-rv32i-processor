#!/usr/bin/env python3
"""Check shared assertion syntax, sampling, reset, and mutation detection."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import resource
import re
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--suite', type=Path, default=Path.home()/'tools/oss-cad-suite-20260905/oss-cad-suite')
    args = parser.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    suite = args.suite.resolve()
    config = json.loads((ROOT/'config/assertion_portability.json').read_text())
    require([r['mutation'] for r in config['support_matrix']] == list(range(1, 7)), 'incomplete support matrix')
    require(config['formal_engine'] == 'smtbmc yices', 'engine is not pinned')
    lock = json.loads((ROOT/'config/formal_tools.lock').read_text())
    synthesis = json.loads((ROOT/lock['synthesis_lock']).read_text())
    for name, expected in {**synthesis['sha256'], **lock['sha256']}.items():
        require((suite/name).is_file() and digest(suite/name) == expected, f'formal tool pin differs: {name}')
    require(digest(Path('/usr/bin/time')) == lock['time_sha256'], 'resource measurement tool differs')
    versions = {}
    for name, expected in {**lock['versions'], 'yosys': synthesis['yosys_version']}.items():
        version = subprocess.check_output([str(suite/'bin'/name), '--version'], text=True).splitlines()[0]
        require(version == expected, f'{name} version differs')
        versions[name] = version
    toolchain = json.loads((ROOT/'config/toolchain.lock').read_text())
    versions['verilator'] = subprocess.check_output(['verilator', '--version'], text=True).splitlines()[0]
    require(versions['verilator'] == next(t['expected_first_line'] for t in toolchain['tools'] if t['name'] == 'verilator'), 'Verilator version differs')
    paths = [*config['sources'], 'config/assertion_portability.json', 'config/formal_tools.lock',
             lock['synthesis_lock'], 'config/toolchain.lock', 'tools/run_assert_portability.py']
    inputs = {p: digest(ROOT/p) for p in paths}
    run_hash = hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest()[:16]
    out = ROOT/'out/assert_portability'/run_hash
    out.mkdir(parents=True, exist_ok=True)
    receipt = out/'receipt.json'
    receipt.unlink(missing_ok=True)
    started = datetime.now(timezone.utc).isoformat()
    commands = []
    env = dict(os.environ, PATH=str(suite/'bin')+os.pathsep+os.environ['PATH'])

    def run(command, log, expected=None, formal=False):
        command = [str(c) for c in command]
        memory_log = log.with_suffix('.rss')
        command = ['/usr/bin/time', '-f', '%M', '-o', str(memory_log), *command]
        before = time.monotonic()
        result = subprocess.run(command, cwd=ROOT, env=env if formal else None,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                timeout=config['timeout_seconds'])
        log.write_text(result.stdout)
        commands.append(dict(command=command, returncode=result.returncode,
                             duration_seconds=round(time.monotonic()-before, 6),
                             max_rss_kib=int(memory_log.read_text().splitlines()[-1]), log=str(log.relative_to(ROOT))))
        if expected:
            require(result.returncode != 0 and expected in result.stdout, f'expected failure missing: {expected}; see {log}')
        else:
            require(result.returncode == 0, f'command failed; see {log}\n{result.stdout[-2500:]}')
        return result.stdout

    rows = [dict(mutation=0, marker=None, assertion=None), *config['support_matrix']]
    simulation = []
    proofs = []
    for row in rows:
        mutation = row['mutation']
        directory = out/f'case_{mutation}'
        directory.mkdir(exist_ok=True)
        run(['verilator', '--binary', '--timing', '--assert', '--Wall', '--build-jobs', '2',
             '--top-module', 'portability_tb', f'-DMUTATION={mutation}', '--Mdir', directory/'obj_dir',
             *config['sources'][:2], '-o', 'portability_check'], directory/'build.log')
        output = run([directory/'obj_dir/portability_check'], directory/'simulation.log', row['marker'])
        if mutation == 0:
            require(f'PORTABILITY PASS cycles={config["simulation_cycles"]} reset_pulses=2' in output, 'simulation did not finish')
        simulation.append(dict(mutation=mutation, result='detected' if mutation else 'pass', marker=row['marker']))
        mode = 'bmc' if mutation else 'prove'
        task = directory/'proof.sby'
        task.write_text(f'[options]\nmode {mode}\ndepth {config["formal_depth"]}\n\n'
                        f'[engines]\n{config["formal_engine"]}\n\n'
                        '[script]\nplugin -i slang\n'
                        f'read_slang --no-synthesis-define --top portability_formal -D MUTATION={mutation} assert_portability.sv portability_formal.sv\n'
                        'prep -top portability_formal\n\n[files]\n'
                        + '\n'.join(str(ROOT/p) for p in (config['sources'][0], config['sources'][2]))+'\n')
        proof = directory/'proof'
        output = run([suite/'bin/sby', '-f', '-d', proof, task], directory/'formal.log',
                     'DONE (FAIL' if mutation else None, formal=True)
        status = (proof/'status').read_text().split()[0]
        require(status == ('FAIL' if mutation else 'PASS'), 'unexpected formal status')
        if mutation:
            require(row['assertion'] in output and 'Assert failed' in output, 'wrong formal assertion failed')
            require((proof/'engine_0/trace_tb.v').is_file() and (proof/'engine_0/trace.vcd').is_file(), 'missing counterexample')
        else:
            require('successful proof by k-induction' in output, 'missing inductive proof')
        proofs.append(dict(mutation=mutation, mode=mode, status=status, assertion=row['assertion']))
        print(f'Assertion portability case={mutation}: simulation/formal PASS ({"fault detected" if mutation else "inductive proof"})', flush=True)

    task = out/'cover.sby'
    task.write_text((out/'case_0/proof.sby').read_text().replace('mode prove', 'mode cover'))
    cover = out/'cover'
    output = run([suite/'bin/sby', '-f', '-d', cover, task], out/'cover.log', formal=True)
    require((cover/'status').read_text().split()[0] == 'PASS', 'cover failed')
    reached = set(re.findall(r'reached cover statement portability_formal\.(\w+)', output))
    require(reached == set(config['required_covers']), 'required cover not reached')
    require(list((cover/'engine_0').glob('trace*.vcd')), 'missing cover traces')
    require(all(digest(ROOT/p) == value for p, value in inputs.items()), 'input changed during run')
    artifacts = {str(p.relative_to(ROOT)): digest(p) for p in out.rglob('*')
                 if p.is_file() and not any(part.startswith('obj_dir') for part in p.parts)}
    result = dict(schema=1, status='pass', profile=config['profile'], claim=config['claim'],
                  source_revision=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                  dirty_tree=bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT)),
                  started_utc=started, finished_utc=datetime.now(timezone.utc).isoformat(),
                  versions=versions, inputs_sha256=inputs, commands=commands,
                  simulation=simulation, formal=proofs, covers=config['required_covers'],
                  engine=config['formal_engine'], depth=config['formal_depth'],
                  assumptions=config['formal_assumptions'], artifacts_sha256=artifacts,
                  formal_readiness_accepted=False)
    receipt.write_text(json.dumps(result, indent=2)+'\n')
    print(f'Assertion portability: PASS (6/6 mutations detected by both tools; 3/3 covers reached)\nReceipt: {receipt.relative_to(ROOT)}')


if __name__ == '__main__':
    main()
