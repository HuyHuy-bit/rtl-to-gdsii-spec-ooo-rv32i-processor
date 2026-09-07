#!/usr/bin/env python3
"""Build the core and check architectural execution under memory/retirement stalls."""
import argparse
import json
import resource
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from verif.core.programs import program, random_alu, i, branch
from verif.core.reference import Reference, unpack_event
from verif.lockstep.comparator import compare_traces
from tools.run_a1_probe import digest, verify_synthesis_suite
from tools.run_lockstep_smoke import verify_spike
from model.iss.spike_log import parse_spike_log
from verif.protocol.generated.memory_protocol import request_from_dict, response_from_dict, validate_request, validate_response

OUT = ROOT / 'out/single_lane'


def run(command, name):
    result = subprocess.run([str(c) for c in command], cwd=ROOT, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=300)
    (OUT / f'{name}.log').write_text(result.stdout)
    if result.returncode: raise RuntimeError(f'{name} failed:\n{result.stdout[-6000:]}')
    return result.stdout


def make_header():
    mem = json.loads((ROOT/'config/memory_protocol.yaml').read_text())
    event = json.loads((ROOT/'config/commit_event.yaml').read_text())
    lines = ['#pragma once']
    for prefix, fields in [('REQ', mem['request_fields']), ('RSP', mem['response_fields'])]:
        offset = 0
        for f in reversed(fields):
            lines.append(f'constexpr unsigned {prefix}_{f["name"].upper()} = {offset};')
            offset += f['width']
        lines.append(f'constexpr unsigned {"REQUEST" if prefix == "REQ" else "RESPONSE"}_WORDS = {(offset+31)//32};')
    offset = 4*sum(f['width'] for f in event['csr_fields'])
    for f in reversed(event['slot_fields']):
        lines.append(f'constexpr unsigned EV_{f["name"].upper()} = {offset};')
        offset += f['width']
    lines += [f'constexpr unsigned EVENT_BITS = {offset};',f'constexpr unsigned COMMIT_WORDS = {(offset*2+31)//32};']
    path = OUT/'fields.hpp'
    content = '\n'.join(lines)+'\n'
    if not path.exists() or path.read_text() != content: path.write_text(content)


def build_core(config):
    OUT.mkdir(parents=True, exist_ok=True)
    make_header()
    flags = ['--Wall','-Wno-UNUSEDPARAM','-Wno-UNUSEDSIGNAL','--assert','--top-module',config['top']]
    run(['verilator','--lint-only',*flags,*config['sources']], 'lint')
    run(['verilator','--cc','--exe','--build','--build-jobs','2',*flags,'--x-initial','unique',
         '--Mdir',OUT/'obj_dir','-CFLAGS',f'-std=c++17 -Wall -Wextra -Werror -I{OUT}',
         *config['sources'],ROOT/'verif/core/single_lane_tb.cpp','-o','core_check'], 'build')
    return OUT/'obj_dir/core_check'


