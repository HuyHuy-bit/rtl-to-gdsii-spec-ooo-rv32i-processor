#pragma once
#include "packed_bits.hpp"
using packed_bits::bit;
using packed_bits::put;
using packed_bits::get32;
#include "Vfetch_execution_core.h"
#include "verilated.h"
#include "backend_event_layout.hpp"
#include "fetch_memory_layout.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
#include <string>

using Event = std::array<uint32_t, (EVENT_BITS+31)/32>;
#include "rv32_reference.hpp"
#include "csr_reference.hpp"
using rv32::instruction;
using rv32::signed_value;
using rv32::arithmetic_right;
using rv32::extend;
using rv32::illegal;
using rv32::operation;
using rv32::evaluate;
struct Input {
    bool reset=false, enable=true, flush=false, drain=false, grant=true, request_ready=true;
    uint32_t target=0;
    unsigned execute=3, complete=3, retire=3, latency=1;
    bool inject_bad=false, trap_ready=false;
};
struct Expected { Event event{}; unsigned op, rd; uint32_t result, next_pc; bool fault, taken=false; };
struct Bench {
    Vfetch_execution_core d;
    std::array<uint32_t,16384> memory{};
    std::array<uint32_t,32> regs{};
    std::array<bool,32> known{};
    std::map<std::string,unsigned> coverage;
    bool fault_support=false, trap_service=false, system_service=false, system_prepared=false;
    CsrModel csr_state;
    Reply system_reply;
    Expected system_expected;
    std::array<uint32_t,4> trap_csrs{0x1800,0,0,0};
    unsigned traps=0;
    unsigned cycles=0, retired=0, requests=0, expected_id=0, pending_id=0, delay=0;
    uint32_t pc=0, pending_address=0, error_address=UINT32_MAX;
    uint64_t order=0;
    bool pending=false, held=false, fatal=false;
    std::array<uint32_t,(REQUEST_BITS+31)/32> held_request{};
    void require(bool good,const std::string& why) const {
        if (!good) throw std::runtime_error("fetch execution core mismatch cycle="+std::to_string(cycles)+": "+why);
    }
    static bool system_instruction(uint32_t insn) {
        const unsigned kind=(insn>>12)&7;
        return insn==0x30200073 || insn==0x10500073 || ((insn&127)==0x73 && kind!=0 && kind!=4);
    }
    static bool decoded_fault(uint32_t insn) {
        return illegal(insn) || insn==0x00000073 || insn==0x00100073;
    }
    static void effects(Event& event,const Reply& reply) {
        for (unsigned n=0;n<4;n++) for (unsigned f=0;f<7;f++)
            put(event,n*CSR_EFFECT_BITS+FIELD_OFFSET[f],FIELD_WIDTH[f],reply.effects[n][f]);
    }
    Expected expected_system() {
        const uint32_t insn=memory[pc/4];
        const bool mret=insn==0x30200073;
        const unsigned source=mret || (insn&(1u<<14)) ? 0 : (insn>>15)&31;
        require(known[source],"system source initialized");
        Command command; command.instruction=insn; command.source=regs[source]; command.pc=pc;
        system_reply=csr_state.propose(command);
        Expected e{}; e.op=mret ? 30:insn==0x10500073 ? 31:29; e.rd=mret ? 0:(insn>>7)&31;
        e.result=system_reply.value; e.next_pc=system_reply.legal ? system_reply.next:pc; e.fault=!system_reply.legal;
        put(e.event,VALID_OFFSET,1,1); put(e.event,ORDER_OFFSET,64,order);
        put(e.event,INSTRUCTION_OFFSET,32,insn); put(e.event,PRIVILEGE_OFFSET,2,3);
        put(e.event,PC_BEFORE_OFFSET,32,pc); put(e.event,PC_AFTER_OFFSET,32,e.next_pc);
        put(e.event,RS1_ADDR_OFFSET,5,source); put(e.event,RS1_VALUE_OFFSET,32,regs[source]);
        put(e.event,RETIRED_OFFSET,1,!e.fault);
        if (e.fault) {
            e.rd=0; put(e.event,TRAP_OFFSET,1,1); put(e.event,TRAP_CAUSE_OFFSET,32,2); put(e.event,TRAP_VALUE_OFFSET,32,insn);
        } else {
            if (e.rd) { put(e.event,RD_ADDR_OFFSET,5,e.rd); put(e.event,RD_VALUE_OFFSET,32,e.result); put(e.event,RD_WRITE_MASK_OFFSET,32,UINT32_MAX); }
            effects(e.event,system_reply);
        }
        return e;
    }
    Expected expected() {
        if (fault_support && (pc%4 || pc>=65536 || (pc&~31U)==error_address || decoded_fault(memory[pc/4]))) {
            const bool fetch=pc%4 || pc>=65536 || (pc&~31U)==error_address;
            const uint32_t insn=fetch ? 0 : memory[pc/4];
            Expected e{}; e.fault=true; e.next_pc=pc;
            put(e.event,VALID_OFFSET,1,1); put(e.event,ORDER_OFFSET,64,order);
            put(e.event,INSTRUCTION_OFFSET,32,insn); put(e.event,PRIVILEGE_OFFSET,2,3);
            put(e.event,PC_BEFORE_OFFSET,32,pc); put(e.event,PC_AFTER_OFFSET,32,pc);
            put(e.event,TRAP_OFFSET,1,1);
            put(e.event,TRAP_CAUSE_OFFSET,32,fetch ? (pc%4 ? 0 : 1) : insn==0x73 ? 11 : insn==0x100073 ? 3 : 2);
            put(e.event,TRAP_VALUE_OFFSET,32,fetch ? pc : insn==0x73 ? 0 : insn==0x100073 ? pc : insn);
            return e;
        }
        require(pc<65536 && pc%4==0,"reference executable PC");
        const uint32_t insn=memory[pc/4];
        if (system_service && system_instruction(insn)) {
            require(system_prepared,"system result without preparation");
            return system_expected;
        }
        require(operation(insn)>=0,"unsupported instruction retired");
        Expected e; e.op=unsigned(operation(insn));
        e.rd=e.op>=21 && e.op<=26 ? 0 : (insn>>7)&31;
        unsigned a=e.op<2 || e.op==27 ? 0 : (insn>>15)&31;
        unsigned b=e.op<11 || e.op>=27 ? 0 : (insn>>20)&31;
        require(known[a] && known[b],"read of unspecified register");
        e.result=evaluate(e.op,insn,pc,regs[a],regs[b]); e.next_pc=pc+4;
        if (e.op>=21 && e.op<=26) {
            switch (e.op) {
            case 21: e.taken=regs[a]==regs[b]; break;
            case 22: e.taken=regs[a]!=regs[b]; break;
            case 23: e.taken=signed_value(regs[a])<signed_value(regs[b]); break;
            case 24: e.taken=signed_value(regs[a])>=signed_value(regs[b]); break;
            case 25: e.taken=regs[a]<regs[b]; break;
            case 26: e.taken=regs[a]>=regs[b]; break;
            }
            uint32_t offset=((insn>>31)&1)<<12 | ((insn>>7)&1)<<11 | ((insn>>25)&63)<<5 | ((insn>>8)&15)<<1;
            if (e.taken) e.next_pc=pc+extend(offset,13);
        } else if (e.op==27) {
            uint32_t offset=((insn>>31)&1)<<20 | ((insn>>12)&255)<<12 | ((insn>>20)&1)<<11 | ((insn>>21)&1023)<<1;
            e.taken=true; e.next_pc=pc+extend(offset,21);
        } else if (e.op==28) {
            e.taken=true; e.next_pc=(regs[a]+extend(insn>>20,12))&~1U;
        }
        e.fault=e.next_pc%4!=0;
        put(e.event,VALID_OFFSET,1,1); put(e.event,ORDER_OFFSET,64,order);
        put(e.event,INSTRUCTION_OFFSET,32,insn); put(e.event,PRIVILEGE_OFFSET,2,3);
        put(e.event,PC_BEFORE_OFFSET,32,pc); put(e.event,PC_AFTER_OFFSET,32,e.next_pc);
        put(e.event,RS1_ADDR_OFFSET,5,a); put(e.event,RS1_VALUE_OFFSET,32,regs[a]);
        put(e.event,RS2_ADDR_OFFSET,5,b); put(e.event,RS2_VALUE_OFFSET,32,regs[b]);
        put(e.event,RETIRED_OFFSET,1,!e.fault);
        if (e.fault) { put(e.event,TRAP_OFFSET,1,1); put(e.event,TRAP_VALUE_OFFSET,32,e.next_pc); }
        else if (e.rd) {
            put(e.event,RD_ADDR_OFFSET,5,e.rd); put(e.event,RD_VALUE_OFFSET,32,e.result);
            put(e.event,RD_WRITE_MASK_OFFSET,32,UINT32_MAX);
        }
        return e;
    }
    template<class T> void compare(const T& actual,unsigned offset,const Event& want) {
        for (unsigned n=0; n<EVENT_BITS; ++n)
            require(bit(actual,offset+n)==bit(want,n),"architectural event bit "+std::to_string(n)+" PC="+std::to_string(pc));
    }
    Event finalized_fault() {
        auto raw=expected(); require(raw.fault,"trap without reference fault");
        auto e=raw.event;
        if (system_service) {
            Command command; command.trap=true; command.pc=pc;
            command.cause=get32(e,TRAP_CAUSE_OFFSET,32); command.value=get32(e,TRAP_VALUE_OFFSET,32);
            const auto reply=csr_state.propose(command);
            put(e,PC_AFTER_OFFSET,32,reply.next); effects(e,reply); return e;
        }
        put(e,PC_AFTER_OFFSET,32,0x100);
        const unsigned addresses[4]={0x300,0x341,0x342,0x343};
        const uint32_t masks[4]={0x88,0xfffffffc,0xffffffff,0xffffffff};
        const uint32_t reads[4]={0x1888,0xffffffff,0xffffffff,0xffffffff};
        const uint32_t values[4]={(trap_csrs[0]&~0x88U)|((trap_csrs[0]&8U)<<4),pc&~3U,
            get32(e,TRAP_CAUSE_OFFSET,32),get32(e,TRAP_VALUE_OFFSET,32)};
        for (unsigned n=0;n<4;n++) {
            const unsigned base=n*CSR_EFFECT_BITS;
            put(e,base+CSR_VALID_OFFSET,1,1); put(e,base+CSR_ADDRESS_OFFSET,12,addresses[n]);
            put(e,base+CSR_OLD_VALUE_OFFSET,32,trap_csrs[n]); put(e,base+CSR_NEW_VALUE_OFFSET,32,values[n]);
            put(e,base+CSR_READ_MASK_OFFSET,32,reads[n]); put(e,base+CSR_WRITE_MASK_OFFSET,32,masks[n]);
            put(e,base+CSR_MASK_REASON_OFFSET,3,2);
        }
        return e;
    }
    void tick(Input i={}) {
        d.trap_ready_i=i.trap_ready;
        d.clk_i=0; d.rst_i=i.reset; d.enable_i=i.enable; d.flush_i=i.flush; d.flush_pc_i=i.target;
        d.drained_i=i.drain; d.resolve_grant_i=i.grant; d.execution_ready_i=i.execute;
        d.completion_enable_i=i.complete; d.retire_ready_i=i.retire; d.request_ready_i=i.request_ready;
        const bool response=(pending && delay==0) || i.inject_bad;
        d.response_valid_i=response;
        for (unsigned n=0; n<(RESPONSE_BITS+31)/32; ++n) d.response_i[n]=0;
        put(d.response_i,RESPONSE_TRANSACTION_ID_OFFSET,4,pending_id);
        const unsigned status=i.inject_bad ? 2 : pending_address==error_address ? 1 : 0;
        put(d.response_i,RESPONSE_STATUS_OFFSET,2,status);
        if (status==0 && pending) for (unsigned n=0; n<8; ++n)
            put(d.response_i,RESPONSE_LINE_READ_DATA_OFFSET+32*n,32,memory[pending_address/4+n]);
        d.eval();
        Reply accepted_csr; bool csr_accept=false;
        unsigned ordinary=0;
        if (i.reset) {
            require(!d.request_valid_o && !d.response_ready_o && !d.dispatch_o && !d.retire_accept_o && !d.redirect_o,"reset outputs");
            pending=held=fatal=false; pc=0; order=0; expected_id=0; known.fill(false); known[0]=true; regs[0]=0;
            trap_csrs={0x1800,0,0,0}; csr_state.reset_state(); system_prepared=false;
            coverage["reset"]++;
        } else {
            require(bool(d.fatal_o)==fatal,"fatal state");
            if (system_service && (i.flush || fatal))
                require(!d.system_prepare_o && !d.system_busy_o,"canceled system response still visible");
            if (i.flush || fatal) system_prepared=false;
            if (system_service && d.system_prepare_o) {
                require(!system_prepared && !i.flush && !fatal && pc<65536 && system_instruction(memory[pc/4]),"head-only system preparation");
                require(!d.retire_accept_o,"preparation overlaps older retirement");
                system_expected=expected_system(); system_prepared=true;
                coverage["system_prepare"]++;
            }
            if (i.flush) {
                require(!d.dispatch_o && !d.retire_accept_o,"flush atomicity");
                if (!fatal) { pc=i.target; coverage["external_flush"]++; }
            }
            if (system_service && system_prepared && !i.flush) {
                require(!d.dispatch_o,"younger dispatch past system barrier");
                if (system_expected.op==31) require(!d.redirect_o,"WFI redirected");
            }
            if (d.redirect_o) {
                const bool mret=system_service && system_prepared && system_expected.op==30 && d.retire_accept_o==1;
                if (system_service && system_prepared && system_expected.op==30 && !i.flush)
                    require(mret,"MRET redirect before retirement");
                require(!d.dispatch_o && (!d.retire_accept_o || mret),"redirect atomicity");
                require(i.flush || d.trap_accept_o || mret || i.grant,"redirect without grant");
                if (mret) { require(d.redirect_pc_o==system_expected.next_pc,"MRET target"); coverage["mret_redirect"]++; }
                else if (!i.flush && !d.trap_accept_o) coverage["branch_redirect"]++;
                if (pending) coverage["redirect_pending"]++;
                if (held) coverage["redirect_request_stall"]++;
            }
            require(d.dispatch_o!=2 && !(d.dispatch_o & ~d.fetch_valid_o),"dispatch prefix");
            if (fault_support && d.dispatch_o && (d.fetch_fault_o || decoded_fault(uint32_t(d.fetch_instruction_o))))
                require(d.dispatch_o==1,"lane-zero fault did not end prefix");
            if (d.dispatch_o==3) coverage["dual_dispatch"]++;
            if (d.fetch_valid_o && !d.dispatch_o && !d.fetch_fault_o && !d.unsupported_o) coverage["dispatch_stall"]++;
            if (d.occupancy_o==32) coverage["rob_full"]++;
            if (d.producer_busy_o && i.complete!=3) coverage["completion_stall"]++;
            if (d.retire_valid_o && i.retire!=3) coverage["retire_stall"]++;
            if (d.fetch_fault_o) {
                require(fault_support || !d.dispatch_o,"fetch fault dispatched");
                if (d.dispatch_o) coverage["fetch_fault_dispatch"]++;
                const uint32_t address=uint32_t(d.fetch_pc_o);
                const unsigned cause=address%4 ? 0 : 1;
                require(d.fetch_fault_cause_o==cause && d.fetch_instruction_o==0,"frontend fault metadata");
                coverage[address%4 ? "frontend_alignment" : address>=65536 ? "frontend_pma" : "frontend_bus_fault"]++;
            } else for (unsigned lane=0; lane<2; ++lane) if (d.fetch_valid_o & (1U<<lane)) {
                uint32_t address=uint32_t(d.fetch_pc_o>>(32*lane));
                require(address<65536 && address%4==0,"fetched executable PC");
                require(uint32_t(d.fetch_instruction_o>>(32*lane))==memory[address/4],"fetched word");
                if (system_service && memory[address/4]==0x10500073)
                    coverage[lane ? "wfi_seen_lane1":"wfi_seen_lane0"]++;
                bool unsupported=operation(memory[address/4])<0 && !(fault_support && decoded_fault(memory[address/4]))
                    && !(system_service && system_instruction(memory[address/4]));
                if (fault_support && illegal(memory[address/4]) && (d.dispatch_o&(1U<<lane)))
                    coverage[lane ? "illegal_lane1" : "illegal_lane0"]++;
                if (fault_support && (d.dispatch_o&(1U<<lane))) {
                    if (memory[address/4]==0x73) coverage[lane ? "ecall_lane1":"ecall_lane0"]++;
                    if (memory[address/4]==0x100073) coverage[lane ? "ebreak_lane1":"ebreak_lane0"]++;
                }
                require(bool(d.unsupported_o&(1U<<lane))==unsupported,"unsupported lane indication");
                if (unsupported) coverage["unsupported"]++;
            }
            if (fatal) require(!d.request_valid_o && !d.response_ready_o && !d.dispatch_o && !d.retire_accept_o && !d.redirect_o,"fatal inhibits progress");
            else {
                if (held) {
                    require(d.request_valid_o,"request withdrawn");
                    for (unsigned n=0;n<REQUEST_BITS;++n) require(bit(d.request_o,n)==bit(held_request,n),"request changed under stall");
                }
                if (d.request_valid_o) {
                    require(!pending,"overlapping instruction requests");
                    const unsigned id=get32(d.request_o,REQUEST_TRANSACTION_ID_OFFSET,4);
                    uint32_t address=get32(d.request_o,REQUEST_ADDRESS_OFFSET,32);
                    require(id==expected_id && address%32==0 && address<65536,"request address/identity");
                    for (unsigned n=0;n<REQUEST_BITS;++n)
                        if (!(n>=REQUEST_TRANSACTION_ID_OFFSET && n<REQUEST_TRANSACTION_ID_OFFSET+4)
                            && !(n>=REQUEST_ADDRESS_OFFSET && n<REQUEST_ADDRESS_OFFSET+32))
                            require(bit(d.request_o,n)==0,"read request reserved fields");
                    if (i.request_ready) {
                        pending=true; held=false; pending_id=id; pending_address=address; delay=i.latency; ++requests;
                    } else {
                        held=true; coverage["request_stall"]++;
                        for (unsigned n=0;n<held_request.size();++n) held_request[n]=d.request_o[n];
                    }
                } else if (pending && delay) --delay;
                if (response && d.response_ready_o) { pending=false; expected_id=(expected_id+1)%16; }
                if (i.inject_bad) { fatal=true; coverage["fatal_injected"]++; }
            }
            require(d.retire_accept_o!=2,"retirement prefix");
            if (system_service && system_prepared && d.retire_valid_o && !system_expected.fault) {
                require(d.retire_valid_o==1,"system retirement must be solo");
                compare(d.retire_event_o,0,system_expected.event);
                if (!d.retire_accept_o) coverage["system_held"]++;
            }
            for (unsigned lane=0;lane<2;++lane) if (d.retire_accept_o&(1U<<lane)) {
                Expected e=expected(); require(!e.fault,"fault retired normally");
                compare(d.retire_event_o,lane*EVENT_BITS,e.event);
                if (system_service && e.op>=29) {
                    require(d.retire_accept_o==1 && system_prepared,"serial acceptance");
                    if (e.op==30) require(d.redirect_o && d.redirect_pc_o==e.next_pc,"missing MRET redirect");
                    accepted_csr=system_reply; csr_accept=true; system_prepared=false;
                    coverage[e.op==30 ? "mret_retired":e.op==31 ? "wfi_retired":"csr_retired"]++;
                } else ordinary++;
                if (e.rd) { regs[e.rd]=e.result; known[e.rd]=true; }
                ++order; ++retired; pc=e.next_pc;
                coverage["op_"+std::to_string(e.op)]++;
                if (e.op>=21 && e.op<=28) coverage[e.taken ? "taken" : "not_taken"]++;
            }
            if (d.retire_accept_o==3) coverage["dual_retire"]++;
            if (d.backend_fault_o && !fatal) {
                auto e=expected(); require(e.fault,"unexpected head fault"); compare(d.backend_fault_event_o,0,e.event);
                coverage["backend_fault"]++;
            }
            require(bool(d.trap_accept_o)==(bool(d.trap_valid_o) && i.trap_ready),"trap handshake");
            if (d.trap_valid_o) {
                require(trap_service && !i.flush && !d.fatal_o && d.backend_fault_o,"unexpected trap offer");
                require(!d.retire_accept_o,"trap overlaps retirement");
                const auto e=finalized_fault(); compare(d.trap_event_o,0,e);
                if (d.trap_accept_o) {
                    const uint32_t target=get32(e,PC_AFTER_OFFSET,32);
                    require(d.redirect_o && d.redirect_pc_o==target && !d.dispatch_o,"trap redirect atomicity");
                    if (system_service) {
                        Command command; command.trap=true; command.pc=pc;
                        command.cause=get32(e,TRAP_CAUSE_OFFSET,32); command.value=get32(e,TRAP_VALUE_OFFSET,32);
                        accepted_csr=csr_state.propose(command); csr_accept=true; system_prepared=false;
                    }
                    for (unsigned n=0;n<4;n++) trap_csrs[n]=get32(e,n*CSR_EFFECT_BITS+CSR_NEW_VALUE_OFFSET,32);
                    ++order; ++traps; pc=target; coverage["trap_accept"]++;
                } else coverage["trap_stall"]++;
            } else for (unsigned n=0;n<EVENT_BITS;n++) require(!bit(d.trap_event_o,n),"invalid trap payload");
            if (trap_service && d.backend_fault_o && !i.flush && !d.trap_accept_o)
                require(!d.redirect_o,"younger redirect bypassed head fault");
            if (i.drain) coverage["drain"]++;
        }
        if (!i.reset && system_service) csr_state.advance(false,ordinary,csr_accept ? &accepted_csr:nullptr);
        d.clk_i=1; d.eval(); ++cycles;
    }
    void reset() { Input i; i.reset=true; tick(i); }
    void run_to(uint32_t stop,std::mt19937& rng,bool fill=false) {
        for (unsigned n=0;n<20000;++n) {
            Input i; i.request_ready=rng()%3!=0; i.latency=rng()%5;
            i.execute=rng()%4; i.complete=rng()%4; i.retire=rng()%4; i.grant=rng()%4!=0;
            if (fill && n<50) i.execute=0;
            if (fill && n<140) i.retire=0;
            tick(i);
            if (pc==stop && d.occupancy_o==0 && d.unsupported_o && !d.fetch_busy_o) return;
            require(!fatal && !d.backend_fault_o,"unexpected stopped execution");
        }
        require(false,"program progress watchdog");
    }
};
void program(Bench& b,std::mt19937& rng) {
    b.memory.fill(0); unsigned cursor=0;
    auto emit=[&](uint32_t insn){ b.memory[cursor++]=insn; };
    for (unsigned r=1;r<32;++r) emit(instruction(2,r,0,0,r*7-70));
    for (unsigned op=0;op<21;++op) emit(instruction(op,10+op%10,4,5,rng()));
    for (unsigned op=21;op<27;++op) {
        emit(instruction(op,0,0,0,8)); emit(instruction(2,30,30,0,1));
        emit(instruction(op,0,1,0,4));
    }
    for (unsigned n=0;n<300;++n) {
        unsigned op=rng()%21;
        emit(instruction(op,1+rng()%25,rng()%26,rng()%26,rng()));
        if (n%13==0) { emit(instruction(27,26,0,0,8)); emit(instruction(2,30,30,0,17)); }
    }
    unsigned target=(cursor+4)*4;
    emit(instruction(2,27,0,0,target|1)); emit(instruction(28,27,27,0));
    emit(instruction(2,30,30,0,45)); emit(instruction(2,30,30,0,71));
    emit(instruction(2,28,0,0,20));
    emit(instruction(2,29,29,0,1)); emit(instruction(2,28,28,0,-1)); emit(instruction(22,0,28,0,-8));
    emit(instruction(27,0,0,0,0x1000-cursor*4));
}
