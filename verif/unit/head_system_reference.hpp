#pragma once
#include "csr_reference_layout.hpp"
#include <array>
#include <cstdint>
#include <map>
using Effect = std::array<uint32_t, 7>;
struct Reply {
    bool legal=false, read=false, trap=false;
    uint32_t value=0, next=0;
    std::array<Effect, 4> effects{};
};
struct Command {
    bool trap=false;
    uint32_t instruction=0, source=0, pc=0, cause=0, value=0;
};
inline uint32_t csr(unsigned address,unsigned kind=2,unsigned source=0,unsigned dest=1) {
    return address<<20 | source<<15 | kind<<12 | dest<<7 | 0x73;
}
struct CsrModel {
    std::map<unsigned,uint32_t> state;
    CsrModel() { reset_state(); }
    void reset_state() { state.clear(); for (const auto& c : CSR_SPEC) state[c.address]=c.reset; }
    const Spec* spec(unsigned address) const {
        for (const auto& c : CSR_SPEC) if (c.address == address) return &c;
        return nullptr;
    }
    Effect effect(unsigned address, uint32_t data, uint32_t mask, unsigned reason) const {
        const auto& c=*spec(address);
        return {1, address, state.at(address), data, c.write_mask | c.fixed_mask, mask, reason};
    }
    Reply propose(const Command& in) const {
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
    void advance(bool reset,unsigned ordinary,const Reply* accepted) {
        if (reset) { reset_state(); return; }
        const unsigned inhibit=state.at(0x320);
        bool cycle_write=false, instret_write=false;
        if (accepted && accepted->legal) for (const auto& e:accepted->effects) if (e[0] && e[5]) {
            cycle_write |= e[1]==0xb00 || e[1]==0xb80;
            instret_write |= e[1]==0xb02 || e[1]==0xb82;
        }
        auto increment=[&](unsigned lo,unsigned hi,uint64_t amount) {
            uint64_t value=(uint64_t(state.at(hi))<<32)|state.at(lo); value+=amount;
            state[lo]=uint32_t(value); state[hi]=uint32_t(value>>32);
        };
        if (!(inhibit&1) && !cycle_write) increment(0xb00,0xb80,1);
        if (!(inhibit&4) && !instret_write)
            increment(0xb02,0xb82,accepted && accepted->legal && !accepted->trap ? 1:ordinary);
        if (accepted && accepted->legal) for (const auto& e:accepted->effects)
            if (e[0]) state[e[1]]=(state.at(e[1])&~e[5])|(e[3]&e[5]);
    }
};
