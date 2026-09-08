#!/usr/bin/env python3
"""Check full-sized serialized rename ownership with bounded formal jobs."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time

from run_assert_portability import digest, require

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--suite', type=Path, default=Path.home()/'tools/oss-cad-suite-20260905/oss-cad-suite')
    args = parser.parse_args()
    suite = args.suite.resolve()
    config = json.loads((ROOT/'config/rename_ownership.json').read_text())
    require((config['physical_registers'], config['architectural_registers'], config['maximum_inflight']) == (64, 32, 1), 'unsupported geometry')
    require(config['mode'] == 'bmc' and config['engine'] == 'abc bmc3'
            and config['cover_engine'] == 'smtbmc yices', 'unsupported proof mode')
    lock = json.loads((ROOT/'config/formal_tools.lock').read_text())
    synthesis = json.loads((ROOT/lock['synthesis_lock']).read_text())
    for name, value in {**synthesis['sha256'], **lock['sha256']}.items():
        require((suite/name).is_file() and digest(suite/name) == value, f'tool pin differs: {name}')
    require(digest(Path('/usr/bin/time')) == lock['time_sha256'], 'resource measurement tool differs')
    versions = {}
    for name, expected in {**lock['versions'], 'yosys': synthesis['yosys_version']}.items():
        versions[name] = subprocess.check_output([str(suite/'bin'/name), '--version'], text=True).splitlines()[0]
        require(versions[name] == expected, f'{name} version differs')
    paths = [*config['sources'], 'config/rename_ownership.json', 'config/formal_tools.lock',
             lock['synthesis_lock'], 'tools/run_rename_ownership.py', 'tools/run_assert_portability.py']
    inputs = {p: digest(ROOT/p) for p in paths}
    run_hash = hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest()[:16]
    out = ROOT/'out/rename_ownership'/run_hash
    out.mkdir(parents=True, exist_ok=True)
    receipt = out/'receipt.json'
    receipt.unlink(missing_ok=True)
    started = datetime.now(timezone.utc).isoformat()
    env = dict(os.environ, PATH=str(suite/'bin')+os.pathsep+os.environ['PATH'])
    jobs = []
    variants = [dict(name='covers', mode='cover')]
    variants += [dict(m, mode='bmc') for m in config['mutations']]
    variants.append(dict(name='baseline', mode='bmc'))
    for variant in variants:
        name = variant['name']
        directory = out/name
        directory.mkdir(exist_ok=True)
        sources = [ROOT/p for p in config['sources']]
        if 'old' in variant:
            original = sources[2].read_text()
            require(original.count(variant['old']) == 1, f'mutation no longer matches: {name}')
            changed = directory/'rename_single.sv'
            changed.write_text('`define SYNTHESIS\n'+original.replace(variant['old'], variant['new'])+'\n`undef SYNTHESIS\n')
            sources[2] = changed
        task = directory/'check.sby'
        engine = config['cover_engine'] if variant['mode'] == 'cover' else config['engine']
        task.write_text(f'[options]\nmode {variant["mode"]}\ndepth {config["depth"]}\n\n'
                        f'[engines]\n{engine}\n\n[script]\nplugin -i slang\n'
                        f'read_slang --no-synthesis-define --top {config["top"]} '+ ' '.join(p.name for p in sources)+'\n'
                        f'prep -top {config["top"]}\n\n[files]\n'+'\n'.join(str(p) for p in sources)+'\n')
        proof = directory/'proof'
        log = directory/'check.log'
        rss = directory/'memory.rss'
        command = ['/usr/bin/time', '-f', '%M', '-o', str(rss), str(suite/'bin/sby'), '-f', '-d', str(proof), str(task)]
        print(f'Rename ownership {name}: running {variant["mode"]} depth={config["depth"]}', flush=True)
        before = time.monotonic()
        with log.open('w') as stream:
            child = subprocess.Popen(command, cwd=ROOT, env=env, stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                code = child.wait(timeout=config['timeout_seconds'])
            except (subprocess.TimeoutExpired, KeyboardInterrupt):
                os.killpg(child.pid, signal.SIGKILL)
                child.wait()
                raise RuntimeError(f'{name} interrupted or timed out; see {log}')
        output = log.read_text()
        status = (proof/'status').read_text().split()[0] if (proof/'status').is_file() else 'ERROR'
        mutant = 'old' in variant
        require(status == ('FAIL' if mutant else 'PASS') and ((code != 0) if mutant else (code == 0)), f'{name}: unexpected status {status}; see {log}')
        summary = (proof/status).read_text()
        assertions = []
        for filename, line in re.findall(r'failed assertion \S+ at (\w+\.sv):(\d+)\.', summary):
            source = next(p for p in sources if p.name == filename)
            label = re.search(r'(\w+):\s*assert\b', source.read_text().splitlines()[int(line)-1])
            assertions.append(label[1] if label else f'{filename}:{line}')
        traces = sorted(str(p.relative_to(ROOT)) for p in (proof/'engine_0').glob('trace*.vcd'))
        if mutant:
            require(any(expected in assertions for expected in variant['assertions']), f'{name}: wrong assertion failed: {assertions}')
            require(traces, f'{name}: missing counterexample')
        reached = re.findall(r'reached cover statement rename_ownership\.(\w+)', summary)
        if variant['mode'] == 'cover':
            require(set(reached) == set(config['required_covers']) and traces, 'incomplete required covers')
        jobs.append(dict(name=name, mode=variant['mode'], engine=engine, depth=config['depth'], status=status,
                         expected_failure=mutant, command=command, duration_seconds=round(time.monotonic()-before, 6),
                         max_rss_kib=int(rss.read_text().splitlines()[-1]), log=str(log.relative_to(ROOT)),
                         assertions_failed=assertions, covers_reached=reached, traces=traces,
                         dut_assertions_enabled=not mutant))
        print(f'Rename ownership {name}: PASS ({"fault detected" if mutant else status})', flush=True)
    require(all(digest(ROOT/p) == value for p, value in inputs.items()), 'inputs changed during run')
    artifacts = {str(p.relative_to(ROOT)): digest(p) for p in out.rglob('*') if p.is_file()}
    result = dict(schema=1, status='pass', profile=config['profile'], claim=config['claim'],
                  source_revision=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                  dirty_tree=bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT)),
                  started_utc=started, finished_utc=datetime.now(timezone.utc).isoformat(), versions=versions,
                  inputs_sha256=inputs, artifacts_sha256=artifacts, assumptions=config['assumptions'],
                  environment=config['environment'], jobs=jobs, formal_readiness_accepted=False)
    receipt.write_text(json.dumps(result, indent=2)+'\n')
    print(f'Rename ownership smoke: PASS; receipt {receipt.relative_to(ROOT)}')


if __name__ == '__main__':
    main()
