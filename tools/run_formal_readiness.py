#!/usr/bin/env python3
"""Run the B0 prerequisites and retain an independently checked evidence bundle."""
import argparse
from datetime import datetime, timezone
import io
import json
import os
from pathlib import Path
import signal
import subprocess
import tarfile
import time

from check_formal_readiness import CONTRACT, RECEIPT, ROOT, evaluate, require, sha, source_files, verify


def stamp():
    return datetime.now(timezone.utc).isoformat()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--suite', type=Path, default=Path.home()/'tools/oss-cad-suite-20260905/oss-cad-suite')
    args = parser.parse_args()
    suite = args.suite.resolve()
    contract = json.loads((ROOT/CONTRACT).read_text())
    destination = ROOT/RECEIPT
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.unlink(missing_ok=True)
    started = stamp()
    run_id = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    work = ROOT/'out/formal_readiness'/run_id
    work.mkdir(parents=True)
    files = {p: (ROOT/p).read_bytes() for p in source_files()}
    inputs = {p: sha(b) for p, b in files.items()}
    revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    dirty = bool(subprocess.check_output(['git', 'status', '--porcelain', '--untracked-files=all', '--', '.', ':!evidence'], cwd=ROOT))
    files['source.patch'] = subprocess.check_output(['git', 'diff', '--binary', 'HEAD', '--', '.', ':!evidence'], cwd=ROOT)
    env = dict(os.environ, CCACHE_DISABLE='1', MAKEFLAGS='', MFLAGS='', OSS_CAD_SUITE=str(suite))
    steps, receipts = [], {}
    for kind, command in zip(('portability', 'rename'), contract['commands']):
        log = work/(kind+'.log')
        print(f'Formal readiness: running {kind}', flush=True)
        before = time.monotonic()
        with log.open('w') as stream:
            child = subprocess.Popen(command, cwd=ROOT, env=env, stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                code = child.wait(timeout=contract['command_timeout_seconds'])
            except (subprocess.TimeoutExpired, KeyboardInterrupt):
                os.killpg(child.pid, signal.SIGKILL)
                child.wait()
                raise RuntimeError(f'{kind} interrupted or timed out; see {log}')
        name = f'commands/{kind}.log'
        files[name] = log.read_bytes()
        steps.append(dict(command=command, returncode=code, log=name, duration_seconds=round(time.monotonic()-before, 6)))
        require(code == 0, f'{kind} failed; see {log}')
        paths = [line.removeprefix('Receipt: ') for line in log.read_text().splitlines() if line.startswith('Receipt: ')]
        if kind == 'rename':
            paths = [line.split('receipt ', 1)[1] for line in log.read_text().splitlines() if line.startswith('Rename ownership smoke: PASS; receipt ')]
        require(len(paths) == 1, f'{kind}: missing unique receipt')
        receipts[kind] = paths[0]
        files[paths[0]] = (ROOT/paths[0]).read_bytes()
        record = json.loads(files[paths[0]])
        for p, value in record['artifacts_sha256'].items():
            data = (ROOT/p).read_bytes()
            require(sha(data) == value, f'{kind}: changed artifact {p}')
            files[p] = data
        if kind == 'portability':
            for n in range(7):
                p = str(Path(paths[0]).parent/f'case_{n}/obj_dir/portability_check')
                files[p] = (ROOT/p).read_bytes()
        print(f'Formal readiness: {kind} PASS ({steps[-1]["duration_seconds"]:.1f}s)', flush=True)
    require(source_files() == sorted(inputs) and all(sha((ROOT/p).read_bytes()) == h for p, h in inputs.items()), 'sources changed during gate')
    manifest = {k: contract[k] for k in ('schema', 'gate', 'profile', 'claim', 'limitations')}
    manifest.update(status='pass', source_revision=revision, dirty_tree=dirty, patch_sha256=sha(files['source.patch']),
                    platform_sha256=inputs['config/platform.yaml'], command=['make', 'formal-readiness-check'],
                    started_utc=started, finished_utc=stamp(), execution_root=str(ROOT), suite=str(suite),
                    environment={k: env[k] for k in ('CCACHE_DISABLE', 'MAKEFLAGS', 'MFLAGS', 'OSS_CAD_SUITE')},
                    steps=steps, receipts=receipts, inputs_sha256=inputs,
                    artifacts_sha256={p: sha(b) for p, b in files.items()})
    metrics = evaluate(manifest, files.__getitem__)
    files['manifest.json'] = (json.dumps(manifest, indent=2, sort_keys=True)+'\n').encode()
    archive = ROOT/f'evidence/formal_readiness/{run_id}.tar.gz'
    archive.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive, 'w:gz') as tar:
        for name, data in sorted(files.items()):
            info = tarfile.TarInfo(name)
            info.size, info.mode = len(data), 0o644
            tar.addfile(info, io.BytesIO(data))
    receipt = {k: manifest[k] for k in ('schema', 'gate', 'status', 'profile', 'claim', 'limitations', 'source_revision', 'dirty_tree',
                                       'patch_sha256', 'platform_sha256', 'command', 'started_utc', 'finished_utc')}
    receipt.update(archive=str(archive.relative_to(ROOT)), archive_sha256=sha(archive.read_bytes()),
                   manifest_sha256=sha(files['manifest.json']), metrics=metrics)
    candidate = work/'acceptance.json'
    candidate.write_text(json.dumps(receipt, indent=2)+'\n')
    verify(candidate)
    destination.write_bytes(candidate.read_bytes())
    print(f'Formal readiness: PASS; receipt {destination.relative_to(ROOT)}; archive {archive.stat().st_size} bytes', flush=True)


if __name__ == '__main__':
    main()
