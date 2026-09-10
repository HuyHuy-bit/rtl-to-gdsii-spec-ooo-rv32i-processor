#!/usr/bin/env python3
"""Audit retained assertion portability and serialized rename proof evidence."""
from datetime import datetime
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
CONTRACT = 'config/formal_readiness.json'
RECEIPT = 'evidence/receipts/formal_readiness.json'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def safe_path(name):
    return bool(name) and not Path(name).is_absolute() and '..' not in Path(name).parts


def source_files(root=ROOT):
    names = subprocess.check_output(['git', 'ls-files', '--cached', '--others', '--exclude-standard', '-z'], cwd=root).decode().split('\0')
    return sorted({p for p in names if p and not p.startswith('evidence/') and (root/p).is_file()})


def provenance(record):
    require(re.fullmatch('[0-9a-f]{40}', record['source_revision']), 'missing source revision')
    require(type(record['dirty_tree']) is bool, 'missing dirty marker')
    start, finish = (datetime.fromisoformat(record[k]) for k in ('started_utc', 'finished_utc'))
    require(start.tzinfo is not None and finish.tzinfo is not None and start <= finish, 'invalid run timestamps')


def evaluate(manifest, read):
    contract = json.loads(read(CONTRACT))
    for key in ('schema', 'gate', 'profile', 'claim', 'limitations'):
        require(manifest[key] == contract[key], f'gate {key} differs')
    require(manifest['status'] == 'pass' and manifest['command'] == ['make', 'formal-readiness-check'], 'gate incomplete')
    provenance(manifest)
    require(sha(read('source.patch')) == manifest['patch_sha256'], 'patch differs')
    require(sha(read('config/platform.yaml')) == manifest['platform_sha256'], 'platform differs')
    require([s['command'] for s in manifest['steps']] == contract['commands'], 'missing gate command')
    for step in manifest['steps']:
        require(step['returncode'] == 0 and math.isfinite(step['duration_seconds'])
                and 0 < step['duration_seconds'] <= contract['command_timeout_seconds'], 'gate command failed or timed out')
        require(read(step['log']), 'missing gate log')
    origin = Path(manifest['execution_root'])
    suite = Path(manifest['suite'])
    require(origin.is_absolute() and suite.is_absolute(), 'missing execution paths')
    lock = json.loads(read('config/formal_tools.lock'))
    synthesis = json.loads(read(lock['synthesis_lock']))
    versions = dict(lock['versions'], yosys=synthesis['yosys_version'])
    measured = []

    def child(kind, config_path, runner, extra):
        config = json.loads(read(config_path))
        receipt_path = manifest['receipts'][kind]
        record = json.loads(read(receipt_path))
        provenance(record)
        require(record['schema'] == 1 and record['status'] == 'pass'
                and record['formal_readiness_accepted'] is False, f'{kind}: invalid child status')
        for key in ('profile', 'claim'):
            require(record[key] == config[key], f'{kind}: {key} differs')
        paths = [*config['sources'], config_path, 'config/formal_tools.lock', lock['synthesis_lock'], runner, *extra]
        require(record['inputs_sha256'] == {p: sha(read(p)) for p in paths}, f'{kind}: stale or incomplete inputs')
        expected_hash = sha(json.dumps(record['inputs_sha256'], sort_keys=True).encode())[:16]
        base = str(Path(receipt_path).parent)
        folder = 'assert_portability' if kind == 'portability' else 'rename_ownership'
        require(base == f'out/{folder}/{expected_hash}', f'{kind}: wrong receipt location')
        artifacts = record['artifacts_sha256']
        require(artifacts and all(safe_path(p) and p.startswith(base+'/') and sha(read(p)) == h
                                 for p, h in artifacts.items()), f'{kind}: artifact differs')
        def raw(path):
            require(path in artifacts, f'{kind}: unbound artifact {path}')
            return read(path).decode()
        expected_versions = dict(versions)
        if kind == 'portability':
            toolchain = json.loads(read('config/toolchain.lock'))
            expected_versions['verilator'] = next(t['expected_first_line'] for t in toolchain['tools'] if t['name'] == 'verilator')
        require(record['versions'] == expected_versions, f'{kind}: tool versions differ')
        marker = f'Receipt: {receipt_path}' if kind == 'portability' else f'receipt {receipt_path}'
        require(marker in read(manifest['steps'][0 if kind == 'portability' else 1]['log']).decode(), 'command receipt differs')
        return config, record, base, raw

    def command(row, expected, rss, raw, timeout, failure=False):
        require(row['command'] == ['/usr/bin/time', '-f', '%M', '-o', str(origin/rss), *expected], 'command differs')
        require(math.isfinite(row['duration_seconds']) and 0 < row['duration_seconds'] <= timeout, 'invalid job duration')
        require(type(row['max_rss_kib']) is int and row['max_rss_kib'] > 0
                and int(raw(rss).splitlines()[-1]) == row['max_rss_kib'], 'missing or inconsistent peak memory')
        if 'returncode' in row:
            require(type(row['returncode']) is int and ((row['returncode'] != 0) if failure else (row['returncode'] == 0)), 'wrong command exit status')
        measured.append(row)
        return raw(row['log'])

    def proof(raw, directory, task, sources, top, mode, engine, depth, output, assertions=(), covers=(), define=''):
        script = (f'[options]\nmode {mode}\ndepth {depth}\n\n[engines]\n{engine}\n\n'
                  '[script]\nplugin -i slang\n'
                  f'read_slang --no-synthesis-define --top {top} {define}'+ ' '.join(Path(p).name for p in sources)+'\n'
                  f'prep -top {top}\n\n[files]\n'+'\n'.join(str(origin/p) for p in sources)+'\n')
        require(raw(task) == script, 'formal script differs from qualified settings')
        for path, data in sources.items():
            require(raw(f'{directory}/src/{Path(path).name}') == data, 'formal source copy differs')
        status = 'FAIL' if assertions else 'PASS'
        require(raw(directory+'/status').split()[0] == status and f'DONE ({status}' in output, 'proof status differs')
        summary = raw(directory+'/'+status)
        require(f'engine_0 ({engine}) returned ' in summary, 'missing engine result')
        traces = re.findall(r'(?:cover|counterexample) trace: (engine_0/trace\w*\.vcd)', summary)
        if assertions or covers:
            require(traces, 'missing formal witness')
            for trace in traces:
                require('$enddefinitions' in raw(directory+'/'+trace), 'invalid formal witness')
        reached = re.findall(r'reached cover statement '+top+r'\.(\w+)', summary)
        require(set(reached) == set(covers), 'required cover not reached')
        labels = []
        for filename, line in re.findall(r'failed assertion \S+ at (\w+\.sv):(\d+)\.', summary):
            source = next(data for p, data in sources.items() if Path(p).name == filename)
            label = re.search(r'(\w+):\s*assert\b', source.splitlines()[int(line)-1])
            labels.append(label[1] if label else f'{filename}:{line}')
        require(bool(set(labels) & set(assertions)) if assertions else not labels, 'wrong assertion failed')
        if mode == 'prove':
            require('successful proof by k-induction' in output, 'induction did not complete')
        return labels, reached, sorted(directory+'/'+t for t in traces)

    pc, port, pb, praw = child('portability', 'config/assertion_portability.json', 'tools/run_assert_portability.py', ['config/toolchain.lock'])
    require(pc['formal_depth'] == port['depth'] == contract['minimum_depth'] == 12
            and pc['formal_engine'] == port['engine'] == 'smtbmc yices', 'portability bound/engine differs')
    require(port['assumptions'] == pc['formal_assumptions'] and port['covers'] == pc['required_covers'], 'portability assumptions/covers differ')
    require(pc['required_covers'] == contract['portability_required_covers'], 'portability cover contract differs')
    require(len(pc['support_matrix']) == contract['portability_mutations'] == 6
            and [r['mutation'] for r in pc['support_matrix']] == list(range(1, 7)), 'portability mutation matrix differs')
    require(len(port['commands']) == 22 and len(port['formal']) == len(port['simulation']) == 7, 'portability jobs missing')
    rows = [dict(mutation=0, marker=None, assertion=None), *pc['support_matrix']]
    for row in rows:
        n = row['mutation']
        base = f'{pb}/case_{n}'
        build, sim, formal = port['commands'][3*n:3*n+3]
        require([r['log'] for r in (build, sim, formal)] == [f'{base}/{name}.log' for name in ('build', 'simulation', 'formal')], 'portability logs differ')
        command(build, ['verilator', '--binary', '--timing', '--assert', '--Wall', '--build-jobs', '2', '--top-module', 'portability_tb',
                        f'-DMUTATION={n}', '--Mdir', str(origin/base/'obj_dir'), *pc['sources'][:2], '-o', 'portability_check'],
                base+'/build.rss', praw, pc['timeout_seconds'])
        output = command(sim, [str(origin/base/'obj_dir/portability_check')], base+'/simulation.rss', praw, pc['timeout_seconds'], bool(n))
        marker = row['marker'] if n else f'PORTABILITY PASS cycles={pc["simulation_cycles"]} reset_pulses=2'
        require(marker in output, 'simulation completion/fault missing')
        require(port['simulation'][n] == dict(mutation=n, result='detected' if n else 'pass', marker=row['marker']), 'simulation receipt differs')
        mode = 'bmc' if n else 'prove'
        require(port['formal'][n] == dict(mutation=n, mode=mode, status='FAIL' if n else 'PASS', assertion=row['assertion']), 'formal receipt differs')
        output = command(formal, [str(suite/'bin/sby'), '-f', '-d', str(origin/base/'proof'), str(origin/base/'proof.sby')],
                         base+'/formal.rss', praw, pc['timeout_seconds'], bool(n))
        sources = {p: read(p).decode() for p in (pc['sources'][0], pc['sources'][2])}
        proof(praw, base+'/proof', base+'/proof.sby', sources, 'portability_formal', mode, port['engine'], port['depth'], output,
              [row['assertion']] if n else (), define=f'-D MUTATION={n} ')
    cover = port['commands'][-1]
    require(cover['log'] == pb+'/cover.log', 'cover log differs')
    output = command(cover, [str(suite/'bin/sby'), '-f', '-d', str(origin/pb/'cover'), str(origin/pb/'cover.sby')],
                     pb+'/cover.rss', praw, pc['timeout_seconds'])
    proof(praw, pb+'/cover', pb+'/cover.sby', sources, 'portability_formal', 'cover', port['engine'], port['depth'], output,
          covers=pc['required_covers'], define='-D MUTATION=0 ')

    rc, rename, rb, rraw = child('rename', 'config/rename_ownership.json', 'tools/run_rename_ownership.py', ['tools/run_assert_portability.py'])
    require((rc['physical_registers'], rc['architectural_registers'], rc['maximum_inflight']) == (64, 32, 1), 'rename geometry differs')
    require(rc['depth'] == contract['minimum_depth'] and rc['mode'] == 'bmc' and rc['engine'] == 'abc bmc3'
            and rc['cover_engine'] == 'smtbmc yices', 'rename bound/engine differs')
    require(rename['assumptions'] == rc['assumptions'] and rename['environment'] == rc['environment'], 'rename assumptions differ')
    require(len(rc['mutations']) == contract['rename_mutations'] == 5, 'rename mutation matrix differs')
    require(rc['required_covers'] == contract['rename_required_covers']
            and [m['name'] for m in rc['mutations']] == contract['rename_required_mutations'], 'rename coverage contract differs')
    variants = [dict(name='covers'), *rc['mutations'], dict(name='baseline')]
    require([j['name'] for j in rename['jobs']] == [v['name'] for v in variants], 'rename jobs missing')
    for variant, job in zip(variants, rename['jobs']):
        base = rb+'/'+variant['name']
        mutant = 'old' in variant
        mode = 'cover' if variant['name'] == 'covers' else 'bmc'
        engine = rc['cover_engine'] if mode == 'cover' else rc['engine']
        require(job['mode'] == mode and job['engine'] == engine and job['depth'] == rc['depth']
                and job['status'] == ('FAIL' if mutant else 'PASS') and job['expected_failure'] is mutant
                and job['dut_assertions_enabled'] is (not mutant) and job['log'] == base+'/check.log', 'rename job settings differ')
        output = command(job, [str(suite/'bin/sby'), '-f', '-d', str(origin/base/'proof'), str(origin/base/'check.sby')],
                         base+'/memory.rss', rraw, rc['timeout_seconds'], mutant)
        sources = {}
        for p in rc['sources']:
            data = read(p).decode()
            if mutant and p == 'rtl/backend/rename_single.sv':
                require(data.count(variant['old']) == 1, 'mutation no longer matches')
                data = '`define SYNTHESIS\n'+data.replace(variant['old'], variant['new'])+'\n`undef SYNTHESIS\n'
                p = base+'/rename_single.sv'
                require(rraw(p) == data, 'mutant source differs')
            sources[p] = data
        labels, reached, traces = proof(rraw, base+'/proof', base+'/check.sby', sources, rc['top'], mode, engine, rc['depth'], output,
                                        variant.get('assertions', ()), rc['required_covers'] if mode == 'cover' else ())
        require(job['assertions_failed'] == labels and job['covers_reached'] == reached and job['traces'] == traces, 'rename reported properties/witnesses differ')
    return dict(portability_mutations=6, portability_covers=len(pc['required_covers']), portability_induction=True,
                rename_mutations=5, rename_covers=len(rc['required_covers']), rename_bmc_depth=rc['depth'],
                measured_commands=len(measured), max_rss_kib=max(r['max_rss_kib'] for r in measured),
                job_duration_seconds=round(sum(r['duration_seconds'] for r in measured), 6))


