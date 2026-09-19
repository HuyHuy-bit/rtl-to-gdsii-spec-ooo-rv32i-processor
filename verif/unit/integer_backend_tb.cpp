#include "Vinteger_backend.h"
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
    return -1;
}
uint32_t instruction(unsigned op, unsigned rd, unsigned a, unsigned b, uint32_t imm = 0) {
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
    default: throw std::runtime_error("unsupported reference operation");
    }
}
struct Entry {
    unsigned id = 0, rd = 0, op = 0;
    uint32_t result = 0;
    std::array<int, 2> dependency{-1, -1};
    bool queued = true, done = false;
    Event event{};
};
struct Input {
    unsigned count = 0, execute = 3, complete = 3, retire = 3;
    std::array<uint32_t, 2> insn{0x13, 0x13}, pc{0, 4};
    bool reset = false, flush = false, drain = false;
};
class Check {
    std::array<uint32_t, 32> committed{};
    std::array<bool, 32> known{};
    std::array<unsigned, 32> uses{};
    std::array<int, 2> held{-1, -1};
    unsigned tail = 0;
    uint64_t order = 0;
public:
    Vinteger_backend dut;
    std::deque<Entry> queue;
    std::map<std::string, unsigned> coverage;
    unsigned cycles = 0;
    Check() { known[0] = true; }
    void require(bool ok, const std::string& what) const {
        if (!ok) throw std::runtime_error("integer backend mismatch cycle="+std::to_string(cycles)+" "+what);
    }
    unsigned locate(int id) const {
        for (unsigned n = 0; n < queue.size(); ++n) if (int(queue[n].id) == id) return n;
        return unsigned(queue.size());
    }
    unsigned resident() const {
        return unsigned(std::count_if(queue.begin(), queue.end(), [](const Entry& e) { return e.queued; }));
    }
    bool exhausted() const {
        return std::any_of(uses.begin(), uses.end(), [](unsigned n) { return n == 256; });
    }
    template<class T> void check_event(const T& data, unsigned lane, const Event& expected) const {
        for (unsigned b = 0; b < EVENT_BITS; ++b)
            require(bit(data, lane*EVENT_BITS+b) == bit(expected, b), "event bit="+std::to_string(b));
    }
    Entry build(uint32_t insn, uint32_t pc, unsigned id) const {
        Entry e; e.id = id; e.op = unsigned(operation(insn)); e.rd = (insn >> 7)&31;
        unsigned a = e.op < 2 ? 0 : (insn >> 15)&31;
        unsigned b = e.op < 11 ? 0 : (insn >> 20)&31;
        std::array<uint32_t, 2> values{committed[a], committed[b]};
        std::array<bool, 2> defined{known[a], known[b]};
        for (const auto& older : queue) for (unsigned operand = 0; operand < 2; ++operand)
            if (older.rd && older.rd == (operand ? b : a)) {
                values[operand] = older.result; defined[operand] = true; e.dependency[operand] = int(older.id);
            }
        require(defined[0] && defined[1], "stimulus reads undefined initial register");
        e.result = evaluate(e.op, insn, pc, values[0], values[1]);
        put(e.event, VALID_OFFSET, 1, 1); put(e.event, RETIRED_OFFSET, 1, 1);
        put(e.event, PRIVILEGE_OFFSET, 2, 3); put(e.event, INSTRUCTION_OFFSET, 32, insn);
        put(e.event, PC_BEFORE_OFFSET, 32, pc); put(e.event, PC_AFTER_OFFSET, 32, pc+4U);
        put(e.event, RS1_ADDR_OFFSET, 5, a); put(e.event, RS1_VALUE_OFFSET, 32, values[0]);
        put(e.event, RS2_ADDR_OFFSET, 5, b); put(e.event, RS2_VALUE_OFFSET, 32, values[1]);
        if (e.rd) {
            put(e.event, RD_ADDR_OFFSET, 5, e.rd); put(e.event, RD_VALUE_OFFSET, 32, e.result);
            put(e.event, RD_WRITE_MASK_OFFSET, 32, UINT32_MAX);
        }
        return e;
    }
    unsigned run(const Input& in = {}) {
        ++cycles;
        const bool active = !in.reset && !in.flush;
        unsigned supported = 0, pending = 0, wb = 0;
        for (unsigned lane = 0; lane < 2; ++lane) {
            if (operation(in.insn[lane]) >= 0 && !(in.pc[lane]&3)) supported |= 1U << lane;
            if (active && held[lane] >= 0) pending |= 1U << lane;
        }
        const unsigned complete = pending & in.complete;
        for (unsigned lane = 0; lane < 2; ++lane)
            if ((complete & (1U << lane)) && queue[locate(held[lane])].rd) wb |= 1U << lane;
        unsigned rv = 0;
        if (active && !queue.empty() && queue[0].done) {
            rv = 1;
            if (queue.size() > 1 && queue[1].done) rv = 3;
        }
        const unsigned retire = (rv & in.retire & 1) ? rv & in.retire : 0;
        const unsigned wanted = (1U << in.count)-1;
        unsigned alloc = active && !in.drain && !(wanted & ~supported)
            && queue.size()+in.count <= 32 && resident()+in.count <= 16 ? wanted : 0;
        for (unsigned lane = 0; lane < in.count; ++lane) if (uses[(tail+lane)%32] >= 256) alloc = 0;

        std::array<int, 2> pick{-1, -1};
        for (unsigned port = 0; port < 2; ++port) {
            if (!active || !(in.execute & (1U << port)) || (held[port] >= 0 && !(complete & (1U << port)))) continue;
            for (unsigned n = 0; n < queue.size(); ++n) {
                if (!queue[n].queued || (port && pick[0] == int(n))) continue;
                bool ready = true;
                for (auto dep : queue[n].dependency) {
                    unsigned index = locate(dep);
                    if (index < queue.size() && !queue[index].done) {
                        bool bypass = false;
                        for (unsigned lane = 0; lane < 2; ++lane) bypass |= (complete & (1U << lane)) && held[lane] == dep;
                        ready &= bypass;
                    }
                }
                if (ready) { pick[port] = int(n); break; }
            }
        }
        dut.clk_i = 0; dut.rst_i = in.reset; dut.flush_i = in.flush; dut.drained_i = in.drain;
        dut.valid_i = wanted; dut.instruction_i = uint64_t(in.insn[0]) | (uint64_t(in.insn[1]) << 32);
        dut.pc_i = uint64_t(in.pc[0]) | (uint64_t(in.pc[1]) << 32);
        dut.execution_ready_i = in.execute; dut.completion_enable_i = in.complete; dut.retire_ready_i = in.retire;
        dut.eval();
        require(dut.supported_o == supported, "supported instruction mask");
        require(dut.allocate_accept_o == alloc, "allocation");
        require(dut.occupancy_o == queue.size() && dut.issue_occupancy_o == resident(), "occupancy");
        require(dut.completion_valid_o == pending && dut.completion_accept_o == complete && dut.wb_accept_o == wb, "completion control");
        require(dut.retire_valid_o == rv && dut.retire_accept_o == retire, "retirement control");
        require(bool(dut.identity_drain_o) == exhausted(), "identity exhaustion");
        for (unsigned lane = 0; lane < 2; ++lane) {
            if (alloc & (1U << lane)) {
                unsigned slot = (tail+lane)%32;
                require(((dut.allocate_id_o >> (13*lane))&8191) == uses[slot]*32+slot, "allocation identity");
            }
            if (pending & (1U << lane)) {
                require(((dut.completion_id_o >> (13*lane))&8191) == unsigned(held[lane]), "held identity");
                check_event(dut.completion_event_o, lane, queue[locate(held[lane])].event);
            }
            require(bool(dut.issue_o & (1U << lane)) == (pick[lane] >= 0), "issue availability");
            if (pick[lane] >= 0) {
                const auto& e = queue[unsigned(pick[lane])];
                require(((dut.issue_id_o >> (13*lane))&8191) == e.id, "issue order/identity");
                for (unsigned n = 0; n < unsigned(pick[lane]); ++n) if (queue[n].queued && int(n) != pick[0]) { ++coverage["out_of_order_issue"]; break; }
                for (auto dep : e.dependency) for (unsigned w = 0; w < 2; ++w)
                    if ((complete & (1U << w)) && held[w] == dep) ++coverage["wakeup_bypass"];
                if (complete & (1U << lane)) ++coverage["consume_refill"];
            }
            if (rv & (1U << lane)) {
                Event e = queue[lane].event; put(e, ORDER_OFFSET, 64, order+lane);
                check_event(dut.retire_event_o, lane, e);
            } else require(!bit(dut.retire_event_o, lane*EVENT_BITS+VALID_OFFSET), "invalid retire record");
        }
        coverage["dual_allocate"] += alloc == 3; coverage["dual_issue"] += dut.issue_o == 3;
        coverage["dual_complete"] += complete == 3; coverage["dual_retire"] += retire == 3;
        coverage["completion_stall"] += (pending & ~in.complete) != 0;
        coverage["retire_stall"] += rv && !retire;
        coverage["issue_stall"] += resident() && !in.execute && active;
        coverage["queue_full"] += resident() == 16; coverage["rob_full"] += queue.size() == 32;
        coverage["unsupported"] += active && wanted && (wanted & ~supported);
        coverage["flush_held"] += in.flush && (held[0] >= 0 || held[1] >= 0);
        coverage["reset_held"] += in.reset && (held[0] >= 0 || held[1] >= 0);
        coverage["drain"] += in.drain;
        coverage["generation_stall"] += active && wanted && uses[tail] == 256 && !alloc;
        coverage["wrapped_window"] += !queue.empty() && tail < queue.front().id%32;
        if (in.reset) { queue.clear(); held = {-1, -1}; uses.fill(0); committed.fill(0); known.fill(false); known[0] = true; order = 0; tail = 0; }
        else if (in.flush) { queue.clear(); held = {-1, -1}; tail = 0; }
        else {
            for (unsigned port = 0; port < 2; ++port) {
                if (complete & (1U << port)) {
                    unsigned n = locate(held[port]); queue[n].done = true;
                    for (unsigned older = 0; older < n; ++older) if (!queue[older].done) { ++coverage["out_of_order_complete"]; break; }
                    coverage["resultless"] += queue[n].rd == 0;
                    held[port] = -1;
                }
                if (pick[port] >= 0) {
                    auto& e = queue[unsigned(pick[port])]; e.queued = false; held[port] = int(e.id);
                }
            }
            for (unsigned lane = 0; lane < 2; ++lane) if (retire & (1U << lane)) {
                auto e = queue.front(); queue.pop_front(); ++order; ++coverage["retired"];
                ++coverage["op_"+std::to_string(e.op)];
                if (e.rd) { committed[e.rd] = e.result; known[e.rd] = true; }
            }
            for (unsigned lane = 0; lane < 2; ++lane) if (alloc & (1U << lane)) {
                auto e = build(in.insn[lane], in.pc[lane], uses[tail]*32+tail);
                if (lane && queue.back().rd) {
                    coverage["bundle_raw"] += e.dependency[0] == int(queue.back().id) || e.dependency[1] == int(queue.back().id);
                    coverage["bundle_waw"] += e.rd == queue.back().rd;
                }
                queue.push_back(e); ++uses[tail]; tail = (tail+1)%32;
            }
            if (in.drain) { require(queue.empty() && held[0] < 0 && held[1] < 0, "invalid stimulus drain"); uses.fill(0); }
        }
        dut.clk_i = 1; dut.eval(); dut.clk_i = 0; dut.eval();
        return alloc;
    }
    void drain() {
        for (unsigned n = 0; !queue.empty(); ++n) { require(n < 200, "progress watchdog"); run(); }
        run();
    }
    void send(uint32_t a, uint32_t b = 0x13, unsigned count = 2, uint32_t pc = 0x80000000U) {
        Input i; i.count = count; i.insn = {a,b}; i.pc = {pc,pc+4};
        for (unsigned n = 0; !run(i); ++n) require(n < 200, "dispatch progress watchdog");
    }
    void initialize() {
        for (unsigned r = 1; r < 32; r += 2) send(instruction(0,r,0,0,r*0x12345000U),instruction(0,(r+1)%32,0,0,(r+1)*0xabcde000U),r == 31 ? 1 : 2);
        drain();
    }
    void constant(unsigned reg, uint32_t v) {
        send(instruction(0,reg,0,0,v+0x800U), instruction(2,reg,reg,0,v)); drain();
    }
};
void directed(Check& c) {
    Input i; i.reset = true; c.run(i); c.initialize();
    const uint32_t values[] = {0,1,UINT32_MAX,0x80000000U,0x7fffffffU,0x55555555U,0xaaaaaaaaU,31,32,63};
    for (unsigned n = 0; n < 10; ++n) {
        c.constant(1,values[n]); c.constant(2,values[9-n]);
        for (unsigned op = 0; op < 21; ++op) {
            c.send(instruction(op,3,1,2,values[n]),instruction(op,4,2,1,values[9-n]),2,n == 0 ? 0xfffffffcU : 0x1000U);
            c.drain();
        }
    }
    // Hold both producers, then accept and refill one while the other remains blocked.
    c.send(instruction(0,5,0,0,0x12345000),instruction(0,6,0,0,0x87654000));
    i = {}; i.complete = 0; c.run(i);
    i.count = 2; i.insn = {instruction(2,7,0,0,7),instruction(2,8,0,0,8)}; c.run(i);
    i.count = 0; for (unsigned n = 0; n < 5; ++n) c.run(i);
    i.complete = 1; c.run(i); c.run(i); c.drain();
    // An independent younger instruction bypasses a consumer waiting for port zero's held result.
    c.send(instruction(0,10,0,0,0x12345000),0x13,1); i = {}; i.complete = 2; c.run(i);
    i.count = 2; i.insn = {instruction(2,11,10,0,1),instruction(0,12,0,0,0x77777000)}; c.run(i);
    i.count = 0; for (unsigned n = 0; n < 4; ++n) c.run(i); c.drain();
    c.send(instruction(2,13,0,0,10), instruction(2,13,13,0,20)); c.drain();
    i = {}; i.execute = 0; i.count = 2;
    for (unsigned n = 0; n < 10; ++n) c.run(i);
    c.drain();
    i = {}; i.retire = 0; i.count = 2;
    for (unsigned n = 0; n < 22; ++n) c.run(i);
    c.drain();
    const uint32_t bad[] = {0,0x6f,0x63,0x2003,0x2023,0x1073,0xf,0x02000033,0x02001013,0x40001033};
    for (auto word : bad) {
        i = {}; i.count = 2; i.insn = {word,0x13}; c.run(i);
        i.insn = {0x13,word}; c.run(i);
    }
    i = {}; i.count = 2; i.pc[1] = 6; c.run(i);
    for (unsigned reset = 0; reset < 2; ++reset) {
        c.send(instruction(2,14,0,0,101),instruction(2,15,0,0,202));
        i = {}; i.complete = 0; c.run(i);
        i.flush = !reset; i.reset = reset; c.run(i); c.run();
    }
    // Exhaust every ROB slot's generation before a coordinated empty-producer drain.
    for (unsigned n = 0; n < 4096; ++n) c.send(0x13,0x13);
    c.drain(); i = {}; i.count = 2; c.run(i);
    i = {}; i.drain = true; c.run(i); c.send(0x13,0x13); c.drain();
    i = {}; i.reset = true; c.run(i); c.initialize();
}
int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    try {
        Check c;
        if (argc > 1 && std::string(argv[1]) == "negative") {
            Input i; i.reset = true; c.run(i);
            const std::string name = argc > 2 ? argv[2] : "";
            if (name == "held_drain") { c.send(0x13,0x13); i = {}; i.complete = 0; c.run(i); }
            c.dut.clk_i = 0; c.dut.rst_i = 0; c.dut.valid_i = name == "prefix" ? 2 : 0;
            c.dut.drained_i = name == "held_drain"; c.dut.eval(); c.dut.clk_i = 1; c.dut.eval();
            throw std::runtime_error("caller assertion did not reject "+name);
        }
        unsigned seed = argc > 1 ? unsigned(std::stoul(argv[1])) : 1;
        unsigned random_cycles = argc > 2 ? unsigned(std::stoul(argv[2])) : 20000;
        directed(c); const unsigned directed_cycles = c.cycles;
        std::mt19937 rng(seed);
        for (unsigned n = 0; n < random_cycles; ++n) {
            Input i; i.count = rng()%3; i.execute = rng()%4; i.complete = rng()%4; i.retire = rng()%4;
            for (unsigned lane = 0; lane < 2; ++lane) {
                i.insn[lane] = instruction(rng()%21,rng()%32,rng()%32,rng()%32,rng());
                i.pc[lane] = rng()&~3U;
            }
            i.flush = rng()%233 == 0;
            if (c.exhausted()) { i = {}; i.drain = c.queue.empty(); }
            c.run(i);
        }
        const unsigned random_end = c.cycles; c.drain();
        std::cout << "INTEGER BACKEND PASS seed=" << seed << " directed=" << directed_cycles
                  << " random=" << random_cycles << " final_drain=" << c.cycles-random_end << " cycles=" << c.cycles;
        for (const auto& [key,value] : c.coverage) std::cout << " " << key << "=" << value;
        std::cout << '\n';
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
