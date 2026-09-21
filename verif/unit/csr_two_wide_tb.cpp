#include "Vcsr_two_wide.h"
#include "csr_reference_layout.hpp"
#include "verilated.h"
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <random>
#include <string>

using Effect = std::array<uint32_t, 7>;
struct Reply {
    bool legal=false, read=false, trap=false;
    uint32_t value=0, next=0;
    std::array<Effect, 4> effects{};
};
struct Input {
    bool reset=false, cancel=false, request=false, ready=false, trap=false;
    unsigned retired=0;
    uint32_t instruction=0, source=0, pc=0, cause=0, value=0;
};
uint32_t csr(unsigned address, unsigned kind=2, unsigned source=0, unsigned dest=1) {
    return (address << 20) | (source << 15) | (kind << 12) | (dest << 7) | 0x73;
}
struct Bench {
    Vcsr_two_wide dut;
    std::map<unsigned, uint32_t> state;
    std::map<std::string, uint64_t> coverage;
    uint64_t cycles=0;
    bool pending=false;
    Reply held;
    Bench() { reset_state(); }
    [[noreturn]] void fail(const std::string& message) const {
        std::cerr << "CSR two-wide mismatch cycle=" << cycles << " " << message << '\n';
        std::exit(1);
    }
    void check(bool value, const std::string& message) const { if (!value) fail(message); }
    void reset_state() { state.clear(); for (const auto& c : CSR_SPEC) state[c.address]=c.reset; }
    const Spec* spec(unsigned address) const {
        for (const auto& c : CSR_SPEC) if (c.address == address) return &c;
        return nullptr;
    }
    Effect effect(unsigned address, uint32_t data, uint32_t mask, unsigned reason) const {
        const auto& c=*spec(address);
        return {1, address, state.at(address), data, c.write_mask | c.fixed_mask, mask, reason};
    }
    Reply propose(const Input& in) const {
        Reply out;
        out.next=in.pc;
        out.trap=in.trap;
        if (in.trap) {
            out.legal=true; out.next=state.at(0x305);
            const uint32_t status=(state.at(0x300) & ~0x88u) | ((state.at(0x300) & 8u) << 4);
            out.effects={effect(0x300,status,0x88,2),effect(0x341,in.pc & ~3u,0xfffffffcu,2),
                         effect(0x342,in.cause,0xffffffffu,2),effect(0x343,in.value,0xffffffffu,2)};
            return out;
        }
        if (in.instruction == 0x30200073) {
            out.legal=true; out.next=state.at(0x341);
            out.effects[0]=effect(0x300,(state.at(0x300)&~0x88u)|0x80u|((state.at(0x300)>>4)&8u),0x88,3);
            return out;
        }
        unsigned kind=(in.instruction>>12)&7, rs=(in.instruction>>15)&31, rd=(in.instruction>>7)&31;
        const auto* c=spec(in.instruction>>20);
        if ((in.instruction&127)!=0x73 || kind==0 || kind==4 || !c) return out;
        const bool write=(kind==1 || kind==5 || rs!=0);
        if (write && c->readonly) return out;
        out.legal=true;
        out.read=!((kind==1 || kind==5) && rd==0);
        const uint32_t old=state.at(c->address);
        const uint32_t operand=kind>=5 ? rs : (rs ? in.source : 0);
        uint32_t data=old;
        if (kind==1 || kind==5) data=operand;
        else if (kind==2 || kind==6) data=old | operand;
        else data=old & ~operand;
        data=(data & c->write_mask) | (old & ~c->write_mask);
        data=(data & ~c->fixed_mask) | c->fixed_value;
        out.value=out.read ? old : 0;
        out.next=in.pc+4;
        out.effects[0]=effect(c->address,write ? data : old,write ? c->write_mask : 0,1);
        return out;
    }
    uint32_t bits(unsigned offset, unsigned width) const {
        uint32_t result=0;
        for (unsigned b=0;b<width;b++)
            result |= ((dut.effects_o[(offset+b)/32] >> ((offset+b)%32))&1u) << b;
        return result;
    }
    void tick(const Input& in={}) {
        dut.clk_i=0; dut.rst_i=in.reset; dut.cancel_i=in.cancel;
        dut.retire_count_i=in.retired; dut.request_valid_i=in.request; dut.response_ready_i=in.ready;
        dut.instruction_i=in.instruction; dut.source_i=in.source; dut.pc_i=in.pc;
        dut.trap_i=in.trap; dut.cause_i=in.cause; dut.trap_value_i=in.value;
        dut.eval();
        const bool valid=pending && !in.reset && !in.cancel;
        const bool request_ready=!pending && !in.reset && !in.cancel;
        const bool accept=valid && in.ready;
        const Reply expected=valid ? held : Reply{};
        check(dut.request_ready_o==request_ready,"request ready");
        check(dut.response_valid_o==valid && dut.accept_o==accept,"response handshake");
        check(dut.legal_o==expected.legal && dut.read_o==expected.read,"legality/read");
        check(dut.value_o==expected.value && dut.next_pc_o==expected.next,"value/next PC");
        check(dut.retired_o==(accept && held.legal && !held.trap),"retirement");
        check(dut.trap_accept_o==(accept && held.legal && held.trap),"trap acceptance");
        check(in.reset || (dut.mtvec_o==state.at(0x305) && dut.mepc_o==state.at(0x341)),"architectural target state");
        for (unsigned e=0;e<4;e++) for (unsigned f=0;f<7;f++)
            check(bits(e*EFFECT_BITS+FIELD_OFFSET[f],FIELD_WIDTH[f])==expected.effects[e][f],
                  "effect "+std::to_string(e)+" field "+std::to_string(f));
        Reply prepared;
        if (in.request && request_ready) prepared=propose(in);
        if (in.reset) { pending=false; reset_state(); coverage["reset"]++; }
        else {
            const unsigned inhibit=state.at(0x320);
            const bool commit=accept && held.legal;
            bool cycle_write=false, instret_write=false;
            if (commit) for (const auto& e:held.effects) if (e[0] && e[5]) {
                cycle_write |= e[1]==0xb00 || e[1]==0xb80;
                instret_write |= e[1]==0xb02 || e[1]==0xb82;
            }
            auto increment=[&](unsigned lo,unsigned hi,uint64_t amount,const std::string& name) {
                uint64_t old=(uint64_t(state.at(hi))<<32)|state.at(lo), next=old+amount;
                if (uint64_t(state.at(lo))+amount>0xffffffffull) coverage[name+"_carry"]++;
                if (next<old) coverage[name+"_wrap"]++;
                state[lo]=uint32_t(next); state[hi]=uint32_t(next>>32);
            };
            if (!(inhibit&1) && !cycle_write) increment(0xb00,0xb80,1,"cycle");
            else coverage[cycle_write ? "cycle_write_priority" : "cycle_inhibit"]++;
            if (!(inhibit&4) && !instret_write)
                increment(0xb02,0xb82,commit && !held.trap ? 1 : in.retired,"instret");
            else coverage[instret_write ? "instret_write_priority" : "instret_inhibit"]++;
            if (in.retired==2) coverage["dual_retire"]++;
            if (in.retired==1) coverage["single_retire"]++;
            if (valid && !in.ready) coverage["held"]++;
            if (commit) {
                for (const auto& e:held.effects) if (e[0]) state[e[1]]=(state.at(e[1])&~e[5])|(e[3]&e[5]);
                coverage[held.trap ? "trap" : (held.effects[0][6]==3 ? "mret" : "csr")]++;
                if (!held.trap && held.effects[0][6]==1) {
                    coverage[held.read ? "csr_read" : "read_suppressed"]++;
                    if (!held.effects[0][5]) coverage["no_effective_write"]++;
                }
            }
            if (accept && !held.legal) coverage["illegal"]++;
            if (in.cancel && pending) coverage["cancel"]++;
            if (in.cancel || accept) pending=false;
            if (in.request && request_ready) { held=prepared; pending=true; coverage["prepare"]++; }
        }
        dut.clk_i=1; dut.eval(); dut.clk_i=0; dut.eval(); cycles++;
    }
    void command(Input in, unsigned stalls=0, bool cancel=false, bool reset=false) {
        check(!pending,"test starts command while busy");
        in.request=true; tick(in);
        for (unsigned i=0;i<stalls;i++) {
            Input noise; noise.request=true; noise.instruction=~in.instruction; noise.source=~in.source;
            noise.trap=!in.trap; noise.pc=~in.pc; noise.cause=~in.cause; noise.value=~in.value;
            tick(noise);
        }
        Input done; done.ready=true; done.cancel=cancel; done.reset=reset; tick(done);
    }
    void write(unsigned addr,uint32_t value) {
        Input in; in.instruction=csr(addr,1,1,0); in.source=value; command(in);
    }
    void audit() {
        for (const auto& c:CSR_SPEC) { Input in; in.instruction=csr(c.address); command(in,1,true); }
    }
};
int main(int argc,char** argv) {
    Verilated::commandArgs(argc,argv);
    const std::string arg=argc>1?argv[1]:"1";
    Bench b;
    Input reset; reset.reset=true; b.tick(reset);
    if (arg=="--bad-count") { Input in; in.retired=3; b.tick(in); return 0; }
    if (arg=="--bad-serialization") {
        Input in; in.instruction=csr(0x340); in.request=true; b.tick(in);
        in={}; in.retired=1; b.tick(in); return 0;
    }
    const unsigned seed=std::stoul(arg); std::mt19937 random(seed);
    b.audit();
    for (const auto& c:CSR_SPEC) for (unsigned kind:{1,2,3,5,6,7})
        for (unsigned rs:{0,1,31}) for (unsigned rd:{0,7}) {
            Input in; in.instruction=csr(c.address,kind,rs,rd); in.source=random(); in.pc=random()&~3u;
            b.command(in,random()%4); b.coverage["kind_"+std::to_string(kind)]++;
        }
    b.write(0x320,0);
    for (unsigned addr=0;addr<4096;addr++) {
        for (unsigned kind:{1,2,3,5,6,7}) {
            Input in; in.instruction=csr(addr,kind,(addr+kind)%2 ? 0:3,(addr+kind)%3 ? 1:0);
            in.source=0; in.pc=0xfffffffc; b.command(in);
        }
        b.coverage["address_sweep"]++;
    }
    for (uint32_t word:{0u,0xffffffffu,0x13u,0x00000073u,0x00100073u,0x10500073u,0x10200073u,0x30004073u}) {
        Input in; in.instruction=word; b.command(in,2); b.coverage["unsupported_instruction"]++;
    }
    for (unsigned mie:{0,8}) for (unsigned cause:{0,1,2,3,4,5,6,7,11}) {
        b.write(0x300,mie); b.write(0x305,random());
        Input in; in.trap=true; in.pc=random(); in.cause=cause; in.value=cause==11 ? 0 : random();
        in.instruction=0xffffffff; b.command(in,5); b.audit();
        in={}; in.instruction=0x30200073; b.command(in,4); b.audit();
        b.coverage["cause_"+std::to_string(cause)]++;
    }
    for (unsigned high:{7u,0xffffffffu}) {
        b.write(0x320,5);
        b.write(0xb80,high); b.write(0xb00,0xfffffffe);
        b.write(0xb82,high); b.write(0xb02,0xffffffff);
        b.write(0x320,0);
        Input in; in.retired=2; b.tick(in); b.tick(in); b.audit();
    }
    for (const auto& c:CSR_SPEC) {
        Input in; in.instruction=csr(c.address,1,1); in.source=0xdeadbeef;
        b.command(in,3,true); b.audit(); b.coverage["cancel_csr"]++;
        b.command(in,2,false,true); b.audit(); b.coverage["reset_csr"]++;
    }
    for (unsigned k=0;k<2;k++) {
        Input in; in.trap=k==0; in.instruction=k ? 0x30200073 : 0; in.pc=0x804; in.value=123; in.cause=2;
        b.command(in,4,true); b.audit(); b.command(in,3,false,true); b.audit();
        b.coverage[k ? "cancel_reset_mret" : "cancel_reset_trap"]++;
    }
    for (unsigned n=0;n<10000;n++) {
        if (random()%3==0) { Input in; in.retired=random()%3; b.tick(in); }
        else {
            Input in;
            unsigned kind=std::array<unsigned,6>{1,2,3,5,6,7}[random()%6];
            in.instruction=csr(CSR_SPEC[random()%CSR_SPEC.size()].address,kind,random()%32,random()%32);
            in.source=random()%4 ? random() : 0; in.pc=random();
            if (n%13==0) in.instruction=0x30200073;
            if (n%17==0) { in.trap=true; in.cause=2; in.value=random(); }
            b.command(in,random()%8,random()%9==0,random()%101==0);
        }
        if (n%100==0) b.audit();
    }
    b.audit();
    std::cout << "CSR TWO WIDE PASS seed=" << seed << " cycles=" << b.cycles;
    for (const auto& [name,count]:b.coverage) std::cout << ' ' << name << '=' << count;
    std::cout << '\n';
}
