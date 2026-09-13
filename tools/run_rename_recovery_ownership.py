#!/usr/bin/env python3
"""Check connected two-wide rename ownership with induction, covers and mutation checks."""
import argparse
from datetime import datetime, timezone
import hashlib
import io
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import tarfile
import time

from run_assert_portability import digest, require

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--suite', type=Path, default=Path.home()/'tools/oss-cad-suite-20260905/oss-cad-suite')
    args = parser.parse_args()
    suite = args.suite.resolve()
    config = json.loads((ROOT/'config/rename_recovery_ownership.json').read_text())
    require((config['physical_registers'], config['architectural_registers'], config['maximum_inflight']) == (64, 32, 4), 'unsupported geometry')
    require(config['mode'] == 'prove' and config['engine'] == 'smtbmc bitwuzla'
            and config['cover_engine'] == config['mutation_engine'] == 'smtbmc yices', 'unsupported proof mode')
    require(config['checkpoints'] == 8 and config['workload_registers'] == [0, 1, 2, 3], 'unsupported workload')
    require((config['depth'], config['cover_depth'], config['mutation_depth'], config['timeout_seconds']) == (3, 12, 12, 600), 'unsupported bound or timeout')
    lock = json.loads((ROOT/'config/formal_tools.lock').read_text())
    synthesis = json.loads((ROOT/lock['synthesis_lock']).read_text())
    extra = config['additional_solver']
    require(extra['name'] == 'bitwuzla' and set(extra['sha256']) == {'bin/bitwuzla'}, 'unsupported solver pin')
    for name, value in {**synthesis['sha256'], **lock['sha256'], **extra['sha256']}.items():
        require((suite/name).is_file() and digest(suite/name) == value, f'tool pin differs: {name}')
    require(digest(Path('/usr/bin/time')) == lock['time_sha256'], 'resource measurement tool differs')
    versions = {}
    for name, expected in {**lock['versions'], 'yosys': synthesis['yosys_version'], extra['name']: extra['version']}.items():
        versions[name] = subprocess.check_output([str(suite/'bin'/name), '--version'], text=True).splitlines()[0]
        require(versions[name] == expected, f'{name} version differs')
    toolchain = json.loads((ROOT/'config/toolchain.lock').read_text())
    versions['verilator'] = subprocess.check_output(['verilator', '--version'], text=True).splitlines()[0]
    require(versions['verilator'] == next(t['expected_first_line'] for t in toolchain['tools'] if t['name'] == 'verilator'), 'Verilator version differs')
    harness = (ROOT/config['sources'][-1]).read_text()
    require(set(re.findall(r'(\w+): cover', harness)) == set(config['required_covers']), 'cover contract differs')
    paths = [*config['sources'], 'config/rename_recovery_ownership.json', 'config/formal_tools.lock',
             lock['synthesis_lock'], 'tools/run_rename_recovery_ownership.py', 'tools/run_assert_portability.py',
             'Makefile', 'config/toolchain.lock', 'config/rename_recovery.json']
    inputs = {p: digest(ROOT/p) for p in paths}
    run_hash = hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest()[:16]
    run_id = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    out = ROOT/'out/rename_recovery_ownership'/run_hash/run_id
    out.mkdir(parents=True, exist_ok=True)
    receipt = out/'receipt.json'
    receipt.unlink(missing_ok=True)
    started = datetime.now(timezone.utc).isoformat()
    env = dict(os.environ, PATH=str(suite/'bin')+os.pathsep+os.environ['PATH'])
    lint_command = ['verilator', '--lint-only', '--assert', '-Wall', '--top-module', config['top'], *config['sources']]
    with (out/'lint.log').open('w') as stream:
        subprocess.run(lint_command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT, check=True, timeout=60)
    jobs = []
    variants = [dict(name='covers', mode='cover', depth=config['cover_depth'])]
    variants += [dict(m, mode='bmc', depth=config['mutation_depth']) for m in config['mutations']]
    variants.append(dict(name='baseline', mode=config['mode'], depth=config['depth']))
    for variant in variants:
        name = variant['name']
        directory = out/name
        directory.mkdir(exist_ok=True)
        sources = [ROOT/p for p in config['sources']]
        if 'old' in variant:
            sources = []
            for path in config['sources']:
                source = ROOT/path
                if path.startswith('rtl/'):
                    original = source.read_text()
                    if path == variant['source']:
                        require(original.count(variant['old']) == 1, f'mutation no longer matches: {name}')
                        original = original.replace(variant['old'], variant['new'])
                    source = directory/source.name
                    source.write_text('`define SYNTHESIS\n'+original+'\n`undef SYNTHESIS\n')
                sources.append(source)
        task = directory/'check.sby'
        engine = config['cover_engine'] if variant['mode'] == 'cover' else (
            config['mutation_engine'] if 'old' in variant else config['engine'])
        lowering = 'memory_map\nopt_clean\n'
        task.write_text(f'[options]\nmode {variant["mode"]}\ndepth {variant["depth"]}\n\n'
                        f'[engines]\n{engine}\n\n[script]\nplugin -i slang\n'
                        f'read_slang --no-synthesis-define --top {config["top"]} '+ ' '.join(p.name for p in sources)+'\n'
                        f'prep -top {config["top"]}\n{lowering}\n[files]\n'+'\n'.join(str(p) for p in sources)+'\n')
        proof = directory/'proof'
        log = directory/'check.log'
        rss = directory/'memory.rss'
        command = ['/usr/bin/time', '-f', '%M', '-o', str(rss), str(suite/'bin/sby'), '-f', '-d', str(proof), str(task)]
        print(f'Rename recovery ownership {name}: running {variant["mode"]} depth={variant["depth"]}', flush=True)
        before = time.monotonic()
        with log.open('w') as stream:
            child = subprocess.Popen(command, cwd=ROOT, env=env, stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                code = child.wait(timeout=config['timeout_seconds'])
            except (subprocess.TimeoutExpired, KeyboardInterrupt):
                os.killpg(child.pid, signal.SIGKILL)
                child.wait()
                raise RuntimeError(f'{name} interrupted or timed out; see {log}')
        status = (proof/'status').read_text().split()[0] if (proof/'status').is_file() else 'ERROR'
        mutant = 'old' in variant
        require(status == ('FAIL' if mutant else 'PASS') and ((code != 0) if mutant else (code == 0)), f'{name}: unexpected status {status}; see {log}')
        summary = (proof/status).read_text()
        assertions = []
        for filename, line in re.findall(r'failed assertion \S+ at (\w+\.sv):(\d+)\.', summary):
            source = next(p for p in sources if p.name == filename)
            label = re.search(r'(\w+):\s*assert\b', source.read_text().splitlines()[int(line)-1])
            assertions.append(label[1] if label else f'{filename}:{line}')
        engine_log = '\n'.join(p.read_text() for p in sorted((proof/'engine_0').glob('logfile*.txt')))
        assertions += [label.rsplit('.', 1)[-1] for label in re.findall(r'Assert failed in rename_recovery_ownership: (\S+)', engine_log)]
        assertions = sorted(set(assertions))
        traces = sorted(str(p.relative_to(ROOT)) for p in (proof/'engine_0').glob('trace*.vcd'))
        if mutant:
            require(any(expected in assertions for expected in variant['assertions']), f'{name}: wrong assertion failed: {assertions}')
            require(traces, f'{name}: missing counterexample')
        reached = re.findall(r'reached cover statement rename_recovery_ownership\.(\w+)', summary)
        reached += re.findall(r'Reached cover statement in step \d+ at rename_recovery_ownership: (\w+)', engine_log)
        reached = sorted(set(reached))
        if variant['mode'] == 'prove':
            require('successful proof by k-induction' in summary and 'Temporal induction successful.' in engine_log, 'incomplete inductive proof')
        if variant['mode'] == 'cover':
            require(set(reached) == set(config['required_covers']) and traces, 'incomplete required covers')
        jobs.append(dict(name=name, mode=variant['mode'], engine=engine, depth=variant['depth'], status=status,
                         expected_failure=mutant, command=command, duration_seconds=round(time.monotonic()-before, 6),
                         max_rss_kib=int(rss.read_text().splitlines()[-1]), log=str(log.relative_to(ROOT)),
                         assertions_failed=assertions, covers_reached=reached, traces=traces,
                         dut_assertions_enabled=not mutant))
        print(f'Rename recovery ownership {name}: PASS ({"fault detected" if mutant else status})', flush=True)
    require(all(digest(ROOT/p) == value for p, value in inputs.items()), 'inputs changed during run')
    artifacts = {str(p.relative_to(ROOT)): digest(p) for p in out.rglob('*') if p.is_file()}
    result = dict(schema=1, status='pass', profile=config['profile'], claim=config['claim'],
                  source_revision=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                  dirty_tree=bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT)),
                  started_utc=started, finished_utc=datetime.now(timezone.utc).isoformat(), versions=versions,
                  inputs_sha256=inputs, artifacts_sha256=artifacts, assumptions=config['assumptions'],
                  environment=config['environment'], state_invariants=config['state_invariants'], jobs=jobs, formal_readiness_accepted=False, maximum_inflight=config['maximum_inflight'],
                  workload_registers=config['workload_registers'], lint_command=lint_command)
    receipt.write_text(json.dumps(result, indent=2)+'\n')
    files = {**inputs, **artifacts}
    archive = ROOT/f'evidence/rename_recovery_ownership/{run_id}.tar.gz'
    archive.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive, 'w:gz') as bundle:
        for name, expected in sorted(files.items()):
            data = (ROOT/name).read_bytes()
            require(hashlib.sha256(data).hexdigest() == expected, f'artifact changed before retention: {name}')
            info = tarfile.TarInfo(name)
            info.size, info.mode = len(data), 0o644
            bundle.addfile(info, io.BytesIO(data))
        data = receipt.read_bytes()
        info = tarfile.TarInfo('manifest.json')
        info.size, info.mode = len(data), 0o644
        bundle.addfile(info, io.BytesIO(data))
    retained = dict(schema=1, status='pass', profile=config['profile'], claim=config['claim'],
                    archive=str(archive.relative_to(ROOT)), archive_sha256=digest(archive),
                    manifest_sha256=digest(receipt), inputs_sha256=inputs, formal_readiness_accepted=False)
    retained_path = ROOT/'evidence/receipts/rename_recovery_ownership.json'
    retained_path.write_text(json.dumps(retained, indent=2)+'\n')
    print(f'Rename recovery ownership: PASS; receipt {receipt.relative_to(ROOT)}; retained {retained_path.relative_to(ROOT)}')


if __name__ == '__main__':
    main()
