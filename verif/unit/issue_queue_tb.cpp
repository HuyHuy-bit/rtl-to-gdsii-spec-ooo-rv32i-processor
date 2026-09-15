#include "Vissue_queue.h"
#include "verilated.h"
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

struct Op {
    uint64_t position = 0, payload = 0;
    unsigned id = 0, destination = 0, eligible = 3;
    std::array<unsigned, 2> source{5, 6};
    std::array<bool, 2> ready{false, false};
    bool queued = true;
};
struct Input {
    std::vector<Op> dispatch;
    unsigned ports = 3, accepted = 0;
    std::array<unsigned, 2> wb{0, 0};
    bool reset = false, flush = false, recover = false, hold_retire = false;
    uint64_t cut = 0;
};
class Check {
    uint64_t tail = 0, payload_sequence = 1;
    std::array<unsigned, 32> generations{};
    bool wake_pending = false;
public:
    Vissue_queue dut;
    std::deque<Op> live;
    std::map<std::string, unsigned> coverage;
    unsigned cycles = 0;
    void require(bool ok, const std::string& what) const {
        if (!ok) throw std::runtime_error("issue queue mismatch cycle="+std::to_string(cycles)+" "+what);
    }
    unsigned occupancy() const {
        return unsigned(std::count_if(live.begin(), live.end(), [](const Op& e) { return e.queued; }));
    }
    void step(Input in = {}) {
        ++cycles;
        const unsigned count = occupancy();
        const bool stop = in.reset || in.flush || in.recover;
        require(stop || (in.dispatch.size() <= 2 && count+in.dispatch.size() <= 16
                && live.size()+in.dispatch.size() <= 32), "invalid reference dispatch");
        require(!in.recover || std::any_of(live.begin(), live.end(), [&](const Op& e) { return e.position == in.cut; }),
                "invalid reference recovery");
        const auto head = live.empty() ? tail : live.front().position;
        dut.clk_i = 0; dut.rst_i = in.reset; dut.flush_i = in.flush; dut.recover_i = in.recover;
        dut.head_slot_i = head%32; dut.recover_slot_i = in.cut%32;
        dut.dispatch_i = (1U << in.dispatch.size())-1;
        dut.dispatch_id_i = 0; dut.dispatch_source_i = 0; dut.dispatch_ready_i = 0;
        dut.dispatch_eligible_i = 0; dut.dispatch_destination_i = 0;
        for (unsigned word = 0; word < 4; ++word) dut.dispatch_payload_i[word] = 0;
        for (unsigned lane = 0; lane < in.dispatch.size(); ++lane) {
            auto& e = in.dispatch[lane];
            e.position = tail+lane;
            e.id = ((generations[e.position%32]&255) << 5) | unsigned(e.position%32);
            e.destination = unsigned(payload_sequence%63)+1;
            e.payload = UINT64_C(0x9e3779b97f4a7c15)*payload_sequence++;
            dut.dispatch_id_i |= e.id << (13*lane);
            dut.dispatch_source_i |= (e.source[0] | (e.source[1] << 6)) << (12*lane);
            dut.dispatch_ready_i |= (unsigned(e.ready[0]) | (unsigned(e.ready[1]) << 1)) << (2*lane);
            dut.dispatch_eligible_i |= e.eligible << (2*lane);
            dut.dispatch_destination_i |= e.destination << (6*lane);
            dut.dispatch_payload_i[2*lane] = uint32_t(e.payload);
            dut.dispatch_payload_i[2*lane+1] = uint32_t(e.payload >> 32);
        }
        dut.wb_accept_i = in.accepted;
        dut.wb_destination_i = in.wb[0] | (in.wb[1] << 6);
        dut.port_ready_i = in.ports;
        dut.eval();
        require(dut.occupancy_o == count, "occupancy");
        require(dut.dispatch_ready_o == (stop ? 0U : unsigned(count < 16) | (unsigned(count < 15) << 1)), "credits");

        // The oracle keeps program order, never the DUT's sparse slots or modular age comparison.
        std::array<int, 2> selected{-1, -1};
        bool any_wake = false, same_cycle_issue = false;
        std::vector<std::array<bool, 2>> readiness;
        for (const auto& e : live) {
            auto r = e.ready;
            for (unsigned operand = 0; operand < 2; ++operand) {
                r[operand] = r[operand] || e.source[operand] == 0;
                for (unsigned lane = 0; lane < 2; ++lane)
                    if ((in.accepted & (1U << lane)) && in.wb[lane] != 0 && in.wb[lane] == e.source[operand])
                        r[operand] = true;
            }
            any_wake |= e.queued && r != e.ready;
            readiness.push_back(r);
        }
        for (unsigned port = 0; port < 2; ++port) {
            if (!stop && (in.ports & (1U << port))) {
                for (unsigned n = 0; n < live.size(); ++n) {
                    const auto& e = live[n];
                    if (e.queued && readiness[n][0] && readiness[n][1] && (e.eligible & (1U << port))
                            && (port == 0 || selected[0] != int(n))) { selected[port] = int(n); break; }
                }
            }
            const bool issued = selected[port] >= 0;
            require(bool(dut.issue_o & (1U << port)) == issued, "launch port="+std::to_string(port));
            const Op expected = issued ? live[unsigned(selected[port])] : Op{};
            require(((dut.issue_id_o >> (13*port)) & 8191) == (issued ? expected.id : 0), "oldest identity");
            require(((dut.read_address_o >> (12*port)) & 4095) == (issued ? expected.source[0] | (expected.source[1] << 6) : 0), "sources");
            require(((dut.issue_destination_o >> (6*port)) & 63) == (issued ? expected.destination : 0), "destination");
            const uint64_t actual = dut.issue_payload_o[2*port] | (uint64_t(dut.issue_payload_o[2*port+1]) << 32);
            require(actual == (issued ? expected.payload : 0), "payload");
            if (issued) {
                same_cycle_issue |= readiness[unsigned(selected[port])] != expected.ready;
                if (expected.source[0] == 0 || expected.source[1] == 0) ++coverage["zero_source"];
                if (expected.source[0] == expected.source[1] && expected.source[0] != 0) ++coverage["same_source"];
                if (expected.eligible == 1) ++coverage["port0_only"];
                if (expected.eligible == 2) ++coverage["port1_only"];
                if (expected.id >> 5) ++coverage["generation_forwarded"];
            }
        }
        const unsigned launches = unsigned(selected[0] >= 0)+unsigned(selected[1] >= 0);
        if (!stop && in.dispatch.size() == 2) ++coverage["dual_dispatch"];
        if (launches == 2) ++coverage["dual_issue"];
        if (launches && !in.dispatch.empty()) ++coverage["dispatch_issue"];
        if (count == 16 && !stop) ++coverage["full"];
        if (count == 15 && !stop) ++coverage["one_credit"];
        if (count && in.ports == 0 && !stop) ++coverage["backpressure"];
        if (!stop && any_wake && in.ports == 0) ++coverage["stalled_wakeup"];
        if (same_cycle_issue && in.accepted) ++coverage["wakeup_issue"];
        if (wake_pending && launches && !in.accepted) ++coverage["retained_wakeup"];
        if (count && !in.accepted && (in.wb[0] || in.wb[1]) && !launches && !stop) ++coverage["unaccepted_broadcast"];
        if (in.accepted == 3 && any_wake && !stop) ++coverage["dual_wakeup"];
        if (head%32+live.size() > 32 && !stop) ++coverage["wrapped_window"];
        if (in.reset && count) ++coverage["reset_live"];
        if (in.flush && count) ++coverage["flush_live"];
        if (in.recover) {
            ++coverage["recovery"];
            if (in.cut%32 < head%32) ++coverage["recovery_wrap"];
            if (any_wake) ++coverage["recovery_wakeup"];
            for (const auto& e : live) if (e.queued)
                ++coverage[e.position > in.cut ? "recovery_killed" : "recovery_survivor"];
        }
        if (!stop && !in.dispatch.empty() && in.accepted) ++coverage["dispatch_wb"];
        wake_pending = any_wake && in.ports == 0 && !stop;
        dut.clk_i = 1; dut.eval();
        if (in.reset || in.flush) { live.clear(); tail = 0; if (in.reset) generations.fill(0); }
        else {
            for (unsigned n = 0; n < live.size(); ++n) if (live[n].queued) live[n].ready = readiness[n];
            if (in.recover) {
                while (!live.empty() && live.back().position > in.cut) live.pop_back();
                tail = in.cut+1;
            } else {
                for (auto n : selected) if (n >= 0) live[unsigned(n)].queued = false;
                for (auto e : in.dispatch) { ++generations[e.position%32]; live.push_back(e); }
                tail += in.dispatch.size();
            }
            if (!in.hold_retire) while (!live.empty() && !live.front().queued) live.pop_front();
        }
    }
    void clear(bool reset = false) { Input i; i.flush = !reset; i.reset = reset; step(i); }
    void drain() {
        Input i; i.accepted = 3;
        for (unsigned tag = 1; tag < 64; tag += 2) { i.wb = {tag, tag+1 < 64 ? tag+1 : 0}; step(i); }
        for (unsigned n = 0; n < 16; ++n) step();
        require(live.empty(), "drain completion");
    }
};
Op op(unsigned eligible = 3, bool ready = false, unsigned a = 5, unsigned b = 6) {
    Op e; e.eligible = eligible; e.ready = {ready, ready}; e.source = {a, b}; return e;
}
void directed(Check& c) {
    c.clear(true);
    Input i;
    for (unsigned n = 0; n < 30; ++n) { i = {}; i.dispatch = {op(3, true)}; c.step(i); c.step(); }
    i = {}; i.ports = 0; i.dispatch = {op(1), op(2)};
    for (unsigned n = 0; n < 8; ++n) c.step(i);
    i = {}; i.ports = 0; c.step(i);
    i = {}; i.wb = {5, 6}; c.step(i);
    i.accepted = 1; c.step(i);
    i.accepted = 2; i.ports = 1; c.step(i);
    i = {}; i.ports = 0; c.step(i);
    i.dispatch = {op(3, true)}; c.step(i);
    c.drain();

    c.clear();
    i = {}; i.ports = 0; i.dispatch = {op(3), op(3, false, 7, 7)}; c.step(i);
    i = {}; i.ports = 0; i.accepted = 3; i.wb = {5, 6}; c.step(i);
    c.step();
    i = {}; i.accepted = 2; i.wb = {0, 7}; c.step(i);
    i = {}; i.dispatch = {op(1, false, 0, 0), op(2, false, 0, 0)}; c.step(i); c.step();
    i = {}; i.dispatch = {op(3, true), op(1, true)}; c.step(i); c.step(); c.step();
    i = {}; i.dispatch = {op(2, true), op(3, true)}; c.step(i);
    i.dispatch = {op(3, true), op(3, true)}; i.accepted = 3; i.wb = {5, 6}; c.step(i); c.drain();

    // Refill holes around an older blocked entry, then make both ages eligible together.
    c.clear();
    i = {}; i.ports = 0; i.dispatch = {op(3, true), op(3)}; c.step(i);
    c.step();
    i = {}; i.ports = 0; i.dispatch = {op(3, true), op(3, true)}; c.step(i);
    i = {}; i.accepted = 3; i.wb = {5, 6}; c.step(i); c.drain();

    c.clear();
    for (unsigned n = 0; n < 30; ++n) { i = {}; i.dispatch = {op(3, true)}; c.step(i); c.step(); }
    i = {}; i.ports = 0; i.dispatch = {op(), op()};
    for (unsigned n = 0; n < 4; ++n) c.step(i);
    i = {}; i.recover = true; i.cut = c.live[3].position; i.accepted = 3; i.wb = {5, 6}; c.step(i);
    i.cut = c.live[1].position; i.accepted = 0; c.step(i);
    i = {}; i.ports = 0; i.dispatch = {op(3, true), op(3, true)}; c.step(i); c.drain();
    i = {}; i.ports = 0; i.dispatch = {op(), op()}; c.step(i); c.clear(); c.step();
    c.step(i); c.clear(true); c.step();
    // Control suppression includes simultaneously offered dispatch, ready ports and broadcasts.
    c.step(i); i.flush = true; i.ports = 3; i.accepted = 3; i.wb = {5, 6}; c.step(i); c.step();
    i.flush = false; i.ports = 0; i.accepted = 0; c.step(i); i.reset = true; i.ports = 3; c.step(i); c.step();
}
void random_run(Check& c, unsigned seed, unsigned cycles) {
    std::mt19937 rng(seed);
    for (unsigned n = 0; n < cycles; ++n) {
        Input i; i.ports = rng()%4; i.hold_retire = rng()%4 == 0;
        i.accepted = rng()%4; i.wb = {unsigned(rng()%64), unsigned(rng()%64)};
        if (i.accepted == 3 && i.wb[0] == i.wb[1]) i.accepted = 1;
        if (n%997 == 996) i.flush = true;
        else if (n%4001 == 4000) i.reset = true;
        else if (!c.live.empty() && rng()%31 == 0) { i.recover = true; i.cut = c.live[rng()%c.live.size()].position; }
        else {
            unsigned count = std::min({unsigned(rng()%3), 16-c.occupancy(), 32-unsigned(c.live.size())});
            for (unsigned lane = 0; lane < count; ++lane) {
                Op e = op(1+rng()%3, false, rng()%64, rng()%64);
                for (unsigned operand = 0; operand < 2; ++operand) {
                    e.ready[operand] = rng()%3 == 0 || e.source[operand] == 0;
                    for (unsigned w = 0; w < 2; ++w)
                        e.ready[operand] = e.ready[operand] || ((i.accepted & (1U << w)) && i.wb[w] == e.source[operand]);
                }
                i.dispatch.push_back(e);
            }
        }
        c.step(i);
    }
}
int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    try {
        Check c;
        if (argc > 1 && std::string(argv[1]) == "negative") {
            c.clear(true);
            const std::string name = argc > 2 ? argv[2] : "";
            Input i; i.ports = 0; i.dispatch = {op(), op()};
            if (name == "overflow") for (unsigned n = 0; n < 8; ++n) c.step(i);
            c.dut.clk_i = 0; c.dut.rst_i = 0; c.dut.dispatch_i = name == "prefix" ? 2 : 1;
            c.dut.dispatch_eligible_i = name == "eligibility" ? 0 : 15;
            if (name == "identity") c.dut.dispatch_i = 3;
            c.dut.eval(); c.dut.clk_i = 1; c.dut.eval();
            throw std::runtime_error("caller assertion did not reject "+name);
        }
        const unsigned seed = argc > 1 ? unsigned(std::stoul(argv[1])) : 1;
        const unsigned random_cycles = argc > 2 ? unsigned(std::stoul(argv[2])) : 20000;
        directed(c);
        const auto directed_cycles = c.cycles;
        random_run(c, seed, random_cycles);
        std::cout << "ISSUE QUEUE PASS seed=" << seed << " directed=" << directed_cycles << " cycles=" << c.cycles;
        for (const auto& [key, value] : c.coverage) std::cout << " " << key << "=" << value;
        std::cout << '\n';
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
