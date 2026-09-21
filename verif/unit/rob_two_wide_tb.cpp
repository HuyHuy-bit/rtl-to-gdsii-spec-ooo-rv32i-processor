#include "Vrob_two_wide.h"
#include "verilated.h"
#include "rob_event_layout.hpp"
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
    bool cfi, solo, done = false, resolved = false;
    Event event{};
};
struct Completion {
    unsigned id = 0;
    bool offer = false, take = false, solo = false;
    Event event{};
};
struct Input {
    unsigned alloc = 0, cfi = 0, solo = 0, retire = 0, resolve_id = 0;
    bool reset = false, flush = false, drain = false, resolve = false, resolve_take = false, mispredict = false, trap = false;
    std::array<Completion, 2> complete{};
};
class Check {
    Vrob_two_wide dut;
    unsigned tail = 0, serial = 0;
    uint64_t order = 0;
public:
    std::deque<Entry> queue;
    std::array<unsigned, 32> uses{};
    std::vector<unsigned> old_ids;
    std::map<std::string, unsigned> coverage;
    unsigned cycles = 0;
    void require(bool ok, const std::string& what) const {
        if (!ok) throw std::runtime_error("rob two wide mismatch cycle="+std::to_string(cycles)+" "+what);
    }
    unsigned credits() const {
        unsigned n = 0;
        while (n < 2 && queue.size()+n < 32 && uses[(tail+n)%32] < 256) ++n;
        return n;
    }
    unsigned eligible() const {
        unsigned n = 0;
        for (const auto& e : queue) {
            if (n == 2 || !e.done || bit(e.event, TRAP_OFFSET) || (e.cfi && !e.resolved)) break;
            if (n && (e.solo || queue.front().solo)) break;
            ++n;
        }
        return n;
    }
    unsigned locate(unsigned id) const {
        for (unsigned i = 0; i < queue.size(); ++i) if (queue[i].id == id) return i;
        return queue.size();
    }
    Completion result(unsigned i, std::mt19937& rng, bool trap = false, bool solo = false) const {
        Completion c; c.id = queue.at(i).id; c.offer = c.take = true; c.solo = solo;
        for (auto& word : c.event) word = rng();
        put(c.event, TRAP_OFFSET, 1, trap);
        return c;
    }
    template<class T> void event_equal(const T& actual, unsigned lane, Event expected, bool trapped) {
        put(expected, VALID_OFFSET, 1, 1);
        put(expected, ORDER_OFFSET, 64, order+lane);
        put(expected, RETIRED_OFFSET, 1, !trapped);
        for (unsigned b = 0; b < EVENT_BITS; ++b)
            require(bit(actual, lane*EVENT_BITS+b) == bit(expected, b), "event payload bit="+std::to_string(b));
    }
    void run(const Input& in = {}) {
        ++cycles;
        const bool active = !in.reset && !in.flush;
        const bool trap_valid = active && !queue.empty() && queue.front().done && bit(queue.front().event, TRAP_OFFSET);
        const bool trap_accept = trap_valid && in.trap;
        const auto ri = locate(in.resolve_id);
        const bool resolve_ready = active && !trap_accept && ri < queue.size() && queue[ri].cfi && !queue[ri].resolved;
        const bool resolve = in.resolve && in.resolve_take && resolve_ready;
        const bool recover = resolve && in.mispredict;
        const unsigned allocation_ready = active && !trap_accept && !recover && !in.drain ? (1U << credits())-1 : 0;
        const unsigned allocation = ((1U << in.alloc)-1) & allocation_ready;
        const unsigned retire_valid = active && !trap_accept && !recover ? (1U << eligible())-1 : 0;
        const unsigned retirement = ((1U << in.retire)-1) & retire_valid;
        unsigned cr = 0, ca = 0;
        for (unsigned lane = 0; lane < 2; ++lane) {
            const auto& c = in.complete[lane]; const auto pos = locate(c.id);
            bool ready = active && !trap_accept && pos < queue.size() && !queue[pos].done && (!recover || pos <= ri);
            if (lane && in.complete[0].offer && c.id == in.complete[0].id) ready = false;
            cr |= unsigned(ready) << lane;
            ca |= unsigned(ready && c.offer && c.take) << lane;
        }
        dut.serial_offer_i = 0; dut.serial_id_i = 0;
        for (unsigned word=0;word<(EVENT_BITS+31)/32;word++) dut.serial_event_i[word]=0;
        dut.clk_i = 0; dut.rst_i = in.reset; dut.flush_i = in.flush; dut.drained_i = in.drain;
        dut.allocate_i = (1U << in.alloc)-1; dut.allocate_cfi_i = in.cfi; dut.allocate_solo_i = in.solo;
        dut.allocate_pc_i = 0;
        dut.allocate_rd_i = dut.allocate_destination_i = dut.allocate_stale_i = 0;
        for (unsigned lane = 0; lane < 2; ++lane) {
            const unsigned value = serial+lane;
            dut.allocate_pc_i |= uint64_t(0x80000000U+4*value) << (lane*32);
            dut.allocate_rd_i |= (value%32) << (lane*5);
            dut.allocate_destination_i |= (value%32 ? 32+value%32 : 0) << (lane*6);
            dut.allocate_stale_i |= (value%32) << (lane*6);
        }
        dut.complete_offer_i = dut.complete_take_i = dut.complete_solo_i = dut.complete_id_i = 0;
        for (unsigned w = 0; w < (2*EVENT_BITS+31)/32; ++w) dut.complete_event_i[w] = 0;
        for (unsigned lane = 0; lane < 2; ++lane) {
            const auto& c = in.complete[lane];
            dut.complete_offer_i |= unsigned(c.offer) << lane; dut.complete_take_i |= unsigned(c.take) << lane;
            dut.complete_solo_i |= unsigned(c.solo) << lane; dut.complete_id_i |= c.id << (lane*13);
            for (unsigned b = 0; b < EVENT_BITS; ++b) put(dut.complete_event_i, lane*EVENT_BITS+b, 1, bit(c.event, b));
        }
        dut.resolve_offer_i = in.resolve; dut.resolve_take_i = in.resolve_take; dut.resolve_id_i = in.resolve_id;
        dut.mispredict_i = in.mispredict; dut.trap_take_i = in.trap; dut.retire_take_i = (1U << in.retire)-1;
        dut.eval();
        require(dut.occupancy_o == queue.size(), "occupancy");
        require(bool(dut.identity_drain_o) == std::any_of(uses.begin(), uses.end(), [](unsigned n) { return n == 256; }), "identity drain");
        require(dut.head_valid_o == (active && !queue.empty()), "head valid");
        if (dut.head_valid_o) require(dut.head_id_o == queue.front().id && dut.head_pc_o == queue.front().pc, "head identity and restart PC");
        require(dut.allocate_ready_o == allocation_ready && dut.allocate_accept_o == allocation, "allocation");
        for (unsigned lane = 0; lane < 2; ++lane) if (allocation_ready & (1U << lane)) {
            unsigned slot = (tail+lane)%32;
            require(((dut.allocate_id_o >> (lane*13))&8191) == uses[slot]*32+slot, "allocation identity");
        }
        require(dut.complete_ready_o == cr && dut.complete_accept_o == ca, "completion identity or acceptance");
        for (unsigned lane = 0; lane < 2; ++lane) if (cr & (1U << lane))
            require(((dut.complete_destination_o >> (lane*6))&63) == queue[locate(in.complete[lane].id)].destination, "completion destination");
        require(dut.resolve_ready_o == resolve_ready && dut.resolve_accept_o == resolve && dut.branch_recover_o == recover, "resolution");
        require(dut.retire_valid_o == retire_valid && dut.retire_accept_o == retirement, "retirement eligibility");
        require(dut.trap_valid_o == trap_valid && dut.trap_accept_o == trap_accept, "trap eligibility");
        for (unsigned lane = 0; lane < 2; ++lane) {
            require(bit(dut.retire_event_o, lane*EVENT_BITS+VALID_OFFSET) == ((retire_valid >> lane)&1), "event valid");
            if (retire_valid & (1U << lane)) {
                const auto& e = queue[lane];
                require(((dut.retire_rd_o >> (lane*5))&31) == e.rd && ((dut.retire_destination_o >> (lane*6))&63) == e.destination
                    && ((dut.retire_stale_o >> (lane*6))&63) == e.stale, "retirement metadata");
                event_equal(dut.retire_event_o, lane, e.event, false);
            }
        }
        require(bit(dut.trap_event_o, VALID_OFFSET) == trap_valid, "trap event valid");
        if (trap_valid) event_equal(dut.trap_event_o, 0, queue.front().event, true);
        coverage["full"] += queue.size() == 32;
        coverage["dual_allocate"] += allocation == 3;
        coverage["dual_retire"] += retirement == 3;
        coverage["dual_complete"] += ca == 3;
        coverage["simultaneous"] += allocation && retirement && ca;
        coverage["backpressure"] += retire_valid && !retirement;
        coverage["unresolved"] += active && !queue.empty() && queue.front().done && queue.front().cfi && !queue.front().resolved;
        coverage["solo"] += retirement == 1 && queue.front().solo;
        coverage["trap"] += trap_accept;
        coverage["flush_live"] += in.flush && !queue.empty();
        coverage["reset_live"] += in.reset && !queue.empty();
        coverage["recovery"] += recover;
        coverage["recovery_wrap"] += recover && (queue.front().id%32 > in.resolve_id%32);
        coverage["recovery_complete"] += recover && ca;
        coverage["recovery_suppression"] += recover && (in.alloc || in.retire);
        coverage["resolution_complete"] += resolve && ca;
        coverage["out_of_order"] += ca && locate(in.complete[(ca&1) ? 0 : 1].id) > 0;
        coverage["duplicate"] += in.complete[0].offer && in.complete[1].offer && in.complete[0].id == in.complete[1].id;
        coverage["stale"] += active && in.complete[0].offer && locate(in.complete[0].id) == queue.size();
        coverage["stale_resolution"] += active && in.resolve && ri == queue.size();
        coverage["wrap_block"] += uses[tail] == 256 && !in.drain && active;
        coverage["drain"] += in.drain;
        coverage["trap_priority"] += trap_accept && in.resolve;
        if (in.reset) {
            queue.clear(); uses.fill(0); old_ids.clear(); tail = serial = 0; order = 0;
        } else {
            order += (retirement == 3 ? 2 : retirement) + unsigned(trap_accept);
            if (in.flush || trap_accept) {
                for (const auto& e : queue) old_ids.push_back(e.id);
                queue.clear(); tail = 0;
            } else {
                if (resolve) queue[ri].resolved = true;
                for (unsigned lane = 0; lane < 2; ++lane) if (ca & (1U << lane)) {
                    auto& e = queue[locate(in.complete[lane].id)];
                    e.done = true; e.event = in.complete[lane].event; e.solo |= in.complete[lane].solo;
                }
                if (recover) {
                    tail = (queue[ri].id%32+1)%32;
                    while (queue.size() > ri+1) { old_ids.push_back(queue.back().id); queue.pop_back(); }
                } else {
                    for (unsigned lane = 0; lane < 2; ++lane) if (retirement & (1U << lane)) {
                        old_ids.push_back(queue.front().id); queue.pop_front();
                    }
                    for (unsigned lane = 0; lane < 2; ++lane) if (allocation & (1U << lane)) {
                        const auto value = serial++;
                        queue.push_back({uses[tail]*32+tail, value%32, value%32 ? 32+value%32 : 0, value%32, 0x80000000U+4*value,
                                         bool(in.cfi & (1U << lane)), bool(in.solo & (1U << lane))});
                        coverage["slot_wrap"] += tail == 31;
                        ++uses[tail]; tail = (tail+1)%32;
                    }
                }
                if (in.drain) { uses.fill(0); old_ids.clear(); }
            }
        }
        dut.clk_i = 1; dut.eval(); dut.clk_i = 0; dut.eval();
    }
};

