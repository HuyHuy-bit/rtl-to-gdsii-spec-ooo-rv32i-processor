#!/usr/bin/env python3
"""Run shared RTL unit verification with profile-specific acceptance requirements."""
import argparse
from datetime import datetime, timezone
import hashlib
import io
import json
import os
from pathlib import Path
import re
import resource
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.unit_profiles import PROFILES


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def local_headers(root, path, seen=None):
    seen = set() if seen is None else seen
    for name in re.findall(r'^\s*#include\s+"([^"]+)"', path.read_text(), re.MULTILINE):
        header = (path.parent/name).resolve()
        if header.is_file() and header.is_relative_to(root) and header not in seen:
            seen.add(header)
            local_headers(root, header, seen)
    return seen


def simulation_result(output, profile, config, seed):
    if not output.startswith(profile['prefix']) or len(output.splitlines()) != 1:
        raise RuntimeError('missing simulation completion')
    tokens = output[len(profile['prefix']):].split()
    if any(not re.fullmatch(r'\w+=\d+', token) for token in tokens):
        raise RuntimeError('malformed simulation counters')
    fields = {key: int(value) for key, value in (token.split('=') for token in tokens)}
    if (len(fields) != len(tokens) or not profile['validate'](config, fields, seed)
            or not all(fields.get(k, 0) > 0 for k in profile['required'])):
        raise RuntimeError('missing completion or coverage: '+output)
    return fields


def mutate(original, old, new, expected):
    if original.count(old) != expected:
        raise RuntimeError('mutation no longer matches')
    return original.replace(old, new)


