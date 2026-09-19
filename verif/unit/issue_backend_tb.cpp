#include "Vissue_backend.h"
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
    unsigned id = 0, rd = 0, destination = 0, stale = 0, pc = 0, eligible = 3;
    std::array<unsigned, 2> source{0, 0};
    std::array<bool, 2> ready{false, false};
    uint64_t payload = 0;
    bool cfi = false, done = false, resolved = false, solo = false, queued = true, issued = false;
    Event event{};
};
struct Branch { unsigned id, slot; };
struct Completion { unsigned id = 0; bool offer = false, solo = false; Event event{}; };
struct Input {
    unsigned count = 0, cfi = 0, solo = 0, retire_ready = 3, resolve_id = 0, ports = 3;
    std::array<unsigned, 2> rd{}, rs1{}, rs2{}, eligible{3, 3};
    std::array<Completion, 2> complete{};
    bool reset = false, flush = false, drain = false, resources = true;
    bool resolve = false, grant = true, mispredict = false, trap_ready = false;
};
class Check {
    Vissue_backend dut;
    std::array<unsigned, 32> committed{};
    std::array<uint32_t, 64> payload{};
    std::array<bool, 64> known{};
    unsigned sequence = 0;
    uint64_t order = 0;
    bool wake_pending = false;
public:
    std::deque<Entry> queue;
    std::vector<Branch> branches;
    std::vector<unsigned> old_ids;
    std::array<unsigned, 32> uses{};
    std::map<std::string, unsigned> coverage;
    unsigned cycles = 0, tail = 0;
    Check() { for (unsigned i = 0; i < 32; ++i) committed[i] = i; known[0] = true; }
    void require(bool ok, const std::string& what) const {
        if (!ok) throw std::runtime_error("issue backend mismatch cycle="+std::to_string(cycles)+" "+what);
    }
    static uint64_t token(unsigned n) { return UINT64_C(0x9e3779b97f4a7c15)*(n+1); }
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
        return unsigned(queue.size());
    }
    unsigned resident() const {
        return unsigned(std::count_if(queue.begin(), queue.end(), [](const Entry& e) { return e.queued; }));
    }
    unsigned retirable() const {
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
        put(c.event, RD_VALUE_OFFSET, 32, rng() | 1U);
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
        require(!resolve || queue[ri].issued, "stimulus resolution before issue");
        const bool recovery = resolve && in.mispredict;
        const unsigned cp = checkpoints(); unsigned released = 0;
        if (resolve) for (auto b : branches) if (b.id == in.resolve_id || (recovery && locate(b.id) > ri)) released |= 1U << b.slot;
        auto speculative = map(); auto available = free(); auto readiness = ready();
        const unsigned rv = active && !trap && !recovery ? (1U << retirable())-1 : 0;
        unsigned retire = rv & in.retire_ready; if (!(retire&1)) retire = 0;
        unsigned complete = 0, wb = 0; uint64_t reclaim = 0;
        std::array<unsigned, 2> wb_tag{0, 0};
        if (recovery) for (unsigned i = ri+1; i < queue.size(); ++i) if (queue[i].rd) reclaim |= UINT64_C(1) << queue[i].destination;
        for (unsigned lane = 0; lane < 2; ++lane) {
            const auto& c = in.complete[lane]; const auto i = locate(c.id);
            if (active && !trap && c.offer && i < queue.size() && !queue[i].done && (!recovery || i <= ri)
                && !(lane && in.complete[0].offer && c.id == in.complete[0].id)) {
                complete |= 1U << lane;
                if (queue[i].rd && !bit(c.event, TRAP_OFFSET)) {
                    wb |= 1U << lane; wb_tag[lane] = queue[i].destination;
                    readiness |= UINT64_C(1) << queue[i].destination;
                }
            }
        }

        // Selection uses program order, never the queue's sparse slots or modular age comparison.
        const bool stop = !active || trap || recovery;
        const unsigned occupancy = resident();
        std::vector<std::array<bool, 2>> wake(queue.size());
        bool any_wake = false;
        for (unsigned n = 0; n < queue.size(); ++n) {
            wake[n] = queue[n].ready;
            for (unsigned operand = 0; operand < 2; ++operand) {
                wake[n][operand] = wake[n][operand] || queue[n].source[operand] == 0;
                for (unsigned lane = 0; lane < 2; ++lane)
                    if ((wb & (1U << lane)) && wb_tag[lane] == queue[n].source[operand]) wake[n][operand] = true;
            }
            any_wake |= queue[n].queued && wake[n] != queue[n].ready;
        }
        std::array<int, 2> pick{-1, -1};
        for (unsigned port = 0; port < 2; ++port)
            if (!stop && (in.ports & (1U << port)))
                for (unsigned n = 0; n < queue.size(); ++n)
                    if (queue[n].queued && wake[n][0] && wake[n][1] && (queue[n].eligible & (1U << port))
                        && (port == 0 || pick[0] != int(n))) { pick[port] = int(n); break; }

        unsigned count = in.count; if ((in.cfi&1) && count) count = 1;
        std::array<unsigned, 2> dest{}, stale{}, s1{}, s2{}; unsigned sr = 0;
        bool enough = occupancy+count <= 16;
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
        const unsigned selected = (1U << count)-1;
        const bool needs_cp = selected & in.cfi;
        enough &= queue.size()+count <= 32 && (!needs_cp || cp != 255);
        const unsigned alloc = active && !trap && !recovery && !in.drain && in.resources && enough ? selected : 0;
        for (unsigned lane = 0; lane < 2; ++lane)
            require(!(alloc & in.cfi & (1U << lane)) || in.eligible[lane] == 1, "stimulus branch port");
        unsigned cp_slot = 0; while (cp_slot < 8 && (cp & (1U << cp_slot))) ++cp_slot;
        const bool create = alloc && needs_cp;

        dut.clk_i = 0; dut.rst_i = in.reset; dut.flush_i = in.flush; dut.drained_i = in.drain;
        dut.resources_ready_i = in.resources; dut.valid_i = (1U << in.count)-1; dut.cfi_i = in.cfi; dut.solo_i = in.solo;
        dut.rs1_i = in.rs1[0] | (in.rs1[1] << 5); dut.rs2_i = in.rs2[0] | (in.rs2[1] << 5);
        dut.rd_i = in.rd[0] | (in.rd[1] << 5);
        dut.pc_i = uint64_t(0x80000000U+4*sequence) | (uint64_t(0x80000004U+4*sequence) << 32);
        dut.eligible_i = in.eligible[0] | (in.eligible[1] << 2);
        for (unsigned lane = 0; lane < 2; ++lane) {
            dut.payload_i[2*lane] = uint32_t(token(sequence+lane));
            dut.payload_i[2*lane+1] = uint32_t(token(sequence+lane) >> 32);
        }
        dut.retire_ready_i = in.retire_ready; dut.trap_ready_i = in.trap_ready; dut.port_ready_i = in.ports;
        dut.resolve_offer_i = in.resolve; dut.resolve_grant_i = in.grant; dut.resolve_id_i = in.resolve_id; dut.mispredict_i = in.mispredict;
        dut.complete_offer_i = dut.complete_solo_i = dut.complete_id_i = 0;
        for (unsigned w = 0; w < (2*EVENT_BITS+31)/32; ++w) dut.complete_event_i[w] = 0;
        for (unsigned lane = 0; lane < 2; ++lane) {
            const auto& c = in.complete[lane]; dut.complete_offer_i |= unsigned(c.offer) << lane;
            dut.complete_solo_i |= unsigned(c.solo) << lane; dut.complete_id_i |= c.id << (13*lane);
            for (unsigned b = 0; b < EVENT_BITS; ++b) put(dut.complete_event_i, lane*EVENT_BITS+b, 1, bit(c.event, b));
        }
        dut.eval();

        require(dut.occupancy_o == queue.size() && dut.issue_occupancy_o == occupancy, "occupancy");
        require(dut.allocate_accept_o == alloc, "joint allocation");
        require(dut.complete_accept_o == complete && dut.wb_accept_o == wb, "joint completion");
        require(dut.retire_valid_o == rv && dut.retire_accept_o == retire, "joint retirement");
        require(dut.resolve_accept_o == resolve && dut.branch_recover_o == recovery, "joint resolution");
        require(dut.trap_valid_o == tv && dut.trap_accept_o == trap, "trap boundary");
        require(dut.head_valid_o == (active && !queue.empty()), "head valid");
        if (!queue.empty()) require(dut.head_id_o == queue.front().id && dut.head_pc_o == queue.front().pc, "head identity/PC");
        require(dut.checkpoint_accept_o == create && dut.checkpoint_released_o == released, "checkpoint transaction");
        if (create) require(dut.checkpoint_id_o == cp_slot, "checkpoint slot");
        if (!in.reset) {
            require(dut.checkpoint_valid_o == cp && dut.free_o == free() && dut.ready_o == ready(), "ownership sets");
            const auto expected_map = map();
            for (unsigned arch = 0; arch < 32; ++arch)
                require(get(dut.rat_o, arch*6, 6) == expected_map[arch] && get(dut.committed_o, arch*6, 6) == committed[arch], "maps");
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
            require(bit(dut.retire_event_o, lane*EVENT_BITS+VALID_OFFSET) == ((rv >> lane)&1), "event validity");
            if (rv & (1U << lane)) {
                const auto& e = queue[lane]; equal_event(dut.retire_event_o, lane, e.event, false);
                require(((dut.retire_rd_o >> (5*lane))&31) == e.rd && ((dut.retire_destination_o >> (6*lane))&63) == e.destination
                    && ((dut.retire_stale_o >> (6*lane))&63) == e.stale, "commit metadata");
            }
        }
        if (tv) equal_event(dut.trap_event_o, 0, queue.front().event, true);

        for (unsigned port = 0; port < 2; ++port) {
            const bool issued = pick[port] >= 0;
            const Entry expected = issued ? queue[unsigned(pick[port])] : Entry{};
            require(bool(dut.issue_o & (1U << port)) == issued, "launch port="+std::to_string(port));
            require(((dut.issue_id_o >> (13*port))&8191) == (issued ? expected.id : 0), "oldest identity");
            require(((dut.issue_destination_o >> (6*port))&63) == (issued ? expected.destination : 0), "issue destination");
            require(((dut.issue_source_o >> (12*port))&4095)
                == (issued ? expected.source[0] | (expected.source[1] << 6) : 0), "issue read ports");
            const uint64_t launched = dut.issue_payload_o[2*port] | (uint64_t(dut.issue_payload_o[2*port+1]) << 32);
            require(launched == (issued ? expected.payload : 0), "issue payload");
            for (unsigned operand = 0; operand < 2; ++operand) {
                const unsigned p = issued ? expected.source[operand] : 0, index = port*2+operand;
                uint32_t value = payload[p]; bool defined = known[p];
                for (unsigned lane = 0; lane < 2; ++lane) if ((wb & (1U << lane)) && wb_tag[lane] == p) {
                    value = unsigned(get(in.complete[lane].event, RD_VALUE_OFFSET, 32)); defined = true;
                    if (issued) ++coverage["prf_bypass"];
                }
                if (defined) require(get(dut.read_data_o, index*32, 32) == value, "PRF payload");
                require(((dut.read_ready_o >> index)&1) == (active && !trap && ((readiness & ~reclaim) >> p & 1)), "PRF readiness");
            }
        }

        const unsigned launches = unsigned(pick[0] >= 0)+unsigned(pick[1] >= 0);
        coverage["dual_allocate"] += alloc == 3; coverage["dual_complete"] += complete == 3; coverage["dual_retire"] += retire == 3;
        coverage["dual_issue"] += launches == 2; coverage["dispatch_issue"] += alloc && launches;
        coverage["simultaneous"] += alloc && complete && retire && launches;
        coverage["queue_full"] += occupancy == 16 && !stop;
        coverage["queue_stall"] += !alloc && count && in.resources && occupancy+count > 16;
        coverage["rob_full"] += queue.size() == 32; coverage["prf_full"] += free() == 0;
        coverage["backpressure"] += occupancy && in.ports == 0 && !stop;
        coverage["stalled_wakeup"] += any_wake && in.ports == 0 && !stop;
        coverage["retained_wakeup"] += wake_pending && launches && !wb;
        coverage["unaccepted_broadcast"] += !stop && in.complete[0].offer && !(complete&1);
        for (unsigned port = 0; port < 2; ++port) if (pick[port] >= 0) {
            const auto& e = queue[unsigned(pick[port])];
            coverage["wakeup_issue"] += wb && wake[unsigned(pick[port])] != e.ready;
            coverage["zero_source"] += e.source[0] == 0 || e.source[1] == 0;
            coverage["port0_only"] += e.eligible == 1; coverage["port1_only"] += e.eligible == 2;
            coverage["branch_issue"] += e.cfi;
            coverage["wrapped_window"] += e.id%32 < queue.front().id%32;
        }
        coverage["recovery"] += recovery; coverage["trap"] += trap;
        if (recovery) for (unsigned n = 0; n < queue.size(); ++n) if (queue[n].queued)
            ++coverage[n > ri ? "recovery_killed" : "recovery_survivor"];
        coverage["trap_live"] += trap && occupancy; coverage["flush_live"] += in.flush && occupancy;
        coverage["reset_live"] += in.reset && occupancy; coverage["drain"] += in.drain;
        coverage["simultaneous_checkpoint_create_resolve"] += create && resolve;
        coverage["resolution_after_issue"] += resolve;
        coverage["nested_recovery"] += recovery && __builtin_popcount(released) > 1;
        coverage["surviving_wb"] += recovery && wb;
        for (unsigned lane = 0; lane < 2; ++lane) if (complete & (1U << lane)) {
            const auto& e = queue[locate(in.complete[lane].id)];
            coverage["fault_no_write"] += e.rd && bit(in.complete[lane].event, TRAP_OFFSET);
            coverage["resultless"] += !e.rd && !bit(in.complete[lane].event, TRAP_OFFSET);
        }
        wake_pending = any_wake && in.ports == 0 && !stop;

        if (in.reset) {
            for (unsigned i = 0; i < 32; ++i) committed[i] = i;
            queue.clear(); branches.clear(); uses.fill(0); old_ids.clear(); tail = sequence = 0; order = 0;
        } else {
            for (unsigned lane = 0; lane < 2; ++lane) if (wb & (1U << lane)) {
                payload[wb_tag[lane]] = unsigned(get(in.complete[lane].event, RD_VALUE_OFFSET, 32)); known[wb_tag[lane]] = true;
            }
            order += (retire == 3 ? 2 : retire)+unsigned(trap);
            if (in.flush || trap) {
                for (auto e : queue) old_ids.push_back(e.id);
                queue.clear(); branches.clear(); tail = 0;
            } else {
                for (unsigned n = 0; n < queue.size(); ++n) if (queue[n].queued) queue[n].ready = wake[n];
                if (resolve) queue[ri].resolved = true;
                for (unsigned lane = 0; lane < 2; ++lane) if (complete & (1U << lane)) {
                    auto& e = queue[locate(in.complete[lane].id)]; e.done = true; e.event = in.complete[lane].event; e.solo |= in.complete[lane].solo;
                }
                branches.erase(std::remove_if(branches.begin(), branches.end(), [&](Branch b){return released & (1U << b.slot);}), branches.end());
                if (recovery) {
                    tail = (queue[ri].id%32+1)%32;
                    while (queue.size() > ri+1) { old_ids.push_back(queue.back().id); queue.pop_back(); }
                } else {
                    for (auto n : pick) if (n >= 0) { queue[unsigned(n)].queued = false; queue[unsigned(n)].issued = true; }
                    for (unsigned lane = 0; lane < 2; ++lane) if (retire & (1U << lane)) {
                        auto e = queue.front(); if (e.rd) committed[e.rd] = e.destination; old_ids.push_back(e.id); queue.pop_front();
                    }
                    for (unsigned lane = 0; lane < 2; ++lane) if (alloc & (1U << lane)) {
                        Entry e;
                        e.id = uses[tail]*32+tail; e.rd = in.rd[lane]; e.destination = dest[lane]; e.stale = stale[lane];
                        e.pc = 0x80000000U+4*sequence; e.eligible = in.eligible[lane];
                        e.source = {s1[lane], s2[lane]};
                        e.ready = {bool((sr >> (lane*2))&1), bool((sr >> (lane*2+1))&1)};
                        e.payload = token(sequence++); e.cfi = in.cfi & (1U << lane); e.solo = in.solo & (1U << lane);
                        queue.push_back(e);
                        if (in.cfi & (1U << lane)) branches.push_back({e.id, cp_slot});
                        ++uses[tail]; tail = (tail+1)%32;
                    }
                }
                if (in.drain) { uses.fill(0); old_ids.clear(); }
            }
        }
        dut.clk_i = 1; dut.eval(); dut.clk_i = 0; dut.eval();
    }
};
void drain(Check& c, std::mt19937& rng) {
    for (unsigned limit = 0; !c.queue.empty(); ++limit) {
        c.require(limit < 400, "drain progress");
        Input in; in.trap_ready = true;
        for (unsigned i = 0, lane = 0; i < c.queue.size() && lane < 2; ++i)
            if (c.queue[i].issued && !c.queue[i].done) { in.complete[lane] = c.result(i, rng); ++lane; }
        for (const auto& e : c.queue) if (e.issued && e.cfi && !e.resolved) { in.resolve = true; in.resolve_id = e.id; break; }
        c.run(in);
    }
}
void fill(Check& c, unsigned entries) {
    for (unsigned n = 0; n < entries; n += 2) { Input in; in.ports = 0; in.count = 2; c.run(in); }
}
void directed(Check& c, std::mt19937& rng) {
    Input in; in.reset = true; c.run(in);

    // Independent pair launches after dispatch, then feeds a dependent pair through the read ports.
    in = {}; in.count = 2; in.rd = {1, 2}; c.run(in);
    c.run();
    in = {}; in.complete = {c.result(0, rng), c.result(1, rng)};
    in.count = 1; in.rd[0] = 3; in.rs1[0] = 1; in.rs2[0] = 2; c.run(in);
    c.run(); c.run();
    drain(c, rng);

    // A stalled consumer wakes on the accepted writeback and reads it in the same cycle.
    in = {}; in.count = 1; in.rd[0] = 5; in.ports = 0; c.run(in);
    in = {}; in.count = 1; in.rd[0] = 6; in.rs1[0] = 5; in.rs2[0] = 5; in.ports = 0; c.run(in);
    in = {}; in.ports = 1; c.run(in);
    in = {}; in.complete[0] = c.result(0, rng); c.run(in);
    drain(c, rng);

    // The same wake latches while the ports are stalled and launches on the next free port.
    in = {}; in.count = 1; in.rd[0] = 7; in.ports = 0; c.run(in);
    in = {}; in.count = 1; in.rd[0] = 8; in.rs1[0] = 7; in.rs2[0] = 0; in.ports = 0; c.run(in);
    in = {}; in.ports = 1; c.run(in);
    in = {}; in.ports = 0; in.complete[0] = c.result(0, rng); c.run(in);
    in = {}; c.run(in);
    drain(c, rng);

    // A rejected offer broadcasts an owned tag that must not wake its consumer.
    in = {}; in.count = 1; in.rd[0] = 9; in.ports = 0; c.run(in);
    in = {}; in.count = 1; in.rd[0] = 10; in.rs1[0] = 9; in.ports = 0; c.run(in);
    in = {}; in.complete[0] = c.result(0, rng); in.complete[0].id = c.queue[0].id+32; c.run(in);
    c.run(); drain(c, rng);

    // Queue credits gate the whole renamed prefix; a lane-zero CFI shortens it to one.
    fill(c, 16);
    in = {}; in.ports = 0; in.count = 2; c.run(in);
    in = {}; in.ports = 1; c.run(in);
    in = {}; in.ports = 0; in.count = 2; c.run(in);
    in = {}; in.ports = 0; in.count = 2; in.cfi = 1; in.eligible[0] = 1; c.run(in);
    drain(c, rng);

    // Oldest-first selection follows the ROB head across a wrapped window.
    while (c.tail != 30) { in = {}; in.count = 1; c.run(in); drain(c, rng); }
    fill(c, 4);
    for (unsigned n = 0; n < 4; ++n) { in = {}; in.ports = 1; c.run(in); }
    drain(c, rng);

    // A branch issues ahead of older port-one residents before resolving and killing younger entries.
    in = {}; in.ports = 0; in.count = 2; in.eligible = {2, 2}; c.run(in);
    in = {}; in.ports = 0; in.count = 1; in.cfi = 1; in.eligible[0] = 1; c.run(in);
    in = {}; in.ports = 0; in.count = 2; c.run(in);
    in = {}; in.ports = 1; c.run(in);
    in = {}; in.ports = 0; in.resolve = true; in.mispredict = true; in.resolve_id = c.queue[2].id; c.run(in);
    for (unsigned n = 0; n < 3; ++n) { in = {}; in.ports = 1; c.run(in); }
    drain(c, rng);

    // An accepted trap flushes the queue with dependent residents still waiting.
    in = {}; in.count = 1; in.rd[0] = 11; c.run(in);
    in = {}; in.count = 2; in.rd = {12, 13}; in.rs1 = {11, 11}; in.rs2 = {11, 11}; c.run(in);
    in = {}; in.complete[0] = c.result(0, rng, true); c.run(in);
    in = {}; in.trap_ready = true; c.run(in);
    drain(c, rng);

    // Port eligibility splits a pair, then reset and flush clear live residents.
    in = {}; in.ports = 0; in.count = 2; in.eligible = {2, 1}; c.run(in);
    in = {}; c.run(in);
    drain(c, rng);
    in = {}; in.ports = 0; in.count = 2; c.run(in);
    in = {}; in.flush = true; c.run(in);
    in = {}; in.ports = 0; in.count = 2; c.run(in);
    in = {}; in.reset = true; c.run(in);
    in = {}; in.drain = true; c.run(in);
}
void random_run(Check& c, std::mt19937& rng, unsigned cycles) {
    for (unsigned step = 0; step < cycles; ++step) {
        Input in; in.count = rng()%3; in.retire_ready = rng()%4; in.resources = rng()%5 != 0;
        in.trap_ready = rng()%2; in.ports = rng()%4;
        for (unsigned lane = 0; lane < 2; ++lane) {
            in.rd[lane] = rng()%32; in.rs1[lane] = rng()%32; in.rs2[lane] = rng()%32;
            in.eligible[lane] = 1+rng()%3;
        }
        if (rng()%5 == 0) in.cfi = 1U << (rng()%2);
        for (unsigned lane = 0; lane < 2; ++lane) if (in.cfi & (1U << lane)) in.eligible[lane] = 1;
        if (rng()%11 == 0) in.solo = 1U << (rng()%2);
        std::vector<unsigned> running, branch;
        for (unsigned i = 0; i < c.queue.size(); ++i) {
            if (c.queue[i].issued && !c.queue[i].done) running.push_back(i);
            if (c.queue[i].issued && c.queue[i].cfi && !c.queue[i].resolved) branch.push_back(i);
        }
        std::shuffle(running.begin(), running.end(), rng);
        for (unsigned lane = 0; lane < 2 && lane < running.size(); ++lane)
            if (rng()%4) { in.complete[lane] = c.result(running[lane], rng, rng()%100 == 0); in.complete[lane].solo = rng()%16 == 0; }
        if (!branch.empty() && rng()%3 == 0) {
            in.resolve = true; in.resolve_id = c.queue[branch[rng()%branch.size()]].id;
            in.grant = rng()%4 != 0; in.mispredict = rng()%3 == 0;
        }
        if (!c.old_ids.empty() && rng()%9 == 0) { in.complete[0] = {}; in.complete[0].offer = true; in.complete[0].id = c.old_ids[rng()%c.old_ids.size()]; }
        if (rng()%300 == 0) in.flush = true;
        if (rng()%4001 == 4000) in.reset = true;
        c.run(in);
    }
}
int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    try {
        Check c;
        if (argc > 1 && std::string(argv[1]) == "negative") {
            const std::string name = argc > 2 ? argv[2] : "";
            Input in; in.reset = true; c.run(in);
            in = {}; in.ports = 0; in.count = 1;
            if (name == "eligibility") in.eligible = {0, 0};
            if (name == "branch_port" || name == "branch_before_issue") {
                in.cfi = 1; in.eligible[0] = name == "branch_port" ? 2 : 1;
            }
            c.run(in);
            if (name == "branch_before_issue") {
                in = {}; in.resolve = true; in.resolve_id = c.queue.front().id; c.run(in);
                throw std::runtime_error("stimulus guard did not reject "+name);
            }
            if (name != "eligibility") { in = {}; in.drain = true; c.run(in); }
            throw std::runtime_error("caller assertion did not reject "+name);
        }
        const unsigned seed = argc > 1 ? unsigned(std::stoul(argv[1])) : 1;
        const unsigned random_cycles = argc > 2 ? unsigned(std::stoul(argv[2])) : 20000;
        std::mt19937 rng(seed);
        directed(c, rng);
        const auto directed_cycles = c.cycles;
        random_run(c, rng, random_cycles);
        std::cout << "ISSUE BACKEND PASS seed=" << seed << " cycles=" << c.cycles << " directed=" << directed_cycles;
        for (const auto& [key, value] : c.coverage) std::cout << " " << key << "=" << value;
        std::cout << "\n";
    } catch (const std::exception& e) { std::cerr << e.what() << "\n"; return 1; }
}