void empty(Check& c, std::mt19937& rng) {
    while (!c.queue.empty()) {
        Input in;
        if (c.queue.front().done && bit(c.queue.front().event, TRAP_OFFSET)) in.trap = true;
        else {
            in.retire = c.eligible();
            for (unsigned i = 0, lane = 0; i < c.queue.size() && lane < 2; ++i)
                if (!c.queue[i].done) in.complete[lane++] = c.result(i, rng);
            for (const auto& e : c.queue) if (e.cfi && !e.resolved) {
                in.resolve = in.resolve_take = true; in.resolve_id = e.id; break;
            }
        }
        c.run(in);
    }
}
void directed(Check& c, std::mt19937& rng) {
    Input in; in.reset = true; c.run(in);
    for (unsigned i = 0; i < 16; ++i) { in = {}; in.alloc = 2; c.run(in); }
    in = {}; c.run(in);
    for (int i = 30; i >= 0; i -= 2) { in = {}; in.complete[0] = c.result(i, rng); if (i != 30) in.complete[1] = c.result(i+1, rng); c.run(in); }
    c.run();
    for (unsigned i = 0; i < 15; ++i) { in = {}; in.retire = 2; c.run(in); }
    // The surviving head is slot 30, and the redirecting CFI is slot zero.
    in = {}; in.alloc = 2; in.cfi = 1; c.run(in);
    in = {}; in.alloc = 2; c.run(in);
    in = {}; in.resolve = in.resolve_take = in.mispredict = true; in.resolve_id = c.queue[2].id;
    in.complete[0] = c.result(2, rng); in.complete[1] = c.result(1, rng);
    in.alloc = in.retire = 2; c.run(in);
    in = {}; in.complete[0].offer = true; in.complete[0].id = c.old_ids.back();
    in.resolve = true; in.resolve_id = c.old_ids.back(); c.run(in);
    empty(c, rng);
    in = {}; in.alloc = 2; in.cfi = 1; c.run(in);
    in = {}; in.complete[0] = c.result(0, rng); in.complete[1] = in.complete[0]; in.complete[1].take = false; c.run(in);
    c.run();
    in = {}; in.resolve = in.resolve_take = true; in.resolve_id = c.queue[0].id; c.run(in);
    in = {}; in.complete[0] = c.result(1, rng, false, true); c.run(in);
    empty(c, rng);
    in = {}; in.alloc = 2; in.cfi = 2; c.run(in);
    in = {}; in.complete[0] = c.result(0, rng, true); c.run(in);
    c.run();
    in = {}; in.trap = true; in.resolve = in.resolve_take = in.mispredict = true;
    in.resolve_id = c.queue[1].id; in.alloc = in.retire = 2; c.run(in);
    in = {}; in.alloc = 2; c.run(in);
    in = {}; in.flush = true; in.complete[0] = c.result(0, rng); in.retire = in.alloc = 2; c.run(in);
    in = {}; in.alloc = 2; c.run(in);
    in = {}; in.reset = true; c.run(in);
    in = {}; in.alloc = 2; c.run(in);
    in = {}; in.complete = {c.result(0, rng), c.result(1, rng)}; in.alloc = 2; c.run(in);
    in = {}; in.complete = {c.result(2, rng), c.result(3, rng)}; in.alloc = in.retire = 2; c.run(in);
    empty(c, rng);
    in = {}; in.reset = true; c.run(in);
    in = {}; in.alloc = 2; in.cfi = 1; c.run(in);
    const unsigned canceled = c.queue.front().id;
    in = {}; in.flush = true; c.run(in);
    in = {}; in.alloc = 2; in.cfi = 1; c.run(in);
    in = {}; in.complete[0].id = canceled; in.complete[0].offer = true;
    in.resolve = true; in.resolve_id = canceled; c.run(in);
    in = {}; in.complete[0] = c.result(0, rng); in.complete[0].take = false;
    c.run(in); c.run(in); in.complete[0].take = true; c.run(in);
    in = {}; in.complete[0] = c.result(1, rng, true); c.run(in);
    in = {}; in.resolve = in.resolve_take = true; in.resolve_id = c.queue.front().id; c.run(in);
    in = {}; in.retire = 1; c.run(in);
    in = {}; in.trap = true; c.run(in);
    in = {}; in.reset = true; c.run(in);
    // Keep one stale identity observable until its slot exhausts every generation.
    for (unsigned round = 0; round < 256; ++round) {
        for (unsigned i = 0; i < 16; ++i) { in = {}; in.alloc = 2; c.run(in); }
        empty(c, rng);
    }
    c.run();
    c.require(c.credits() == 0, "wrap must stall");
    in = {}; in.complete[0].offer = true; in.complete[0].id = 0; c.run(in);
    in = {}; in.drain = true; c.run(in);
    in = {}; in.alloc = 2; c.run(in); empty(c, rng);
}
int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    try {
        if (argc > 2 && std::string(argv[1]) == "negative") {
            Check c; Input in; in.reset = true; c.run(in); in = {}; in.alloc = 2; c.run(in);
            in = {}; const std::string name = argv[2];
            if (name == "drain_live") in.drain = true;
            else if (name == "complete_stale") { in.complete[0].offer = in.complete[0].take = true; in.complete[0].id = 99; }
            else if (name == "resolve_stale") { in.resolve = in.resolve_take = true; in.resolve_id = 99; }
            else if (name == "retire_unready") in.retire = 1;
            else if (name == "trap_unready") in.trap = true;
            else throw std::runtime_error("unknown negative");
            c.run(in); throw std::runtime_error("negative survived");
        }
        const unsigned seed = argc > 1 ? std::stoul(argv[1]) : 1;
        const unsigned count = argc > 2 ? std::stoul(argv[2]) : 20000;
        std::mt19937 rng(seed); Check c; directed(c, rng);
        const unsigned directed_cycles = c.cycles;
        for (unsigned cycle = 0; cycle < count; ++cycle) {
            Input in;
            if (rng()%300 == 0) in.flush = true;
            else if (!c.queue.empty() && c.queue.front().done && bit(c.queue.front().event, TRAP_OFFSET)) in.trap = rng()%2;
            else {
                in.alloc = rng()%(c.credits()+1); in.cfi = (rng()%4 == 0 ? (in.alloc == 2 ? 2 : 1) : 0);
                in.solo = rng()%8 == 0 ? 1 : 0; in.retire = rng()%(c.eligible()+1);
                std::vector<unsigned> undone, branches;
                for (unsigned i = 0; i < c.queue.size(); ++i) {
                    if (!c.queue[i].done) undone.push_back(i);
                    if (c.queue[i].cfi && !c.queue[i].resolved) branches.push_back(i);
                }
                std::shuffle(undone.begin(), undone.end(), rng);
                for (unsigned lane = 0; lane < 2 && lane < undone.size(); ++lane) if (rng()%4) {
                    in.complete[lane] = c.result(undone[lane], rng, rng()%100 == 0, rng()%25 == 0);
                    in.complete[lane].take = rng()%4 != 0;
                }
                if (!branches.empty() && rng()%3 == 0) {
                    const auto pos = branches[rng()%branches.size()];
                    in.resolve = true; in.resolve_take = rng()%4 != 0; in.resolve_id = c.queue[pos].id;
                    in.mispredict = rng()%3 == 0;
                    if (in.resolve_take && in.mispredict)
                        for (auto& wb : in.complete) if (c.locate(wb.id) > pos) wb.take = false;
                }
                if (!c.old_ids.empty() && rng()%8 == 0) {
                    in.complete[0] = {}; in.complete[0].offer = true; in.complete[0].id = c.old_ids[rng()%c.old_ids.size()];
                }
                if (c.queue.empty() && std::any_of(c.uses.begin(), c.uses.end(), [](unsigned n) { return n == 256; })) {
                    in = {}; in.drain = true;
                }
            }
            c.run(in);
        }
        std::cout << "ROB TWO WIDE PASS seed=" << seed << " cycles=" << c.cycles << " directed=" << directed_cycles;
        for (const auto& [name, value] : c.coverage) std::cout << " " << name << "=" << value;
        std::cout << "\n";
    } catch (const std::exception& e) { std::cerr << e.what() << "\n"; return 1; }
}
