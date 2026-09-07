#!/usr/bin/env python3
"""Run one serialized Architectural Slice promotion and retain its complete evidence bundle."""
from datetime import datetime, timezone
import io
import json
import os
from pathlib import Path
import subprocess
import tarfile
import time

from check_architectural_slice import CONTRACT, RECEIPT, ROOT, evaluate, require, sha, source_files, verify


def stamp():
    return datetime.now(timezone.utc).isoformat()


def main():
    contract = json.loads((ROOT/CONTRACT).read_text())
    destination = ROOT/RECEIPT
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.unlink(missing_ok=True)
    started = stamp()
    run_id = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    work = ROOT/'out/architectural_slice'/run_id
    work.mkdir(parents=True)
    files = {name: (ROOT/name).read_bytes() for name in source_files()}
    inputs = {name: sha(data) for name, data in files.items()}
    revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    dirty = bool(subprocess.check_output(['git', 'status', '--porcelain', '--untracked-files=all', '--', '.', ':!evidence'], cwd=ROOT))
    files['source.patch'] = subprocess.check_output(['git', 'diff', '--binary', 'HEAD', '--', '.', ':!evidence'], cwd=ROOT)
    env = dict(os.environ, CCACHE_DISABLE='1', MAKEFLAGS='', MFLAGS='')
    steps = []
    receipts = {'core': 'out/single_lane/synth_receipt.json', 'a1': 'out/a1/acceptance.json'}
    for index, command in enumerate(contract['commands']):
        log = work/f'{index}_{command[-1]}.log'
        before = time.monotonic()
        print(f'Architectural Slice [{index+1}/{len(contract["commands"])}]: {" ".join(command)}', flush=True)
        with log.open('w') as stream:
            result = subprocess.run(command, cwd=ROOT, env=env, stdout=stream, stderr=subprocess.STDOUT,
                                    timeout=contract['command_timeout_seconds'])
        name = f'commands/{log.name}'
        files[name] = log.read_bytes()
        steps.append(dict(command=command, returncode=result.returncode, log=name,
                          duration_seconds=round(time.monotonic()-before, 6)))
        require(result.returncode == 0, f'command failed; see {log}')
        if command[-1] in ('act4-core-check', 'sail-differential-check'):
            paths = [line.removeprefix('Receipt: ') for line in log.read_text().splitlines() if line.startswith('Receipt: ')]
            require(len(paths) == 1, 'missing unique child receipt')
            receipts['act4' if command[-1] == 'act4-core-check' else 'sail'] = paths[0]
        print(f'  PASS ({steps[-1]["duration_seconds"]:.1f}s)', flush=True)

    suffixes = {'.log', '.json', '.rpt', '.trace', '.bin', '.elf', '.v', '.sv', '.ys', '.tcl', '.S', '.hpp'}
    directories = ['out/prf', 'out/a1', 'out/a1_timing', 'out/lockstep', 'out/single_lane',
                   str(Path(receipts['act4']).parent), str(Path(receipts['sail']).parent)]
    for directory in directories:
        for path in sorted((ROOT/directory).rglob('*')):
            if path.is_file() and not path.is_symlink() and not any(p.startswith('obj_dir') for p in path.parts) and path.suffix in suffixes:
                files[str(path.relative_to(ROOT))] = path.read_bytes()
    binary = 'out/single_lane/obj_dir/core_check'
    files[binary] = (ROOT/binary).read_bytes()
    for path in receipts.values():
        files[path] = (ROOT/path).read_bytes()
    act = json.loads(files[receipts['act4']])
    checkout = ROOT/'out/deps/riscv-arch-test-4.0.0'
    for name in act['upstream']:
        files['external/act4/tests/'+name] = (checkout/'tests'/name).read_bytes()
    for path in checkout.glob('LICENSE*'):
        if path.is_file():
            files['external/act4/'+path.name] = path.read_bytes()
    reference = ROOT/'verif/reference/rv32i'
    reference_hashes = {}
    for folder in ('rtl', 'sim', 'tests', 'tools'):
        for path in sorted((reference/folder).rglob('*')):
            if path.is_file() and '__pycache__' not in path.parts and path.suffix not in {'.o', '.d'}:
                reference_hashes[str(path.relative_to(reference))] = sha(path.read_bytes())
    reference_hashes['Makefile'] = sha((reference/'Makefile').read_bytes())
    files['external/reference_inputs.json'] = json.dumps(reference_hashes, sort_keys=True).encode()
    require(source_files() == sorted(inputs), 'source inventory changed during run')
    require(all(sha((ROOT/p).read_bytes()) == value for p, value in inputs.items()), 'source changed during run')
    manifest = dict(schema=1, gate='Architectural Slice', status='pass', profile=contract['profile'],
                    claim_class=contract['claim_class'], limitations=contract['limitations'],
                    source_revision=revision, dirty_tree=dirty, patch_sha256=sha(files['source.patch']),
                    platform_sha256=inputs['config/platform.yaml'],
                    command=['make', 'architectural-slice-check'], environment={'CCACHE_DISABLE': '1', 'MAKEFLAGS': '', 'MFLAGS': ''},
                    started_utc=started, finished_utc=stamp(), steps=steps, receipts=receipts,
                    inputs_sha256=inputs, artifacts_sha256={p: sha(data) for p, data in files.items()})
    metrics = evaluate(manifest, files.__getitem__)
    files['manifest.json'] = (json.dumps(manifest, indent=2, sort_keys=True)+'\n').encode()
    archive = ROOT/f'evidence/architectural_slice/{run_id}.tar.gz'
    archive.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive, 'w:gz') as tar:
        for name, data in sorted(files.items()):
            info = tarfile.TarInfo(name)
            info.size = len(data)
            info.mode = 0o644
            tar.addfile(info, io.BytesIO(data))
    receipt = dict(schema=1, gate='Architectural Slice', status='pass', profile=contract['profile'],
                   claim_class=contract['claim_class'], source_revision=revision, dirty_tree=dirty,
                   patch_sha256=manifest['patch_sha256'], platform_sha256=manifest['platform_sha256'],
                   command=manifest['command'], started_utc=started, finished_utc=manifest['finished_utc'],
                   archive=str(archive.relative_to(ROOT)), archive_sha256=sha(archive.read_bytes()),
                   manifest_sha256=sha(files['manifest.json']), metrics=metrics, limitations=contract['limitations'])
    candidate = work/'acceptance.json'
    candidate.write_text(json.dumps(receipt, indent=2)+'\n')
    verify(candidate)
    destination.write_bytes(candidate.read_bytes())
    print(f'Architectural Slice: PASS; receipt {destination.relative_to(ROOT)}; archive {archive.stat().st_size} bytes', flush=True)


if __name__ == '__main__':
    main()
