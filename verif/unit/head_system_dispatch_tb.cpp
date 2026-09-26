#include "Vhead_system_dispatch.h"
#include "verilated.h"
#include <array>
#include <cstdint>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
#include <string>

struct Input {
    bool reset=false, flush=false, drain=false, recover=false, head=false, retire=false;
    unsigned valid=0, system=0, cfi=0, accept=0, head_id=0, branch_id=0, serial_id=0;
    uint32_t head_pc=0;
    std::array<unsigned,2> id{}, source{};
    std::array<uint32_t,2> instruction{}, pc{};
};
struct Owner { unsigned id, source; uint32_t instruction, pc; };
class Check {
    Vhead_system_dispatch dut;
public:
    bool busy=false;
    Owner owner{};
    uint64_t cycles=0;
    std::map<std::string,uint64_t> coverage;
    void require(bool value, const std::string& what) const {
        if (!value) throw std::runtime_error("head system dispatch mismatch cycle="+std::to_string(cycles)+" "+what);
    }
    static unsigned position(unsigned slot, unsigned head) {
        for (unsigned n=0;n<32;n++) if ((head+n)%32==slot) return n;
        throw std::runtime_error("invalid slot");
    }
    unsigned selected(const Input& in) const {
        if (busy || in.reset || in.flush || in.drain || in.recover || !(in.valid&1)) return 0;
        if (in.valid==3 && !(in.cfi&1) && in.system==0) return 3;
        return 1;
    }
    bool at_head(const Input& in) const {
        return busy && in.head && in.head_id==owner.id && in.head_pc==owner.pc
            && !in.reset && !in.flush && !in.drain && !in.recover;
    }
    void step(const Input& in) {
        dut.clk_i=0; dut.rst_i=in.reset; dut.flush_i=in.flush; dut.drained_i=in.drain;
        dut.valid_i=in.valid; dut.system_i=in.system; dut.cfi0_i=in.cfi&1;
        dut.instruction_i=in.instruction[0];
        dut.pc_i=in.pc[0];
        dut.allocate_accept_i=in.accept; dut.allocate_id_i=in.id[0];
        dut.source1_i=in.source[0];
        dut.head_valid_i=in.head; dut.head_id_i=in.head_id; dut.head_pc_i=in.head_pc;
        dut.recover_i=in.recover; dut.recover_slot_i=in.branch_id%32;
        dut.serial_accept_i=in.retire; dut.serial_id_i=in.serial_id;
        dut.eval();
        const bool killed=busy && (in.reset || in.flush || (in.recover
            && position(owner.id%32,in.head_id%32)>position(in.branch_id%32,in.head_id%32)));
        const unsigned dispatch=selected(in);
        require(dut.dispatch_valid_o==dispatch,"selected prefix");
        require(dut.dispatch_solo_o==(dispatch&in.system),"solo reservation");
        const unsigned queued=(in.reset || in.flush || in.drain || in.recover || busy) ? 0 : in.accept&~in.system&3;
        require(dut.queue_dispatch_o==queued,"IQ admission");
        require(dut.busy_o==busy && dut.killed_o==killed,"descriptor ownership");
        require(dut.system_valid_o==at_head(in),"full head identity and PC");
        require(dut.system_id_o==(busy ? owner.id:0),"saved identity");
        require(dut.system_instruction_o==(busy ? owner.instruction:0),"saved instruction");
        require(dut.system_pc_o==(busy ? owner.pc:0),"saved PC");
        require(dut.system_source_o==(busy ? owner.source:0),"saved physical source");
        if (dispatch==3) coverage["dual_ordinary"]++;
        if (dispatch==1 && in.valid==3 && in.system==2) coverage["lane1_deferred"]++;
        if (dispatch==1 && in.valid==3 && (in.system&1)) coverage["system_tail_cut"]++;
        if (dispatch==1 && in.valid==3 && (in.cfi&1)) coverage["cfi_cut"]++;
        if (dispatch && !in.accept) coverage["allocation_stall"]++;
        if (in.drain && !busy) coverage["drain_idle"]++;
        if (in.recover && in.valid && !busy) coverage["recovery_blocked"]++;
        if (in.recover && busy && in.head_id==owner.id && in.head_pc==owner.pc) coverage["head_recovery_blocked"]++;
        if (busy) {
            coverage["held"]++;
            if (in.valid) coverage["younger_blocked"]++;
            if (in.instruction[0]!=owner.instruction && in.source[0]!=owner.source) coverage["changed_inputs"]++;
            if (!in.head) coverage["no_head"]++;
            if (in.head && in.head_id%32==owner.id%32 && in.head_id!=owner.id) coverage["stale_generation"]++;
            if (in.head && in.head_id==owner.id && in.head_pc!=owner.pc) coverage["wrong_pc"]++;
            if (in.head && in.head_id%32!=owner.id%32) coverage["older_head"]++;
            if (in.recover) {
                coverage[killed ? "recover_kill":"recover_preserve"]++;
                if (in.branch_id%32<in.head_id%32 || owner.id%32<in.head_id%32) coverage["wrap_recovery"]++;
            }
            if (in.reset) coverage["reset_live"]++;
            if (in.flush) coverage["flush_live"]++;
            if (in.retire) { coverage["retired"]++; if (in.valid && dispatch==0) coverage["release_bubble"]++; }
        }
        if (in.reset || in.flush || killed || (in.retire && in.serial_id==owner.id)) busy=false;
        else if ((in.accept&1) && (in.system&1)) {
            busy=true; owner={in.id[0],in.source[0],in.instruction[0],in.pc[0]};
            coverage["captured"]++;
            coverage[owner.source ? "register_source":"zero_source"]++;
            if (owner.instruction==0x30200073) coverage["mret"]++;
            else if (owner.instruction==0x10500073) coverage["wfi"]++;
            else if ((owner.instruction>>14)&1) coverage["immediate"]++;
            else coverage["register_csr"]++;
            if (owner.id>=32) coverage["generation_capture"]++;
        }
        dut.clk_i=1; dut.eval(); cycles++;
    }
};
uint32_t csr(unsigned kind, unsigned rs, unsigned rd=1) {
    return 0x34000073u | kind<<12 | rs<<15 | rd<<7;
}
Input capture(unsigned id, unsigned source, uint32_t instruction=csr(1,5)) {
    Input in; in.valid=3; in.system=1; in.accept=1;
    in.id={id,(id+1)%8192}; in.source={source,63};
    in.instruction={instruction,0x00500113}; in.pc={0x80000000u+id*4,0x80000004u+id*4};
    return in;
}
Input matching(const Check& check) {
    Input in; in.head=true; in.head_id=check.owner.id; in.head_pc=check.owner.pc;
    in.valid=3; in.system=3; in.instruction={0xffffffffu,0x12345678}; in.source={63-check.owner.source,0};
    return in;
}
void clear(Check& check) { Input in; in.flush=true; check.step(in); }
void directed(Check& check) {
    Input in; in.reset=true; check.step(in);
    for (unsigned valid : {0u,1u,3u}) for (unsigned system=0;system<4;system++) for (unsigned cfi=0;cfi<4;cfi++) {
        in={}; in.valid=valid; in.system=system; in.cfi=cfi; check.step(in);
    }
    in={}; in.valid=3; in.accept=3; check.step(in);
    in={}; in.drain=true; in.valid=3; check.step(in);
    in={}; in.recover=true; in.head=true; in.valid=3; check.step(in);
    for (unsigned kind : {1u,2u,3u,5u,6u,7u,0u,8u}) for (unsigned source=0;source<64;source++) {
        const uint32_t instruction=kind==8 ? 0x10500073:kind ? csr(kind,source%32):0x30200073;
        const unsigned tag=kind && kind<4 && source%32 ? source:0;
        check.step(capture((source*37+kind*257)%8192,tag,instruction));
        in=matching(check); check.step(in);
        in.head=false; check.step(in);
        for (unsigned bit=5;bit<13;bit++) { in=matching(check); in.head_id^=1u<<bit; check.step(in); }
        in=matching(check); in.head_pc^=4; check.step(in);
        in=matching(check); in.head_id=(in.head_id&~31u)|((in.head_id+1)%32); check.step(in);
        in=matching(check); in.retire=true; in.serial_id=check.owner.id; check.step(in);
    }
    // Enumerate circular ROB order independently of numeric slot order.
    for (unsigned head=0;head<32;head++) for (unsigned entry=0;entry<32;entry++) for (unsigned branch=0;branch<32;branch++) {
        check.step(capture(32*193+(head+entry)%32,37));
        in=matching(check); in.recover=true; in.head_id=entry==0 ? check.owner.id:32*7+head; in.branch_id=32*61+(head+branch)%32;
        check.step(in); clear(check);
    }
    check.step(capture(63,1)); in=matching(check); in.reset=true; check.step(in);
    check.step(capture(63,2)); clear(check);
    check.step(capture(8191,3)); in=matching(check); in.retire=true; in.serial_id=8191; check.step(in);
    check.step(capture(31,4)); clear(check);
}
void negative(Check& check,const std::string& name) {
    Input in; in.reset=true; check.step(in);
    if (name=="prefix") { in={}; in.valid=2; }
    else if (name=="allocation") { in={}; in.accept=1; }
    else if (name=="operation") { in=capture(4,3,0x13); }
    else if (name=="source") { in=capture(4,3,csr(5,3)); }
    else if (name=="wfi-source") { in=capture(4,3,0x10500073); }
    else if (name=="retire") { check.step(capture(4,3)); in=matching(check); in.retire=true; in.serial_id=36; }
    else if (name=="drain") { check.step(capture(4,3)); in={}; in.drain=true; }
    else if (name=="recovery") { in={}; in.recover=true; }
    else throw std::runtime_error("unknown negative test");
    check.step(in);
    throw std::runtime_error("caller assertion did not fire");
}
int main(int argc,char** argv) {
    Verilated::commandArgs(argc,argv);
    try {
        Check check;
        if (argc>1 && std::string(argv[1])=="negative") { negative(check,argv[2]); return 1; }
        unsigned seed=argc>1 ? std::stoul(argv[1]):1, random_cycles=argc>2 ? std::stoul(argv[2]):10000;
        std::mt19937 rng(seed); directed(check);
        for (unsigned cycle=0;cycle<random_cycles;cycle++) {
            Input in;
            if (!check.busy) {
                if (rng()%3==0) {
                    const unsigned kind=std::array<unsigned,8>{1,2,3,5,6,7,0,8}[rng()%8], rs=rng()%32;
                    in=capture(rng()%8192,kind && kind<4 && rs ? 1+rng()%63:0,kind==8 ? 0x10500073:kind ? csr(kind,rs):0x30200073);
                    if (rng()%4==0) in.accept=0;
                } else {
                    in.valid=rng()%2 ? 3:1; in.cfi=rng()%4; in.system=rng()%2 ? 2:0;
                    in.accept=rng()%2 ? check.selected(in):0;
                }
            } else {
                in=matching(check);
                switch (rng()%9) {
                case 0: in.flush=true; break;
                case 1: in.reset=true; break;
                case 2: in.recover=true; in.branch_id=rng()%8192; in.head_id=rng()%8192; break;
                case 3: in.head_id^=32; break;
                case 4: in.head_pc^=4; break;
                case 5: in.head=false; break;
                case 6: in.retire=true; in.serial_id=check.owner.id; break;
                default: break;
                }
            }
            check.step(in);
        }
        std::cout<<"HEAD SYSTEM DISPATCH PASS seed="<<seed<<" cycles="<<check.cycles;
        for (const auto& [key,value]:check.coverage) std::cout<<" "<<key<<"="<<value;
        std::cout<<'\n';
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
