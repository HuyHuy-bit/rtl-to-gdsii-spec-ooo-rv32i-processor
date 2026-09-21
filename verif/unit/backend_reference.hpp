#pragma once
#include "Vbackend_two_wide.h"
#include "verilated.h"
#include "backend_event_layout.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using Event = std::array<uint32_t, (EVENT_BITS+31)/32>;
template<class T> void put(T& data, unsigned offset, unsigned width, uint64_t value) {
    for (unsigned b = 0; b < width; ++b) {
        const unsigned p = offset+b;
        data[p/32] = (data[p/32] & ~(1U << (p%32))) | (unsigned((value >> b)&1) << (p%32));
    }
}
template<class T> unsigned bit(const T& data, unsigned offset) { return (data[offset/32] >> (offset%32)) & 1; }
struct Entry {
    unsigned id, rd, destination, stale, pc;
    bool cfi, done = false, resolved = false, solo = false;
    Event event{};
};
struct Branch { unsigned id, slot; };
struct Completion { unsigned id = 0; bool offer = false, solo = false; Event event{}; };
struct Input {
    unsigned count = 0, cfi = 0, solo = 0, retire_ready = 3, resolve_id = 0;
    std::array<unsigned, 2> rd{}, rs1{}, rs2{};
    std::array<unsigned, 4> read{};
    std::array<Completion, 2> complete{};
    Completion serial;
    bool reset = false, flush = false, drain = false, resources = true;
    bool resolve = false, grant = true, mispredict = false, trap_ready = false;
};
class Check {
    Vbackend_two_wide dut;
    std::array<unsigned, 32> committed{};
    std::array<uint32_t, 64> payload{};
    std::array<bool, 64> known{};
    unsigned tail = 0, sequence = 0;
    uint64_t order = 0;
public:
    std::deque<Entry> queue;
    std::vector<Branch> branches;
    std::vector<unsigned> old_ids;
    std::array<unsigned, 32> uses{};
    std::map<std::string, unsigned> coverage;
    unsigned cycles = 0;
    Check() { for (unsigned i = 0; i < 32; ++i) committed[i] = i; known[0] = true; }
    void require(bool ok, const std::string& what) const {
        if (!ok) throw std::runtime_error("backend two wide mismatch cycle="+std::to_string(cycles)+" "+what);
    }
    auto map() const {
        auto m = committed;
        for (const auto& e : queue) if (e.rd) m[e.rd] = e.destination;
        return m;
    }
    uint64_t free() const {
        uint64_t f = ~UINT64_C(1);
        for (auto p : committed) f &= ~(UINT64_C(1) << p);
        for (const auto& e : queue) if (e.rd) f &= ~(UINT64_C(1) << e.destination);
        return f;
    }
    uint64_t ready() const {
        uint64_t r = 1;
        for (auto p : committed) r |= UINT64_C(1) << p;
        for (const auto& e : queue) if (e.rd && e.done && !bit(e.event, TRAP_OFFSET)) r |= UINT64_C(1) << e.destination;
        return r;
    }
    unsigned checkpoints() const {
        unsigned mask = 0; for (auto b : branches) mask |= 1U << b.slot; return mask;
    }
    unsigned locate(unsigned id) const {
        for (unsigned i = 0; i < queue.size(); ++i) if (queue[i].id == id) return i;
        return queue.size();
    }
    unsigned eligible() const {
        unsigned n = 0;
        for (const auto& e : queue) {
            if (n == 2 || !e.done || bit(e.event, TRAP_OFFSET) || (e.cfi && !e.resolved)) break;
            if (n && (queue.front().solo || e.solo)) break;
            ++n;
        }
        return n;
    }
    Completion result(unsigned index, std::mt19937& rng, bool trap = false) const {
        Completion c; const auto& e = queue.at(index); c.id = e.id; c.offer = true;
        for (auto& word : c.event) word = rng();
        put(c.event, TRAP_OFFSET, 1, trap); put(c.event, PC_BEFORE_OFFSET, 32, e.pc);
        put(c.event, RD_ADDR_OFFSET, 5, trap ? 0 : e.rd);
        put(c.event, RD_WRITE_MASK_OFFSET, 32, !trap && e.rd ? UINT32_MAX : 0);
        return c;
    }
    template<class T> static uint64_t get(const T& data, unsigned offset, unsigned width) {
        uint64_t value = 0; for (unsigned b = 0; b < width; ++b) value |= uint64_t(bit(data, offset+b)) << b;
        return value;
    }
    template<class T> void equal_event(const T& actual, unsigned lane, Event expected, bool trap) {
        put(expected, VALID_OFFSET, 1, 1); put(expected, ORDER_OFFSET, 64, order+lane); put(expected, RETIRED_OFFSET, 1, !trap);
        for (unsigned b = 0; b < EVENT_BITS; ++b)
            require(bit(actual, lane*EVENT_BITS+b) == bit(expected, b), "event bit="+std::to_string(b));
    }
    void run(const Input& in = {}) {
        ++cycles;
        const bool active = !in.reset && !in.flush;
        const bool tv = active && !queue.empty() && queue.front().done && bit(queue.front().event, TRAP_OFFSET);
        const bool trap = tv && in.trap_ready;
        const auto ri = locate(in.resolve_id);
        const bool resolve = active && !trap && in.resolve && in.grant && ri < queue.size() && queue[ri].cfi && !queue[ri].resolved;
        const bool recovery = resolve && in.mispredict;
        const unsigned cp = checkpoints(); unsigned released = 0;
        if (resolve) for (auto b : branches) if (b.id == in.resolve_id || (recovery && locate(b.id) > ri)) released |= 1U << b.slot;
        auto speculative = map(); auto available = free(); auto readiness = ready();
        const auto& sc=in.serial;
        const bool serial_ready=active && !trap && !recovery && !in.drain && !queue.empty()
            && queue.front().solo && !queue.front().done && !queue.front().cfi && sc.id==queue.front().id
            && bit(sc.event,VALID_OFFSET) && bit(sc.event,RETIRED_OFFSET) && !bit(sc.event,TRAP_OFFSET)
            && get(sc.event,PC_BEFORE_OFFSET,32)==queue.front().pc
            && get(sc.event,RD_ADDR_OFFSET,5)==queue.front().rd
            && get(sc.event,RD_WRITE_MASK_OFFSET,32)==(queue.front().rd ? UINT32_MAX:0)
            && (queue.front().rd || get(sc.event,RD_VALUE_OFFSET,32)==0);
        const bool serial=sc.offer && serial_ready;
        const unsigned rv = serial ? 1 : active && !trap && !recovery ? (1U << eligible())-1 : 0;
        unsigned retire = rv & in.retire_ready; if (!(retire&1)) retire = 0;
        unsigned complete = 0, wb = 0; uint64_t reclaim = 0;
        if (recovery) for (unsigned i = ri+1; i < queue.size(); ++i) if (queue[i].rd) reclaim |= UINT64_C(1) << queue[i].destination;
        for (unsigned lane = 0; lane < 2; ++lane) {
            const auto& c = in.complete[lane]; const auto i = locate(c.id);
            if (!serial && active && !trap && c.offer && i < queue.size() && !queue[i].done && (!recovery || i <= ri)
                && !(lane && in.complete[0].offer && c.id == in.complete[0].id)) {
                complete |= 1U << lane;
                if (queue[i].rd && !bit(c.event, TRAP_OFFSET)) {
                    wb |= 1U << lane; readiness |= UINT64_C(1) << queue[i].destination;
                }
            }
        }
        if (serial && (retire&1) && queue.front().rd) { wb=1; readiness |= UINT64_C(1)<<queue.front().destination; }
        auto wb_index=[&](unsigned lane) { return serial ? 0 : locate(in.complete[lane].id); };
        auto wb_event=[&](unsigned lane) -> const Event& { return serial ? sc.event : in.complete[lane].event; };
        unsigned count = in.count; if ((in.cfi&1) && count) count = 1;
        std::array<unsigned, 2> dest{}, stale{}, s1{}, s2{}; unsigned sr = 0;
        bool enough = true;
        for (unsigned lane = 0; lane < count; ++lane) {
            s1[lane] = speculative[in.rs1[lane]]; s2[lane] = speculative[in.rs2[lane]];
            sr |= unsigned((readiness >> s1[lane])&1) << (lane*2);
            sr |= unsigned((readiness >> s2[lane])&1) << (lane*2+1);
            if (lane && in.rd[0]) {
                if (in.rs1[1] == in.rd[0]) sr &= ~(1U << 2);
                if (in.rs2[1] == in.rd[0]) sr &= ~(1U << 3);
            }
            if (in.rd[lane]) {
                stale[lane] = speculative[in.rd[lane]];
                for (unsigned p = 1; p < 64; ++p) if ((available >> p)&1) { dest[lane] = p; break; }
                if (!dest[lane]) enough = false;
                available &= ~(UINT64_C(1) << dest[lane]); speculative[in.rd[lane]] = dest[lane];
            }
            enough &= uses[(tail+lane)%32] < 256;
        }
        unsigned selected = (1U << count)-1;
        const bool needs_cp = selected & in.cfi;
        enough &= queue.size()+count <= 32 && (!needs_cp || cp != 255);
        const unsigned alloc = !serial && active && !trap && !recovery && !in.drain && in.resources && enough ? selected : 0;
        unsigned cp_slot = 0; while (cp_slot < 8 && (cp & (1U << cp_slot))) ++cp_slot;
        const bool create = alloc && needs_cp;
        dut.serial_offer_i = sc.offer; dut.serial_id_i = sc.id;
        for (unsigned w=0;w<sc.event.size();w++) dut.serial_event_i[w]=sc.event[w];
        dut.clk_i = 0; dut.rst_i = in.reset; dut.flush_i = in.flush; dut.drained_i = in.drain;
        dut.resources_ready_i = in.resources; dut.valid_i = (1U << in.count)-1; dut.cfi_i = in.cfi; dut.solo_i = in.solo;
        dut.rs1_i = in.rs1[0] | (in.rs1[1] << 5); dut.rs2_i = in.rs2[0] | (in.rs2[1] << 5);
        dut.rd_i = in.rd[0] | (in.rd[1] << 5);
        dut.pc_i = uint64_t(0x80000000U+4*sequence) | (uint64_t(0x80000004U+4*sequence) << 32);
        dut.retire_ready_i = in.retire_ready; dut.trap_ready_i = in.trap_ready;
        dut.resolve_offer_i = in.resolve; dut.resolve_grant_i = in.grant; dut.resolve_id_i = in.resolve_id; dut.mispredict_i = in.mispredict;
        dut.complete_offer_i = dut.complete_solo_i = dut.complete_id_i = 0;
        for (unsigned w = 0; w < (2*EVENT_BITS+31)/32; ++w) dut.complete_event_i[w] = 0;
        for (unsigned lane = 0; lane < 2; ++lane) {
            const auto& c = in.complete[lane]; dut.complete_offer_i |= unsigned(c.offer) << lane;
            dut.complete_solo_i |= unsigned(c.solo) << lane; dut.complete_id_i |= c.id << (13*lane);
            for (unsigned b = 0; b < EVENT_BITS; ++b) put(dut.complete_event_i, lane*EVENT_BITS+b, 1, bit(c.event,b));
        }
        dut.read_address_i = 0;
        for (unsigned port = 0; port < 4; ++port) dut.read_address_i |= in.read[port] << (6*port);
        dut.eval();
        require(dut.serial_ready_o==serial_ready && dut.serial_accept_o==(serial && bool(retire&1)),"serial handshake");
        require(dut.occupancy_o == queue.size(), "ROB occupancy");
        require(dut.allocate_accept_o == alloc, "joint allocation");
        require(dut.complete_accept_o == complete && dut.wb_accept_o == wb, "joint completion");
        require(dut.retire_valid_o == rv && dut.retire_accept_o == retire, "joint retirement");
        require(dut.resolve_accept_o == resolve && dut.branch_recover_o == recovery, "joint resolution");
        require(dut.trap_valid_o == tv && dut.trap_accept_o == trap, "trap boundary");
        require(dut.head_valid_o == (active && !queue.empty()), "head valid");
        if (!queue.empty()) require(dut.head_id_o == queue.front().id && dut.head_pc_o == queue.front().pc, "head identity/PC");
        require(bool(dut.identity_drain_o) == std::any_of(uses.begin(),uses.end(),[](unsigned n){return n==256;}), "identity exhaustion");
        require(dut.checkpoint_accept_o == create && dut.checkpoint_released_o == released, "checkpoint transaction");
        if (create) require(dut.checkpoint_id_o == cp_slot, "checkpoint slot");
        if (!in.reset) {
            require(dut.checkpoint_valid_o == cp && dut.free_o == free() && dut.ready_o == ready(), "ownership sets");
            const auto expected_map = map();
            for (unsigned arch = 0; arch < 32; ++arch)
                require(get(dut.rat_o,arch*6,6) == expected_map[arch] && get(dut.committed_o,arch*6,6) == committed[arch], "maps");
        }
        require(dut.source_ready_o == (alloc ? sr : 0), "rename operand readiness");
        for (unsigned lane = 0; lane < 2; ++lane) {
            const bool accepted = alloc & (1U << lane);
            require(((dut.source1_o >> (6*lane))&63) == (accepted?s1[lane]:0)
                && ((dut.source2_o >> (6*lane))&63) == (accepted?s2[lane]:0)
                && ((dut.destination_o >> (6*lane))&63) == (accepted?dest[lane]:0)
                && ((dut.stale_o >> (6*lane))&63) == (accepted?stale[lane]:0), "rename tags");
            if (accepted) { unsigned slot = (tail+lane)%32; require(((dut.allocate_id_o >> (13*lane))&8191) == uses[slot]*32+slot, "new ROB identity"); }
            if (complete & (1U << lane)) require(((dut.wb_destination_o >> (6*lane))&63) == queue[locate(in.complete[lane].id)].destination, "owned WB destination");
            require(bit(dut.retire_event_o,lane*EVENT_BITS+VALID_OFFSET) == ((rv>>lane)&1), "event validity");
            if (rv & (1U << lane)) {
                const auto& e = queue[lane]; equal_event(dut.retire_event_o,lane,serial ? sc.event : e.event,false);
                require(((dut.retire_rd_o >> (5*lane))&31) == e.rd && ((dut.retire_destination_o >> (6*lane))&63) == e.destination
                    && ((dut.retire_stale_o >> (6*lane))&63) == e.stale,"commit metadata");
            }
        }
        if (tv) equal_event(dut.trap_event_o,0,queue.front().event,true);
        for (unsigned port = 0; port < 4; ++port) {
            unsigned p = in.read[port]; uint32_t value = payload[p]; bool defined = known[p];
            for (unsigned lane = 0; lane < 2; ++lane) if ((wb & (1U << lane)) && queue[wb_index(lane)].destination == p) {
                value = get(wb_event(lane),RD_VALUE_OFFSET,32); defined = true; ++coverage["prf_bypass"];
            }
            if (defined) require(get(dut.read_data_o,port*32,32) == value, "PRF payload");
            coverage["read_reclaim"] += ((reclaim & ready()) >> p)&1;
            const bool read_ready = active && !trap && ((readiness & ~reclaim) >> p & 1);
            require(((dut.read_ready_o >> port)&1) == read_ready,"PRF readiness");
        }
        coverage["serial_accept"] += serial && (retire&1);
        coverage["serial_stall"] += serial && !(retire&1);
        coverage["serial_rejected"] += sc.offer && !serial_ready;
        coverage["serial_no_destination"] += serial && (retire&1) && !queue.front().rd;
        coverage["serial_blocks_completion"] += serial && (in.complete[0].offer || in.complete[1].offer);
        coverage["serial_blocks_allocation"] += serial && in.count;
        coverage["dual_allocate"] += alloc==3; coverage["dual_complete"] += complete==3; coverage["dual_retire"] += retire==3;
        coverage["simultaneous"] += alloc && complete && retire; coverage["stalled_retire"] += rv && !retire;
        coverage["waw"] += alloc==3 && in.rd[0] && in.rd[0]==in.rd[1];
        coverage["raw"] += alloc==3 && in.rd[0] && in.rs1[1]==in.rd[0];
        coverage["rob_full"] += queue.size()==32; coverage["prf_full"] += free()==0;
        coverage["checkpoint_full"] += cp==255 && needs_cp && !alloc;
        coverage["checkpoint_reuse"] += create && resolve; coverage["recovery"] += recovery;
        coverage["nested_recovery"] += recovery && __builtin_popcount(released)>1;
        coverage["surviving_wb"] += recovery && wb; coverage["recovery_wrap"] += recovery && queue.front().id%32>in.resolve_id%32;
        coverage["trap"] += trap;
        coverage["solo_retire"] += (retire&1) && queue.front().solo;
        for (unsigned lane = 0; lane < 2; ++lane) if (complete & (1U << lane)) {
            const auto& e = queue[locate(in.complete[lane].id)];
            coverage["fault_no_write"] += e.rd && bit(in.complete[lane].event, TRAP_OFFSET);
            coverage["resultless"] += !e.rd && !bit(in.complete[lane].event, TRAP_OFFSET);
            coverage["late_solo"] += in.complete[lane].solo;
        }
        coverage["stale"] += active && in.complete[0].offer && locate(in.complete[0].id)==queue.size();
        coverage["stale_resolve"] += active && in.resolve && !resolve;
        coverage["duplicate"] += in.complete[0].offer && in.complete[1].offer && in.complete[0].id==in.complete[1].id;
        coverage["flush_live"] += in.flush && !queue.empty(); coverage["reset_live"] += in.reset && !queue.empty();
        coverage["drain"] += in.drain; coverage["generation_stall"] += uses[tail]==256;
        if (in.reset) {
            for (unsigned i=0;i<32;++i) committed[i]=i;
            queue.clear(); branches.clear(); uses.fill(0); old_ids.clear(); tail=sequence=0; order=0;
        } else {
            for (unsigned lane=0;lane<2;++lane) if (wb & (1U<<lane)) {
                const auto p=queue[wb_index(lane)].destination;
                payload[p]=get(wb_event(lane),RD_VALUE_OFFSET,32); known[p]=true;
            }
            order += (retire==3?2:retire)+unsigned(trap);
            if (in.flush || trap) {
                for (auto e:queue) old_ids.push_back(e.id);
                queue.clear(); branches.clear(); tail=0;
            } else {
                if (resolve) queue[ri].resolved=true;
                for (unsigned lane=0;lane<2;++lane) if (complete & (1U<<lane)) {
                    auto& e=queue[locate(in.complete[lane].id)]; e.done=true; e.event=in.complete[lane].event; e.solo|=in.complete[lane].solo;
                }
                branches.erase(std::remove_if(branches.begin(),branches.end(),[&](Branch b){return released & (1U<<b.slot);}),branches.end());
                if (recovery) {
                    tail=(queue[ri].id%32+1)%32;
                    while(queue.size()>ri+1) {old_ids.push_back(queue.back().id);queue.pop_back();}
                } else {
                    for (unsigned lane=0;lane<2;++lane) if (retire & (1U<<lane)) {
                        auto e=queue.front(); if(e.rd)committed[e.rd]=e.destination; old_ids.push_back(e.id); queue.pop_front();
                    }
                    for (unsigned lane=0;lane<2;++lane) if (alloc & (1U<<lane)) {
                        unsigned id=uses[tail]*32+tail;
                        queue.push_back({id,in.rd[lane],dest[lane],stale[lane],0x80000000U+4*sequence++,bool(in.cfi&(1U<<lane)),false,false,bool(in.solo&(1U<<lane))});
                        if(in.cfi&(1U<<lane)) branches.push_back({id,cp_slot});
                        ++uses[tail]; tail=(tail+1)%32;
                    }
                }
                if(in.drain) {uses.fill(0);old_ids.clear();}
            }
        }
        dut.clk_i=1;dut.eval();dut.clk_i=0;dut.eval();
    }
};
