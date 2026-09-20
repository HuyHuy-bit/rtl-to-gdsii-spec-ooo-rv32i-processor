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
template<class T> void put(T& data, unsigned offset, unsigned width, uint64_t value) {
    for (unsigned b = 0; b < width; ++b) {
        unsigned p = offset+b;
        data[p/32] = (data[p/32] & ~(1U << (p%32))) | (unsigned((value >> b)&1) << (p%32));
    }
}
template<class T> unsigned bit(const T& data, unsigned offset) { return (data[offset/32] >> (offset%32))&1; }
int operation(uint32_t insn) {
    unsigned opcode = insn&127, f = (insn >> 12)&7, upper = insn >> 25;
    if (opcode == 0x37) return 0;
    if (opcode == 0x17) return 1;
    if (opcode == 0x13) {
        const int ops[8] = {2, 3, 4, 5, 6, 7, 9, 10};
        if (f == 1 && upper != 0) return -1;
        if (f == 5) return upper == 0 ? 7 : upper == 32 ? 8 : -1;
        return ops[f];
    }
    if (opcode == 0x33) {
        const int ops[8] = {11, 13, 14, 15, 16, 17, 19, 20};
        if (upper == 0) return ops[f];
        if (upper == 32) return f == 0 ? 12 : f == 5 ? 18 : -1;
    }
    if (opcode == 0x63) {
        const int branch[8] = {21,22,-1,-1,23,24,25,26};
        return branch[f];
    }
    if (opcode == 0x6f) return 27;
    if (opcode == 0x67 && f == 0) return 28;
    return -1;
}
uint32_t instruction(unsigned op, unsigned rd, unsigned a, unsigned b, uint32_t imm = 0) {
    if (op >= 21 && op <= 26) {
        const unsigned function[6] = {0,1,4,5,6,7};
        return ((imm >> 12)&1U) << 31 | ((imm >> 5)&63U) << 25 | b << 20 | a << 15
            | function[op-21] << 12 | ((imm >> 1)&15U) << 8 | ((imm >> 11)&1U) << 7 | 0x63;
    }
    if (op == 27) return ((imm >> 20)&1U) << 31 | ((imm >> 1)&1023U) << 21
        | ((imm >> 11)&1U) << 20 | ((imm >> 12)&255U) << 12 | rd << 7 | 0x6f;
    if (op == 28) return (imm&4095U) << 20 | a << 15 | rd << 7 | 0x67;
    if (op < 2) return (imm&0xfffff000U) | (rd << 7) | (op == 0 ? 0x37U : 0x17U);
    const unsigned function[21] = {0,0,0,1,2,3,4,5,5,6,7,0,0,1,2,3,4,5,5,6,7};
    const unsigned alternate = op == 8 || op == 12 || op == 18 ? 32 : 0;
    if (op <= 10) {
        if (op == 3 || op == 7 || op == 8) imm = (alternate << 5) | (imm&31);
        return ((imm&4095) << 20) | (a << 15) | (function[op] << 12) | (rd << 7) | 0x13;
    }
    return (alternate << 25) | (b << 20) | (a << 15) | (function[op] << 12) | (rd << 7) | 0x33;
}
int64_t signed_value(uint32_t v) { return v&0x80000000U ? int64_t(v)-INT64_C(4294967296) : int64_t(v); }
uint32_t arithmetic_right(uint32_t a, unsigned shift) {
    shift &= 31;
    uint32_t result = a >> shift;
    if (shift && (a&0x80000000U)) result |= UINT32_MAX << (32-shift);
    return result;
}
uint32_t evaluate(unsigned op, uint32_t insn, uint32_t pc, uint32_t a, uint32_t b) {
    uint32_t immediate = insn >> 20;
    if (immediate&2048) immediate |= 0xfffff000U;
    if (op >= 2 && op <= 10) b = immediate;
    switch (op) {
    case 0: return insn&0xfffff000U;
    case 1: return pc+(insn&0xfffff000U);
    case 2: case 11: return a+b;
    case 12: return a-b;
    case 3: case 13: return a << (b&31);
    case 4: case 14: return signed_value(a) < signed_value(b);
    case 5: case 15: return a < b;
    case 6: case 16: return a^b;
    case 7: case 17: return a >> (b&31);
    case 8: case 18: return arithmetic_right(a, b);
    case 9: case 19: return a|b;
    case 10: case 20: return a&b;
    case 21: case 22: case 23: case 24: case 25: case 26: return 0;
    case 27: case 28: return pc+4U;
    default: throw std::runtime_error("unsupported reference operation");
    }
}
uint32_t extend(uint32_t value, unsigned bits) {
    return value & (1U << (bits-1)) ? value | (UINT32_MAX << bits) : value;
}
template<class T> uint32_t get(const T& data, unsigned offset, unsigned width) {
    uint32_t value=0;
    for (unsigned n=0; n<width; ++n) value|=bit(data,offset+n)<<n;
    return value;
}
struct Input {
    bool reset=false, enable=true, flush=false, drain=false, grant=true, request_ready=true;
    uint32_t target=0;
    unsigned execute=3, complete=3, retire=3, latency=1;
    bool inject_bad=false;
};
struct Expected { Event event{}; unsigned op, rd; uint32_t result, next_pc; bool fault, taken=false; };
struct Bench {
    Vfetch_execution_core d;
    std::array<uint32_t,16384> memory{};
    std::array<uint32_t,32> regs{};
    std::array<bool,32> known{};
    std::map<std::string,unsigned> coverage;
    unsigned cycles=0, retired=0, requests=0, expected_id=0, pending_id=0, delay=0;
    uint32_t pc=0, pending_address=0, error_address=UINT32_MAX;
    uint64_t order=0;
    bool pending=false, held=false, fatal=false;
    std::array<uint32_t,(REQUEST_BITS+31)/32> held_request{};
    void require(bool good,const std::string& why) const {
        if (!good) throw std::runtime_error("fetch execution core mismatch cycle="+std::to_string(cycles)+": "+why);
    }
    Expected expected() {
        require(pc<65536 && pc%4==0,"reference executable PC");
        const uint32_t insn=memory[pc/4];
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
    void tick(Input i={}) {
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
        if (i.reset) {
            require(!d.request_valid_o && !d.response_ready_o && !d.dispatch_o && !d.retire_accept_o && !d.redirect_o,"reset outputs");
            pending=held=fatal=false; pc=0; order=0; expected_id=0; known.fill(false); known[0]=true; regs[0]=0;
            coverage["reset"]++;
        } else {
            require(bool(d.fatal_o)==fatal,"fatal state");
            if (i.flush) {
                require(!d.dispatch_o && !d.retire_accept_o,"flush atomicity");
                if (!fatal) { pc=i.target; coverage["external_flush"]++; }
            }
            if (d.redirect_o) {
                require(!d.dispatch_o && !d.retire_accept_o,"redirect atomicity");
                require(i.flush || i.grant,"redirect without grant");
                if (!i.flush) coverage["branch_redirect"]++;
                if (pending) coverage["redirect_pending"]++;
                if (held) coverage["redirect_request_stall"]++;
            }
            require(d.dispatch_o!=2 && !(d.dispatch_o & ~d.fetch_valid_o),"dispatch prefix");
            if (d.dispatch_o==3) coverage["dual_dispatch"]++;
            if (d.fetch_valid_o && !d.dispatch_o && !d.fetch_fault_o && !d.unsupported_o) coverage["dispatch_stall"]++;
            if (d.occupancy_o==32) coverage["rob_full"]++;
            if (d.producer_busy_o && i.complete!=3) coverage["completion_stall"]++;
            if (d.retire_valid_o && i.retire!=3) coverage["retire_stall"]++;
            if (d.fetch_fault_o) {
                require(!d.dispatch_o,"fetch fault dispatched");
                const uint32_t address=uint32_t(d.fetch_pc_o);
                const unsigned cause=address%4 ? 0 : 1;
                require(d.fetch_fault_cause_o==cause && d.fetch_instruction_o==0,"frontend fault metadata");
                coverage[address%4 ? "frontend_alignment" : address>=65536 ? "frontend_pma" : "frontend_bus_fault"]++;
            } else for (unsigned lane=0; lane<2; ++lane) if (d.fetch_valid_o & (1U<<lane)) {
                uint32_t address=uint32_t(d.fetch_pc_o>>(32*lane));
                require(address<65536 && address%4==0,"fetched executable PC");
                require(uint32_t(d.fetch_instruction_o>>(32*lane))==memory[address/4],"fetched word");
                bool unsupported=operation(memory[address/4])<0;
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
                    const unsigned id=get(d.request_o,REQUEST_TRANSACTION_ID_OFFSET,4);
                    uint32_t address=get(d.request_o,REQUEST_ADDRESS_OFFSET,32);
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
            for (unsigned lane=0;lane<2;++lane) if (d.retire_accept_o&(1U<<lane)) {
                Expected e=expected(); require(!e.fault,"fault retired normally");
                compare(d.retire_event_o,lane*EVENT_BITS,e.event);
                if (e.rd) { regs[e.rd]=e.result; known[e.rd]=true; }
                ++order; ++retired; pc=e.next_pc;
                coverage["op_"+std::to_string(e.op)]++;
                if (e.op>=21) coverage[e.taken ? "taken" : "not_taken"]++;
            }
            if (d.retire_accept_o==3) coverage["dual_retire"]++;
            if (d.backend_fault_o && !fatal) {
                auto e=expected(); require(e.fault,"unexpected head fault"); compare(d.backend_fault_event_o,0,e.event);
                coverage["backend_fault"]++;
            }
            if (i.drain) coverage["drain"]++;
        }
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
static void program(Bench& b,std::mt19937& rng) {
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
int main(int argc,char** argv) {
    Verilated::commandArgs(argc,argv);
    try {
        Bench b;
        if (argc>1 && std::string(argv[1])=="negative") {
            b.reset(); b.d.rst_i=0; b.d.enable_i=1; b.d.drained_i=1;
            b.d.clk_i=0; b.d.eval(); b.d.clk_i=1; b.d.eval();
            throw std::runtime_error("missing drain assertion");
        }
        unsigned seed=argc>1 ? std::stoul(argv[1]) : 1;
        std::mt19937 rng(seed);
        program(b,rng); b.reset(); b.run_to(0x1000,rng,true);
        for (unsigned iteration=0;iteration<8;++iteration) { program(b,rng); b.reset(); b.run_to(0x1000,rng); }
        // A sequential not-taken stream must not redirect, including lane-one branches.
        b.memory.fill(0); b.memory[0]=instruction(2,1,0,0,1);
        for (unsigned n=1;n<32;++n) b.memory[n]=instruction(21,0,1,0,8);
        b.reset(); unsigned redirects=b.coverage["branch_redirect"]; b.run_to(128,rng);
        b.require(b.coverage["branch_redirect"]==redirects,"not-taken prediction caused redirect");
        // Wrong-path instruction access failure must be discarded when the older jump resolves.
        b.memory.fill(instruction(2,0,0,0,0)); b.memory[0x1000/4]=0;
        b.memory[0]=instruction(27,0,0,0,0x1000); b.error_address=32; b.reset();
        for (unsigned n=0;n<60;++n) { Input i; i.grant=false; b.tick(i); }
        b.require(b.d.fetch_fault_o,"wrong-path fetch fault reached frontend");
        b.run_to(0x1000,rng); b.coverage["wrong_path_fault_recovered"]++; b.error_address=UINT32_MAX;
        // Restart flush preserves committed values and global order while killing speculative work.
        program(b,rng); b.reset();
        for (unsigned n=0;n<40;++n) b.tick();
        Input i; i.flush=true; i.target=0; b.tick(i); b.run_to(0x1000,rng);
        i={}; i.enable=false; i.flush=true; i.target=0x1000; b.tick(i);
        i.flush=false; i.drain=true; b.tick(i); b.tick();
        b.require(!b.d.identity_drain_o,"identity recycle stayed blocked");
        for (uint32_t target : {2U,0x10000000U}) {
            i={}; i.flush=true; i.target=target; b.tick(i);
            for (unsigned n=0;n<8;++n) b.tick();
            b.require(b.d.fetch_fault_o,"local fetch fault missing");
        }
        b.memory.fill(0); b.memory[0]=instruction(27,1,0,0,2); b.reset();
        for (unsigned n=0;n<40;++n) b.tick();
        b.require(b.d.backend_fault_o,"target fault missing");
        program(b,rng); b.reset();
        for (unsigned n=0;n<50;++n) { i={}; i.retire=0; b.tick(i); }
        i={}; i.retire=0; i.inject_bad=true; b.tick(i);
        for (unsigned n=0;n<8;++n) b.tick();
        b.reset(); b.run_to(0x1000,rng);
        std::cout<<"FETCH EXECUTION CORE PASS seed="<<seed<<" cycles="<<b.cycles<<" retired="<<b.retired<<" requests="<<b.requests;
        for (const auto& [key,value]:b.coverage) std::cout<<' '<<key<<'='<<value;
        std::cout<<'\n';
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