class UnitRun:
    def __init__(self, name, root=ROOT):
        self.name, self.root = name, root
        self.profile = PROFILES[name]
        self.config = json.loads((root/f'config/{name}.json').read_text())
        if not self.profile['geometry'](self.config):
            raise RuntimeError('unsupported profile configuration')
        self.out = root/'out'/name
        self.out.mkdir(parents=True, exist_ok=True)
        self.retained = root/f'evidence/receipts/{name}.json'
        self.retained.parent.mkdir(parents=True, exist_ok=True)
        self.retained.unlink(missing_ok=True)
        (self.out/'receipt.json').unlink(missing_ok=True)
        self.sources = [root/p for p in self.config.get('sources', self.profile['sources'])]
        self.bench = root/f'verif/unit/{name}_tb.cpp'
        paths = [*self.sources, self.bench, *local_headers(root, self.bench),
                 *(root/p for p in self.profile['inputs']), root/f'config/{name}.json',
                 root/'tools/run_unit.py', root/'tools/unit_profiles.py', root/'Makefile',
                 root/'config/toolchain.lock', root/'config/synthesis.lock']
        if 'lint_config' in self.config:
            paths.append(root/self.config['lint_config'])
        self.inputs = {str(p.relative_to(root)): digest(p) for p in sorted(set(paths))}
        self.commands, self.artifacts = [], set()

    def run(self, command, name, failure=None):
        result = subprocess.run([str(p) for p in command], cwd=self.root, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=self.profile['timeout'], env=dict(os.environ, CCACHE_DISABLE='1'))
        path = self.out/name
        path.write_text(result.stdout)
        self.artifacts.add(path)
        self.commands.append(dict(command=[str(p) for p in command], returncode=result.returncode, log=name))
        if ((failure and (result.returncode == 0 or failure not in result.stdout))
                or (not failure and result.returncode != 0)):
            raise RuntimeError(f'check failed; see {path}\n{result.stdout[-2500:]}')
        return result.stdout

    def headers(self):
        def write(name, lines):
            path = self.out/name
            path.write_text('\n'.join(lines)+'\n')
            self.artifacts.add(path)
        if self.profile['schema_checks']:
            self.run(['make', *self.profile['schema_checks']], 'schema.log')
        if 'event' in self.profile['headers']:
            schema = json.loads((self.root/'config/commit_event.yaml').read_text())
            offset, fields = 0, []
            for f in reversed(schema['csr_fields']):
                fields.append(f'constexpr unsigned CSR_{f["name"].upper()}_OFFSET = {offset};')
                offset += f['width']
            fields.append(f'constexpr unsigned CSR_EFFECT_BITS = {offset};')
            offset *= schema['max_csr_effects']
            for f in reversed(schema['slot_fields']):
                fields.append(f'constexpr unsigned {f["name"].upper()}_OFFSET = {offset};')
                offset += f['width']
            write('backend_event_layout.hpp', [*fields, f'constexpr unsigned EVENT_BITS = {offset};'])
        if 'memory' in self.profile['headers']:
            schema = json.loads((self.root/'config/memory_protocol.yaml').read_text())
            fields = []
            for kind in ('request', 'response'):
                offset = 0
                for f in reversed(schema[kind+'_fields']):
                    fields.append(f'constexpr unsigned {kind.upper()}_{f["name"].upper()}_OFFSET = {offset};')
                    offset += f['width']
                fields.append(f'constexpr unsigned {kind.upper()}_BITS = {offset};')
            write('fetch_memory_layout.hpp', fields)
        if 'pma' in self.profile['headers']:
            regions = json.loads((self.root/'config/platform.yaml').read_text())['memory']['regions']
            rows = ['{'+', '.join([r['base']+'u', r['size']+'u',
                    *(str(r[k]).lower() for k in ('read', 'write', 'cacheable', 'idempotent'))])+'}' for r in regions]
            write('platform_memory_layout.hpp', ['#include <array>', '#include <cstdint>',
                  'struct Region { uint32_t base, size; bool read, write, cacheable, idempotent; };',
                  f'constexpr std::array<Region, {len(rows)}> REGIONS = {{{{'+', '.join(rows)+'}};'])
        if 'csr' in self.profile['headers']:
            platform = json.loads((self.root/'config/platform.yaml').read_text())
            schema = json.loads((self.root/'config/commit_event.yaml').read_text())
            rows = []
            for c in platform['csrs']:
                values = [c[k]+'u' for k in ('address', 'reset', 'write_mask', 'fixed_mask', 'fixed_value')]
                rows.append('{'+', '.join([*values, 'true' if c['access']=='ro' else 'false'])+'}')
            offset, offsets = 0, {}
            for f in reversed(schema['csr_fields']):
                offsets[f['name']] = offset
                offset += f['width']
            write('csr_reference_layout.hpp', ['#include <array>', '#include <cstdint>',
                  'struct Spec { unsigned address; uint32_t reset, write_mask, fixed_mask, fixed_value; bool readonly; };',
                  f'constexpr std::array<Spec, {len(rows)}> CSR_SPEC = {{{{'+', '.join(rows)+'}};',
                  f'constexpr unsigned EFFECT_BITS = {offset};',
                  'constexpr unsigned FIELD_OFFSET[] = {'+', '.join(str(offsets[f['name']]) for f in schema['csr_fields'])+'};',
                  'constexpr unsigned FIELD_WIDTH[] = {'+', '.join(str(f['width']) for f in schema['csr_fields'])+'};'])

    def verilator_options(self):
        return ['--Wall', '--assert', '--top-module', self.profile['top'],
                *(f'-G{k}={v}' for k, v in self.profile['parameters'].items()),
                *([self.root/self.config['lint_config']] if 'lint_config' in self.config else [])]

    def build(self, sources, directory, log, mutated=False):
        self.run(['verilator', '--cc', '--exe', '--build', '--build-jobs', '2',
                  *self.verilator_options(), *(self.profile['mutation_flags'] if mutated else []),
                  '--Mdir', directory, '-CFLAGS', f'-std=c++17 -Wall -Wextra -Werror -I{self.out}',
                  *sources, self.bench, '-o', self.name+'_check'], log)
        return directory/(self.name+'_check')

    def publish(self, result):
        if any(digest(self.root/p) != value for p, value in self.inputs.items()):
            raise RuntimeError('input changed during check')
        files = {p: (self.root/p).read_bytes() for p in self.inputs}
        files.update({str(p.relative_to(self.root)): p.read_bytes() for p in self.artifacts})
        result.update(inputs_sha256=self.inputs, commands=self.commands,
                      artifacts_sha256={p: hashlib.sha256(b).hexdigest() for p, b in files.items()})
        files['manifest.json'] = (json.dumps(result, indent=2)+'\n').encode()
        run_id = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
        archive = self.root/f'evidence/{self.name}/{run_id}.tar.gz'
        archive.parent.mkdir(parents=True, exist_ok=True)
        with tarfile.open(archive, 'w:gz') as tar:
            for name, data in sorted(files.items()):
                info = tarfile.TarInfo(name)
                info.size, info.mode = len(data), 0o644
                tar.addfile(info, io.BytesIO(data))
        result.update(archive=str(archive.relative_to(self.root)), archive_sha256=digest(archive))
        text = json.dumps(result, indent=2)+'\n'
        (self.out/'receipt.json').write_text(text)
        self.retained.write_text(text)

    def execute(self, suite):
        profile, config = self.profile, self.config
        version = self.run(['verilator', '--version'], 'version.log').strip()
        tools = json.loads((self.root/'config/toolchain.lock').read_text())['tools']
        if version != next(t['expected_first_line'] for t in tools if t['name']=='verilator'):
            raise RuntimeError('Verilator version differs')
        for path, value in json.loads((self.root/'config/synthesis.lock').read_text())['sha256'].items():
            if digest(suite/path) != value:
                raise RuntimeError(f'synthesis tool pin differs: {path}')
        self.headers()
        self.run(['verilator', '--lint-only', *self.verilator_options(), *self.sources], 'lint.log')
        binary = self.build(self.sources, self.out/'obj_dir', 'build.log')
        results = []
        for seed in config['seeds']:
            args = [str(config[profile['random_key']])] if profile['random_key'] else []
            output = self.run([binary, str(seed), *args], f'seed_{seed}.log')
            results.append(simulation_result(output, profile, config, seed))
            print(output.strip(), flush=True)
        for args, log in profile['extra_runs']:
            self.run([binary, *args], log)
        for category in ('negatives', 'guards'):
            for name, (args, marker) in profile[category].items():
                self.run([binary, *args], category+'_'+name+'.log', failure=marker)
                print(f'{self.name} {category} {name}: PASS (rejected)', flush=True)
        for name, (path, old, new, expected) in profile['mutations'].items():
            rtl = self.root/path
            if rtl not in self.sources:
                raise RuntimeError(f'mutation target is not in source list: {path}')
            directory = self.out/'mutations'/name
            directory.mkdir(parents=True, exist_ok=True)
            changed = directory/rtl.name
            changed.write_text(mutate(rtl.read_text(), old, new, expected))
            self.artifacts.add(changed)
            executable = self.build([changed if p==rtl else p for p in self.sources], directory/'obj_dir', name+'_build.log', True)
            self.run([executable, *profile['mutation_args']], name+'.log', failure=profile['mismatch'])
            print(f'{self.name} mutation {name}: PASS (detected)', flush=True)
        script = self.out/'synth.ys'
        top = profile['top']
        parameters = ''.join(f' -G {k}={v}' for k, v in profile['parameters'].items())
        flatten = '' if self.name=='rename_bundle' else ' -flatten'
        script.write_text(f'plugin -i slang\nread_slang --top {top}{parameters} '+' '.join(str(p) for p in self.sources)+'\n'
                          f'synth -top {top}{flatten}\ncheck -assert\nwrite_json {self.out/"synth.json"}\n')
        self.artifacts.update([script, self.out/'synth.json'])
        self.run([suite/'bin/yosys', '-s', script], 'synth.log')
        modules = json.loads((self.out/'synth.json').read_text())['modules']
        counts = {}
        for data in modules.values():
            for cell in data['cells'].values():
                kind = cell['type']
                counts[kind] = counts.get(kind, 0)+1
        state = any('DFF' in k.upper() for k in counts)
        if (not counts or state != (self.name!='rename_bundle')
                or any('LATCH' in k.upper() or (not k.startswith('$_') and k not in modules) for k in counts)):
            raise RuntimeError('unexpected state, latch or unmapped synthesis cell')
        self.publish(dict(schema=1, profile=config['profile'], status='pass', scope=config['scope'],
                          verilator=version, simulations=results, required_counters=profile['required'],
                          mutations_detected=list(profile['mutations']), caller_assertions_checked=list(profile['negatives']),
                          stimulus_guards_checked=list(profile['guards']), synthesis_cells=counts, two_wide_core_accepted=False))
        print(f'{self.name}: PASS; {sum(counts.values())} generic cells; receipt {self.retained.relative_to(self.root)}', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('profile', choices=PROFILES)
    parser.add_argument('--suite', type=Path, default=Path.home()/'tools/oss-cad-suite-20260905/oss-cad-suite')
    args = parser.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    UnitRun(args.profile).execute(args.suite.resolve())


if __name__ == '__main__':
    main()
