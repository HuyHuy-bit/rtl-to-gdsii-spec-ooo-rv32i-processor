#!/usr/bin/env python3
"""Compare RTL architectural events with the pinned Sail model under stalls/reset."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import resource
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from model.iss.sail_log import parse_sail_log
from tools.act4_tools import SAIL
from tools.check_act4 import check_harness, check_sail
from tools.run_single_lane import OUT as CORE_OUT, build_core, check_trace
from verif.core.differential_programs import program
from verif.core.reference import unpack_event
from verif.lockstep.comparator import compare_traces, ComparisonError


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def packets(events):
    result = copy.deepcopy(events)
    for n,event in enumerate(result): event['order'] = n
    return [{'slots':[event,{}]} for event in result]


def compare_output(output, expected, boot):
    segments = []
    for line in output.splitlines():
        if line == 'RESET': segments.append([])
        elif line.startswith('EVENT '):
            if not segments: raise ValueError('event before reset')
            segments[-1].append(unpack_event(line.split()[3:]))
    if not segments or not segments[-1] or 'CORE PASS' not in output: raise ValueError('incomplete RTL run')
    total = 0
    for segment in segments:
        for n,event in enumerate(segment[:len(boot)]):
            if event['instruction'] != boot[n] or event['pc_before'] != 4*n:
                raise ValueError('RTL initialization sequence differs')
        if len(segment) > len(expected): raise ValueError('RTL exceeds reference trace')
        actual = segment[len(boot):]
        compare_traces(packets(expected[len(boot):len(segment)]),packets(actual))
        total += len(actual)
    if len(segments[-1]) != len(expected): raise ValueError('short RTL trace')
    return total


def coverage(events):
    traps = sorted({e['trap_cause'] for e in events if e.get('trap')})
    branches = sorted({(e['instruction'] >> 12) & 7 for e in events if e['instruction'] & 127 == 0x63})
    csr_forms = sorted({(e['instruction'] >> 12) & 7 for e in events if e['instruction'] & 127 == 0x73 and (e['instruction'] >> 12) & 7})
    loads = sorted({(e['instruction'] >> 12) & 7 for e in events if e.get('mem_read_mask')})
    stores = sorted({(e['instruction'] >> 12) & 7 for e in events if e.get('mem_write_mask')})
    if traps != [0,1,2,3,4,5,6,7,11] or branches != [0,1,4,5,6,7] or csr_forms != [1,2,3,5,6,7] or loads != [0,1,2,4,5] or stores != [0,1,2]:
        raise ValueError('required differential scenarios were not reached')
    return dict(trap_causes=traps,branch_forms=branches,csr_forms=csr_forms,load_forms=loads,store_forms=stores)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--sail', type=Path, default=SAIL)
    args = parser.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    sail = args.sail.resolve()
    lock,_ = check_harness(); check_sail(sail,lock)
    settings = json.loads((ROOT/'config/sail_differential.json').read_text())
    if digest(sail) != settings['sail_binary_sha256']: raise ValueError('Sail binary pin differs')
    if settings['sail_override']['platform']['instructions_per_tick'] <= settings['instruction_limit']+1:
        raise ValueError('Sail timer tick interval must exceed the run limit')
    config = json.loads((ROOT/'config/single_lane.json').read_text())
    versions = {}
    toolchain = json.loads((ROOT/'config/toolchain.lock').read_text())
    for name,command in [('verilator','verilator'),('riscv-gcc','riscv64-unknown-elf-gcc')]:
        version = subprocess.check_output([command,'--version'],text=True).splitlines()[0]
        if version != next(t['expected_first_line'] for t in toolchain['tools'] if t['name']==name):
            raise ValueError(f'{name} pin differs')
        versions[name] = version
    paths = config['sources'] + ['config/sail_differential.json','config/single_lane.json','config/platform.yaml',
        'config/commit_event.yaml','config/memory_protocol.yaml','config/toolchain.lock','config/act4.lock',
        'model/iss/sail_log.py','tools/run_sail_differential.py','tools/run_single_lane.py','tools/check_act4.py',
        'verif/core/differential_programs.py','verif/core/programs.py','verif/core/reference.py','verif/core/single_lane_tb.cpp',
        'verif/lockstep/comparator.py','verif/lockstep/generated/commit_event.py','verif/protocol/generated/memory_protocol.py',
        'sw/link/platform.ld','tests/test_sail_log.py']
    paths += [str(p.relative_to(ROOT)) for p in sorted((ROOT/lock['harness']['directory']).iterdir()) if p.is_file()]
    inputs = {p:digest(ROOT/p) for p in paths}
    fingerprint = {'inputs':inputs,'sail_sha256':digest(sail),'versions':versions}
    run_hash = hashlib.sha256(json.dumps(fingerprint,sort_keys=True).encode()).hexdigest()[:16]
    out = ROOT/'out/sail_differential'/run_hash; out.mkdir(parents=True,exist_ok=True)
    receipt = out/'receipt.json'; receipt.unlink(missing_ok=True)
    override = out/'override.json'; override.write_text(json.dumps(settings['sail_override'])+'\n')
    def run(command,log):
        result = subprocess.run([str(c) for c in command],cwd=ROOT,text=True,stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT,timeout=300)
        (out/log).write_text(result.stdout)
        if result.returncode: raise RuntimeError(f'command failed; see {out/log}\n{result.stdout[-1500:]}')
        return result.stdout
    binary = build_core(config)
    results = []; references = {}
    for seed in settings['seeds']:
        image,boot,end_pc = program(seed)
        raw = out/f'{seed}.bin'; raw.write_bytes(image)
        assembly = out/f'{seed}.S'
        assembly.write_text(f'.section .text.init\n.globl _start\n_start:\n.incbin "{raw.relative_to(ROOT)}"\n')
        elf = out/f'{seed}.elf'
        run(['riscv64-unknown-elf-gcc','-march=rv32i','-mabi=ilp32','-nostdlib','-nostartfiles',
             '-Wl,--no-relax,--build-id=none','-T','sw/link/platform.ld',assembly,'-o',elf],f'compile_{seed}.log')
        trace = out/f'{seed}.trace'
        run([sail,'--config',ROOT/'verif/arch/act4/sail.json','--config-override',override,
             '--trace-instr','--trace-reg','--trace-mem','--trace-exception','--trace-step',
             '--trace-output',trace,'--inst-limit',settings['instruction_limit']+1,elf],f'sail_{seed}.log')
        expected = parse_sail_log(trace.read_text())
        if len(expected) != settings['instruction_limit']: raise ValueError('Sail trace is incomplete')
        if [(e['pc_before'],e['instruction']) for e in expected[:len(boot)]] != [(4*n,ins) for n,ins in enumerate(boot)]:
            raise ValueError('Sail initialization sequence differs')
        end = next((n for n,e in enumerate(expected) if e['pc_before'] == end_pc),None)
        if end is None: raise ValueError('Sail did not reach program end')
        expected = expected[:end+1]
        covered = coverage(expected[len(boot):])
        references[seed] = (raw,image,boot,expected)
        modes = ['normal'] + (['reset_fetch','reset_data','reset_commit','stall_store'] if seed == 42 else [])
        for mode in modes:
            output = run([binary,raw,seed,len(expected),mode,'+verilator+rand+reset+2',f'+verilator+seed+{seed}'],f'core_{seed}_{mode}.log')
            check_trace(output,image,mode)
            count = compare_output(output,expected,boot)
            results.append(dict(seed=seed,mode=mode,compared_events=count,coverage=covered,elf_sha256=digest(elf),
                                sail_trace_sha256=digest(trace),rtl_log_sha256=digest(out/f'core_{seed}_{mode}.log'),
                                bootstrap_events_per_reset=len(boot)))
            print(f'Sail differential seed={seed} {mode}: PASS ({count} events)',flush=True)
    raw,image,boot,expected = references[42]
    mutations = []
    for name,file,old,new in [
        ('load_sign','rtl/core/single_lane_core.sv',"{{24{selected_word[7]}}, selected_word[7:0]}","{24'd0, selected_word[7:0]}"),
        ('trap_value','rtl/execute/execute_single.sv','trap_value_o = address_o;','trap_value_o = address_o + 4;'),
    ]:
        text = (ROOT/file).read_text()
        if text.count(old) != 1: raise ValueError('RTL mutation no longer matches')
        directory = out/name; directory.mkdir(exist_ok=True)
        changed = directory/Path(file).name; changed.write_text(text.replace(old,new))
        sources = [changed if p == file else p for p in config['sources']]
        run(['verilator','--cc','--exe','--build','--build-jobs','2','--Wall','-Wno-UNUSEDPARAM','-Wno-UNUSEDSIGNAL',
             '--assert','--top-module',config['top'],'--Mdir',directory/'obj_dir','-CFLAGS',f'-std=c++17 -I{CORE_OUT}',
             *sources,ROOT/'verif/core/single_lane_tb.cpp','-o','core_check'],name+'_build.log')
        output = run([directory/'obj_dir/core_check',raw,42,len(expected),'normal'],name+'.log')
        try: compare_output(output,expected,boot)
        except ComparisonError: mutations.append(name)
        else: raise RuntimeError(f'Sail comparison missed {name} RTL mutation')
        print(f'Sail RTL mutation {name}: PASS (detected)',flush=True)
    if any(digest(ROOT/p) != h for p,h in inputs.items()): raise ValueError('inputs changed during differential run')
    receipt.write_text(json.dumps({**fingerprint,'schema':1,'profile':settings['profile'],'runs':results,
        'core_binary_sha256':digest(binary),'sail_override':settings['sail_override'],
        'mutations_detected':mutations,'limitations':settings['limitations'],'architectural_slice_accepted':False},indent=2)+'\n')
    print(f'Receipt: {receipt.relative_to(ROOT)}')


if __name__ == '__main__':
    main()