def check_trace(output, image, mode):
    actual = []
    expected = []
    total = 0
    transactions = {}
    memory_schema = json.loads((ROOT/'config/memory_protocol.yaml').read_text())
    for line in output.splitlines():
        if line == 'RESET':
            if actual: compare_traces(expected, actual)
            ref = Reference(image, mode)
            actual = []; expected = []
            transactions = {}
        elif line.startswith(('IREQ ', 'DREQ ', 'IRSP ', 'DRSP ')):
            tag, *words = line.split()
            raw = sum(int(w,16) << (32*n) for n,w in enumerate(words))
            fields = memory_schema['request_fields' if tag.endswith('REQ') else 'response_fields']
            values = {}
            for field in reversed(fields):
                values[field['name']] = raw & ((1 << field['width'])-1)
                raw >>= field['width']
            port = 'instruction' if tag[0] == 'I' else 'data'
            if tag.endswith('REQ'):
                request = request_from_dict(values)
                validate_request(port,request)
                assert port not in transactions, 'occupied transaction reused'
                transactions[port] = request
            else:
                request = transactions.pop(port)
                injected = ((port == 'data' and mode in ('wrong_id','protocol_error','reserved_status','payload_error'))
                            or (port == 'instruction' and mode in ('fetch_wrong_id','fetch_payload')))
                if not injected: validate_response(request,response_from_dict(values))
        elif line.startswith('EVENT '):
            event = unpack_event(line.split()[3:])
            want = ref.step()
            compare_traces([{'slots':[want,{}]}], [{'slots':[event,{}]}]) if event['order'] == 0 else None
            actual.append({'slots':[event,{}]}); expected.append({'slots':[want,{}]})
            total += 1
    compare_traces(expected, actual)
    return total, actual


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--synth', action='store_true')
    parser.add_argument('--mutations', action='store_true')
    parser.add_argument('--suite', type=Path, default=Path.home()/'tools/oss-cad-suite-20260905/oss-cad-suite')
    args = parser.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    OUT.mkdir(parents=True, exist_ok=True)
    receipt = OUT/('synth_receipt.json' if args.synth else 'mutation_receipt.json' if args.mutations else 'receipt.json')
    receipt.unlink(missing_ok=True)
    config = json.loads((ROOT/'config/single_lane.json').read_text())
    lock = json.loads((ROOT/'config/toolchain.lock').read_text())
    for name, command in [('verilator','verilator'),('riscv-gcc','riscv64-unknown-elf-gcc'),('riscv-objcopy','riscv64-unknown-elf-objcopy')]:
        expected = next(t['expected_first_line'] for t in lock['tools'] if t['name']==name)
        if run([command,'--version'], name).splitlines()[0] != expected: raise RuntimeError(f'{name} version mismatch')
    make_header()
    for bench, extra in [('backend_single_tb', ['rtl/backend/rename_single.sv','rtl/backend/rob_single.sv']),
                         ('csr_single_tb',['rtl/core/csr_single.sv'])]:
        run(['verilator','--binary','--timing','--build-jobs','2','--assert','--Wall','-Wno-UNUSEDPARAM','-Wno-UNUSEDSIGNAL',
             '--top-module',bench,'--Mdir',OUT/bench,*config['sources'][:4],*extra,f'verif/core/{bench}.sv','-o','unit_check'],bench+'_build')
        print(run([OUT/bench/'unit_check'],bench).splitlines()[0],flush=True)
    build_core(config)
    flags = ['--Wall','-Wno-UNUSEDPARAM','-Wno-UNUSEDSIGNAL','--assert','--top-module',config['top']]
    results = []
    cases = [(seed,'normal',1600) for seed in config['seeds']]
    cases += [(42,'stall_store',300)]
    cases += [(42,mode,80) for mode in ('reset_fetch','reset_data','reset_commit','load_fault','fetch_fault')]
    cases += [(42,mode,80) for mode in ('wrong_id','protocol_error','reserved_status','unsolicited','store_fault','payload_error','fetch_wrong_id','fetch_payload')]
    for seed, mode, count in cases:
        image = program(seed,mode)
        path = OUT/f'{mode}_{seed}.bin'; path.write_bytes(image)
        output = run([OUT/'obj_dir/core_check',path,seed,count,mode,'+verilator+rand+reset+2',f'+verilator+seed+{seed}'], f'{mode}_{seed}')
        fatal = mode in ('wrong_id','protocol_error','reserved_status','unsolicited','store_fault','payload_error','fetch_wrong_id','fetch_payload')
        if ('FATAL ' not in output if fatal else 'CORE PASS' not in output): raise RuntimeError(f'{mode}: wrong termination')
        checked, packets = check_trace(output,image,mode)
        if not fatal and len(packets) != count: raise RuntimeError('incomplete retirement trace')
        results.append({'seed':seed,'mode':mode,'checked_events':checked,'expected_fatal':fatal})
        print(f'Core {mode} seed={seed}: PASS ({checked} events)',flush=True)
    spike = (ROOT/'../riscv-isa-sim/build/spike').resolve()
    revision = verify_spike(spike)
    words = [i(n,0,0) for n in range(1,32)] + random_alu(20260906,1024) + [branch(0,0,0)]
    assembly = OUT/'spike_alu.S'
    assembly.write_text('.section .text.init\n.globl _start\n_start:\n'+''.join(f'.word 0x{w:08x}\n' for w in words))
    elf = OUT/'spike_alu.elf'; relocated = OUT/'spike_alu_relocated.elf'; binary = OUT/'spike_alu.bin'
    run(['riscv64-unknown-elf-gcc','-march=rv32i','-mabi=ilp32','-nostdlib','-nostartfiles','-Wl,--no-relax,--build-id=none','-T','sw/link/platform.ld',assembly,'-o',elf], 'assemble')
    run(['riscv64-unknown-elf-objcopy','-O','binary',elf,binary],'binary')
    run(['riscv64-unknown-elf-objcopy','--change-addresses=0x80000000',elf,relocated], 'relocate')
    count = len(words)
    spike_output = run([spike,'--isa=rv32i_zicsr_zifencei','--priv=m','--pmpregions=0',f'--instructions={count+5}','-l','--log-commits','-m0x80000000:0x10000',relocated], 'spike')
    expected = parse_spike_log(spike_output,0x80000000,65536)
    output = run([OUT/'obj_dir/core_check',binary,42,count], 'spike_core')
    checked, actual = check_trace(output,binary.read_bytes(),'normal')
    compare_traces(expected,actual)
    print(f'Core Spike differential: PASS ({checked} events)',flush=True)
    if args.mutations:
        mutation_results = []
        for name, file, old, new, mode in [
            ('wrong_alu', 'rtl/execute/execute_single.sv', 'source1_i + operand2;', 'source1_i - operand2;', 'normal'),
            ('early_store', 'rtl/core/single_lane_core.sv',
             'state_q == LOAD_REQUEST || state_q == STORE_REQUEST',
             'state_q == LOAD_REQUEST || state_q == STORE_REQUEST || (state_q == RETIRE && pending_q.mem_write_mask != 0)', 'stall_store'),
        ]:
            source = (ROOT/file).read_text()
            assert source.count(old) == 1, f'{name} mutation no longer matches'
            directory = OUT/'mutations'/name
            directory.mkdir(parents=True,exist_ok=True)
            changed = directory/Path(file).name
            changed.write_text(source.replace(old,new))
            sources = [str(changed) if s == file else s for s in config['sources']]
            run(['verilator','--cc','--exe','--build','--build-jobs','2',*flags,'--Mdir',directory/'obj_dir',
                 '-CFLAGS',f'-std=c++17 -Wall -Wextra -Werror -I{OUT}',*sources,
                 ROOT/'verif/core/single_lane_tb.cpp','-o','core_check'],name+'_build')
            path = directory/'program.bin'; image=program(42,mode); path.write_bytes(image)
            result = subprocess.run([str(directory/'obj_dir/core_check'),str(path),'42','300',mode],cwd=ROOT,text=True,
                                    stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=60)
            (directory/'check.log').write_text(result.stdout)
            if name == 'early_store':
                if result.returncode == 0 or 'store escaped before retirement' not in result.stdout:
                    raise RuntimeError('early-store fault was not detected')
            else:
                if result.returncode != 0: raise RuntimeError('ALU mutation failed outside the scoreboard')
                try: check_trace(result.stdout,image,mode)
                except AssertionError: pass
                else: raise RuntimeError('ALU fault was not detected')
            mutation_results.append(name)
            print(f'Core mutation {name}: PASS (detected)',flush=True)
    else: mutation_results = []
    synthesis = None
    if args.synth:
        yosys = verify_synthesis_suite(args.suite.resolve())
        script = f'read_slang --top single_lane_core -D SYNTHESIS {" ".join(config["sources"])}; synth -top single_lane_core -flatten -noabc; check -assert; write_json out/single_lane/synth.json'
        run([yosys,'-Q','-T','-m','slang','-p',script],'synth')
        net = json.loads((OUT/'synth.json').read_text())['modules']['single_lane_core']
        from collections import Counter
        cells = Counter(c['type'] for c in net['cells'].values())
        if any('LATCH' in c or not c.startswith('$_') for c in cells): raise RuntimeError('latch or unmapped hierarchy in core')
        synthesis = dict(cells)
        print(f'Core synthesis smoke: PASS ({sum(cells.values())} generic cells; no latches)',flush=True)
    paths = config['sources'] + ['config/single_lane.json','config/platform.yaml','config/commit_event.yaml','config/memory_protocol.yaml',
                                'config/toolchain.lock','config/references.lock','verif/core/programs.py','verif/core/reference.py',
                                'verif/core/single_lane_tb.cpp','verif/core/backend_single_tb.sv','verif/core/csr_single_tb.sv','tools/run_single_lane.py',
                                'config/synthesis.lock','config/prf_contract.json','sw/link/platform.ld',
                                'tools/run_a1_probe.py','tools/run_lockstep_smoke.py','model/iss/spike_log.py',
                                'verif/lockstep/comparator.py','verif/lockstep/generated/commit_event.py','verif/protocol/generated/memory_protocol.py']
    receipt.write_text(json.dumps({'schema':1,'profile':config['profile'],'inputs_sha256':{p:digest(ROOT/p) for p in paths},
                                  'cases':results,'spike_revision':revision,'spike_events':checked,
                                  'mutations_detected':mutation_results,'synthesis':synthesis,'architectural_slice_accepted':False},indent=2)+'\n')


if __name__ == '__main__':
    main()