def verify(receipt_path=ROOT/RECEIPT, root=ROOT):
    receipt = json.loads(Path(receipt_path).read_text())
    require(safe_path(receipt['archive']), 'unsafe archive path')
    archive = root/receipt['archive']
    require(sha(archive.read_bytes()) == receipt['archive_sha256'], 'archive hash differs')
    with tarfile.open(archive, 'r:gz') as tar:
        members = tar.getmembers()
        names = [m.name for m in members]
        require(len(names) == len(set(names)) and all(m.isfile() and safe_path(m.name) for m in members), 'unsafe archive members')
        files = {m.name: tar.extractfile(m).read() for m in members}
    require(sha(files['manifest.json']) == receipt['manifest_sha256'], 'manifest hash differs')
    manifest = json.loads(files.pop('manifest.json'))
    require(set(files) == set(manifest['artifacts_sha256']) and all(sha(b) == manifest['artifacts_sha256'][p] for p, b in files.items()), 'archive inventory differs')
    require(source_files(root) == sorted(manifest['inputs_sha256']), 'source inventory differs')
    for path, value in manifest['inputs_sha256'].items():
        require(sha((root/path).read_bytes()) == value == sha(files[path]), f'stale source: {path}')
    for key in ('schema', 'gate', 'status', 'profile', 'claim', 'limitations', 'source_revision', 'dirty_tree',
                'patch_sha256', 'platform_sha256', 'command', 'started_utc', 'finished_utc'):
        require(receipt[key] == manifest[key], f'receipt {key} differs')
    metrics = evaluate(manifest, files.__getitem__)
    require(receipt['metrics'] == metrics, 'receipt metrics differ')
    return metrics


if __name__ == '__main__':
    print('Formal readiness evidence: PASS', json.dumps(verify(), sort_keys=True))
