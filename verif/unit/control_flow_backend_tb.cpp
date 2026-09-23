#include "packed_bits.hpp"
using packed_bits::bit;
using packed_bits::put;
using packed_bits::get;
#include "Vcontrol_flow_backend.h"
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
#include "rv32_reference.hpp"
using rv32::instruction;
using rv32::signed_value;
using rv32::arithmetic_right;
using rv32::extend;
using rv32::illegal;
using rv32::operation;
using rv32::evaluate;
struct Entry {
    unsigned id = 0, rd = 0, op = 0;
    uint32_t result = 0, next_pc = 0;
    std::array<int, 2> dependency{-1, -1};
    bool queued = true, done = false, resolved = false, fault = false, taken = false, miss = false, cleared_lsb = false;
    Event event{};
    bool cfi() const { return op >= 21; }
};
struct Input {
    unsigned count = 0, execute = 3, complete = 3, retire = 3;
    std::array<uint32_t, 2> insn{0x13,0x13}, pc{0,4}, predicted_pc{4,8};
    unsigned predicted_taken = 0;
    bool grant = true, reset = false, flush = false, drain = false;
};
class Check {
    std::array<uint32_t, 32> committed{};
    std::array<bool, 32> known{};
    std::array<unsigned, 32> uses{};
    std::array<int, 2> held{-1,-1};
    std::array<int, 8> checkpoints{-1,-1,-1,-1,-1,-1,-1,-1};
    std::array<unsigned, 8> checkpoint_uses{};
    unsigned tail = 0;
    uint64_t order = 0;
public:
    Vcontrol_flow_backend dut;
    std::deque<Entry> queue;
    std::map<std::string, unsigned> coverage;
    unsigned cycles = 0;
    Check() { known[0] = true; }
    void require(bool ok, const std::string& what) const {
        if (!ok) throw std::runtime_error("control flow backend mismatch cycle="+std::to_string(cycles)+" "+what);
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
    bool head_fault() const { return !queue.empty() && queue.front().done && queue.front().fault; }
    template<class T> void check_event(const T& data, unsigned lane, const Event& expected) const {
        for (unsigned b = 0; b < EVENT_BITS; ++b)
            require(bit(data, lane*EVENT_BITS+b) == bit(expected, b), "event bit="+std::to_string(b));
    }
    Entry build(uint32_t insn, uint32_t pc, unsigned id, bool predicted_taken, uint32_t predicted_pc) const {
        Entry e; e.id = id; e.op = unsigned(operation(insn));
        e.rd = e.op >= 21 && e.op <= 26 ? 0 : (insn >> 7)&31;
        unsigned a = e.op < 2 || e.op == 27 ? 0 : (insn >> 15)&31;
        unsigned b = e.op < 11 || e.op >= 27 ? 0 : (insn >> 20)&31;
        std::array<uint32_t, 2> values{committed[a],committed[b]};
        std::array<bool, 2> defined{known[a],known[b]};
        for (const auto& older : queue) for (unsigned operand = 0; operand < 2; ++operand)
            if (older.rd && older.rd == (operand ? b : a)) {
                values[operand] = older.result; defined[operand] = true; e.dependency[operand] = int(older.id);
            }
        require(defined[0] && defined[1], "stimulus reads undefined initial register");
        e.result = evaluate(e.op, insn, pc, values[0], values[1]);
        e.next_pc = pc+4U;
        if (e.op >= 21 && e.op <= 26) {
            switch (e.op) {
            case 21: e.taken = values[0] == values[1]; break;
            case 22: e.taken = values[0] != values[1]; break;
            case 23: e.taken = signed_value(values[0]) < signed_value(values[1]); break;
            case 24: e.taken = signed_value(values[0]) >= signed_value(values[1]); break;
            case 25: e.taken = values[0] < values[1]; break;
            case 26: e.taken = values[0] >= values[1]; break;
            }
            uint32_t off = ((insn >> 31)&1U) << 12 | ((insn >> 7)&1U) << 11
                | ((insn >> 25)&63U) << 5 | ((insn >> 8)&15U) << 1;
            if (e.taken) e.next_pc = pc+extend(off,13);
        } else if (e.op == 27) {
            uint32_t off = ((insn >> 31)&1U) << 20 | ((insn >> 12)&255U) << 12
                | ((insn >> 20)&1U) << 11 | ((insn >> 21)&1023U) << 1;
            e.taken = true; e.next_pc = pc+extend(off,21);
        } else if (e.op == 28) {
            e.taken = true;
            uint32_t target = values[0]+extend(insn >> 20,12);
            e.cleared_lsb = target&1U; e.next_pc = target&~1U;
        }
        e.fault = (e.next_pc&3) != 0;
        e.miss = e.cfi() && (predicted_taken != e.taken || predicted_pc != e.next_pc);
        put(e.event, VALID_OFFSET, 1, 1); put(e.event, RETIRED_OFFSET, 1, !e.fault);
        put(e.event, PRIVILEGE_OFFSET, 2, 3); put(e.event, INSTRUCTION_OFFSET, 32, insn);
        put(e.event, PC_BEFORE_OFFSET, 32, pc); put(e.event, PC_AFTER_OFFSET, 32, e.next_pc);
        put(e.event, RS1_ADDR_OFFSET, 5, a); put(e.event, RS1_VALUE_OFFSET, 32, values[0]);
        put(e.event, RS2_ADDR_OFFSET, 5, b); put(e.event, RS2_VALUE_OFFSET, 32, values[1]);
        if (e.fault) { put(e.event, TRAP_OFFSET, 1, 1); put(e.event, TRAP_VALUE_OFFSET, 32, e.next_pc); }
        else if (e.rd) {
            put(e.event, RD_ADDR_OFFSET, 5, e.rd); put(e.event, RD_VALUE_OFFSET, 32, e.result);
            put(e.event, RD_WRITE_MASK_OFFSET, 32, UINT32_MAX);
        }
        return e;
    }
    unsigned run(const Input& in = {}) {
        ++cycles;
        const bool active = !in.reset && !in.flush;
        unsigned busy = 0, supported = 0, cfi = 0, cp_mask = 0;
        for (unsigned lane = 0; lane < 2; ++lane) {
            int op = operation(in.insn[lane]);
            if (op >= 0 && !(in.pc[lane]&3)) supported |= 1U << lane;
            if (op >= 21) cfi |= 1U << lane;
            if (active && held[lane] >= 0) busy |= 1U << lane;
        }
        int free_cp = -1;
        for (int n = 7; n >= 0; --n) {
            if (checkpoints[unsigned(n)] >= 0) cp_mask |= 1U << n;
            else free_cp = n;
        }
        unsigned resolver = locate(held[0]);
        bool resolve_valid = (busy&1) && queue[resolver].cfi() && !queue[resolver].fault && !queue[resolver].resolved;
        bool resolve = resolve_valid && in.grant;
        bool redirect = resolve && queue[resolver].miss;
        bool cancel = redirect && (busy&2) && locate(held[1]) > resolver;
        unsigned pending = busy & 2;
        if ((busy&1) && (!queue[resolver].cfi() || queue[resolver].fault || queue[resolver].resolved)) pending |= 1;
        if (cancel) { busy &= ~2U; pending &= ~2U; }
        unsigned complete = pending & in.complete, wb = 0;
        for (unsigned lane = 0; lane < 2; ++lane) if (complete & (1U << lane)) {
            const auto& e = queue[locate(held[lane])];
            if (e.rd && !e.fault) wb |= 1U << lane;
        }
        auto retirement_ready = [](const Entry& e) { return e.done && !e.fault && (!e.cfi() || e.resolved); };
        unsigned rv = 0;
        if (active && !redirect && !queue.empty() && retirement_ready(queue[0])) {
            rv = 1;
            if (queue.size() > 1 && retirement_ready(queue[1])) rv = 3;
        }
        unsigned retire = (rv & in.retire & 1) ? rv & in.retire : 0;
        unsigned wanted = (1U << in.count)-1;
        unsigned selected = (cfi&1) ? wanted&1 : wanted;
        unsigned count = unsigned(__builtin_popcount(selected));
        unsigned alloc = active && !redirect && !in.drain && !(selected & ~supported)
            && queue.size()+count <= 32 && resident()+count <= 16
            && (!(selected&cfi) || free_cp >= 0) ? selected : 0;
        for (unsigned lane = 0; lane < count; ++lane) if (uses[(tail+lane)%32] >= 256) alloc = 0;
        std::array<int, 2> pick{-1,-1};
        for (unsigned port = 0; port < 2; ++port) {
            if (!active || redirect || !(in.execute & (1U << port)) || (held[port] >= 0 && !(complete & (1U << port)))) continue;
            for (unsigned n = 0; n < queue.size(); ++n) {
                if (!queue[n].queued || (port && (pick[0] == int(n) || queue[n].cfi()))) continue;
                bool ready = true;
                for (auto dep : queue[n].dependency) {
                    unsigned index = locate(dep);
                    if (index < queue.size() && (!queue[index].done || queue[index].fault)) {
                        bool bypass = false;
                        for (unsigned lane = 0; lane < 2; ++lane) bypass |= (wb & (1U << lane)) && held[lane] == dep;
                        ready &= bypass;
                    }
                }
                if (ready) { pick[port] = int(n); break; }
            }
        }
        dut.trap_ready_i = 0;
        dut.clk_i = 0; dut.rst_i = in.reset; dut.flush_i = in.flush; dut.drained_i = in.drain;
        dut.frontend_fault_i = 0; dut.frontend_cause_i = 0; dut.frontend_value_i = 0;
        dut.valid_i = wanted; dut.instruction_i = uint64_t(in.insn[0]) | (uint64_t(in.insn[1]) << 32);
        dut.pc_i = uint64_t(in.pc[0]) | (uint64_t(in.pc[1]) << 32);
        dut.predicted_pc_i = uint64_t(in.predicted_pc[0]) | (uint64_t(in.predicted_pc[1]) << 32);
        dut.predicted_taken_i = in.predicted_taken; dut.resolve_grant_i = in.grant;
        dut.execution_ready_i = in.execute; dut.completion_enable_i = in.complete; dut.retire_ready_i = in.retire;
        dut.eval();
        require(dut.supported_o == supported, "supported instruction mask");
        require(dut.allocate_accept_o == alloc, "allocation");
        require(dut.occupancy_o == queue.size() && dut.issue_occupancy_o == resident(), "occupancy");
        require(dut.producer_busy_o == busy, "held producer capacity/cancellation");
        require(dut.completion_valid_o == pending && dut.completion_accept_o == complete && dut.wb_accept_o == wb, "completion control");
        require(dut.resolve_valid_o == resolve_valid && dut.resolve_accept_o == resolve && dut.redirect_o == redirect, "resolution control");
        require(dut.retire_valid_o == rv && dut.retire_accept_o == retire, "retirement control");
        require(bool(dut.fault_pending_o) == (active && head_fault()), "head fault");
        require(bool(dut.identity_drain_o) == exhausted(), "identity exhaustion");
        require(dut.checkpoint_valid_o == cp_mask, "checkpoint ownership");
        if (active && !queue.empty()) require(dut.head_id_o == queue.front().id, "head identity");
        if (resolve_valid) {
            const auto& e = queue[resolver];
            require(dut.resolve_id_o == e.id && dut.resolve_pc_o == e.next_pc
                && dut.resolve_taken_o == e.taken && dut.resolve_mispredict_o == e.miss, "held resolution metadata");
            coverage["resolution_stall"] += !resolve;
            coverage["resolution_blocks_completion"] += (in.complete&1) != 0;
        }
        if (active && head_fault()) {
            Event e = queue.front().event; put(e, ORDER_OFFSET, 64, order);
            check_event(dut.fault_event_o, 0, e); ++coverage["head_fault"];
        }
        for (unsigned lane = 0; lane < 2; ++lane) {
            if (alloc & (1U << lane)) {
                unsigned slot = (tail+lane)%32;
                require(((dut.allocate_id_o >> (13*lane))&8191) == uses[slot]*32+slot, "allocation identity");
            }
            if (busy & (1U << lane)) {
                require(((dut.completion_id_o >> (13*lane))&8191) == unsigned(held[lane]), "held identity");
                check_event(dut.completion_event_o, lane, queue[locate(held[lane])].event);
            }
            require(bool(dut.issue_o & (1U << lane)) == (pick[lane] >= 0), "issue availability");
            if (pick[lane] >= 0) {
                const auto& e = queue[unsigned(pick[lane])];
                require(((dut.issue_id_o >> (13*lane))&8191) == e.id, "issue order/identity");
                for (unsigned n = 0; n < unsigned(pick[lane]); ++n) if (queue[n].queued && int(n) != pick[0]) { ++coverage["out_of_order_issue"]; break; }
                for (auto dep : e.dependency) for (unsigned w = 0; w < 2; ++w)
                    if ((wb & (1U << w)) && held[w] == dep) ++coverage["wakeup_bypass"];
                coverage["consume_refill"] += (complete & (1U << lane)) != 0;
                coverage["branch_launch"] += e.cfi();
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
        coverage["queue_full"] += resident() == 16; coverage["rob_full"] += queue.size() == 32;
        coverage["checkpoint_full"] += cp_mask == 255;
        coverage["unsupported"] += active && (selected & ~supported);
        coverage["cfi_prefix"] += wanted == 3 && alloc == 1 && (cfi&1);
        coverage["ignored_tail_unsupported"] += alloc == 1 && wanted == 3 && !(supported&2);
        coverage["flush_held"] += in.flush && (held[0] >= 0 || held[1] >= 0);
        coverage["reset_held"] += in.reset && (held[0] >= 0 || held[1] >= 0);
        coverage["drain"] += in.drain;
        coverage["generation_stall"] += active && wanted && uses[tail] == 256 && !alloc;
        coverage["wrapped_window"] += !queue.empty() && tail < queue.front().id%32;
        if (in.reset) {
            queue.clear(); held = {-1,-1}; checkpoints.fill(-1); checkpoint_uses.fill(0);
            uses.fill(0); committed.fill(0); known.fill(false); known[0] = true; order = 0; tail = 0;
        } else if (in.flush) { queue.clear(); held = {-1,-1}; checkpoints.fill(-1); tail = 0; }
        else {
            if (resolve) {
                auto& e = queue[resolver]; e.resolved = true;
                ++coverage[e.miss ? "prediction_wrong" : "prediction_right"];
                ++coverage[e.taken ? "taken" : "not_taken"];
                coverage["resolve_completion_stall"] += !(in.complete&1);
                coverage["resolve_allocate"] += alloc != 0;
                coverage["nested_resolve"] += __builtin_popcount(cp_mask) > 1;
                for (auto& owner : checkpoints) if (owner == int(e.id) || (redirect && owner >= 0 && locate(owner) > resolver)) owner = -1;
                if (redirect) {
                    coverage["wrapped_recovery"] += e.id%32 < queue.front().id%32;
                    for (unsigned n = resolver+1; n < queue.size(); ++n) {
                        coverage["kill_queued"] += queue[n].queued;
                        coverage["kill_completed"] += queue[n].done;
                        coverage["kill_resolved_branch"] += queue[n].cfi() && queue[n].resolved;
                        coverage["kill_fault"] += queue[n].fault && queue[n].done;
                    }
                    coverage["kill_held"] += cancel;
                    coverage["surviving_held"] += (busy&2) != 0;
                    coverage["surviving_wb"] += (wb&2) != 0;
                    if (cancel) held[1] = -1;
                    queue.resize(resolver+1); tail = (e.id%32+1)%32;
                }
            }
            for (unsigned port = 0; port < 2; ++port) {
                if (complete & (1U << port)) {
                    unsigned n = locate(held[port]); auto& e = queue[n]; e.done = true;
                    for (unsigned older = 0; older < n; ++older) if (!queue[older].done) { ++coverage["out_of_order_complete"]; break; }
                    coverage["resultless"] += e.rd == 0;
                    if (e.fault) { ++coverage["fault_complete"]; ++coverage["fault_op_"+std::to_string(e.op)]; }
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
                auto e = build(in.insn[lane],in.pc[lane],uses[tail]*32+tail,
                               (in.predicted_taken & (1U << lane)) != 0,in.predicted_pc[lane]);
                if (lane && queue.back().rd) {
                    coverage["bundle_raw"] += e.dependency[0] == int(queue.back().id) || e.dependency[1] == int(queue.back().id);
                    coverage["bundle_waw"] += e.rd == queue.back().rd;
                }
                if (e.cfi()) {
                    require(free_cp >= 0, "reference checkpoint capacity");
                    unsigned slot = unsigned(free_cp); checkpoints[slot] = int(e.id);
                    coverage["checkpoint_reuse"] += checkpoint_uses[slot]++ > 0;
                    coverage["cfi_lane1"] += lane == 1;
                    coverage["direction_only_prediction"] += e.miss && in.predicted_pc[lane] == e.next_pc;
                    coverage["jalr_lsb_cleared"] += e.cleared_lsb;
                }
                queue.push_back(e); ++uses[tail]; tail = (tail+1)%32;
            }
            if (in.drain) { require(queue.empty() && held[0] < 0 && held[1] < 0, "invalid stimulus drain"); uses.fill(0); }
        }
        dut.clk_i = 1; dut.eval(); dut.clk_i = 0; dut.eval();
        return alloc;
    }
    void drain() {
        for (unsigned n = 0; !queue.empty(); ++n) {
            require(n < 300, "progress watchdog");
            Input i; i.flush = head_fault(); run(i);
        }
        run();
    }
    unsigned send(uint32_t a, uint32_t b = 0x13, unsigned count = 2, uint32_t pc = 0x80000000U) {
        Input i; i.count = count; i.insn = {a,b}; i.pc = {pc,pc+4}; i.predicted_pc = {pc+4,pc+8};
        for (unsigned n = 0;; ++n) { unsigned accepted = run(i); if (accepted) return accepted; require(n < 300, "dispatch progress watchdog"); }
    }
    void initialize() {
        for (unsigned r = 1; r < 32; r += 2)
            send(instruction(0,r,0,0,r*0x12345000U),instruction(0,(r+1)%32,0,0,(r+1)*0xabcde000U),r == 31 ? 1 : 2);
        drain();
    }
    void constant(unsigned reg, uint32_t v) {
        send(instruction(0,reg,0,0,v+0x800U),instruction(2,reg,reg,0,v)); drain();
    }
};
void cf_case(Check& c, uint32_t word, uint32_t pc, unsigned prediction) {
    auto e = c.build(word,pc,0,false,pc+4);
    Input i; i.count = 2; i.insn = {word,0}; i.pc = {pc,pc+4};
    i.predicted_pc[0] = prediction == 1 ? e.next_pc^4U : e.next_pc;
    i.predicted_taken = unsigned(prediction == 2 ? !e.taken : e.taken);
    c.require(c.run(i) == 1, "directed CFI prefix");
    i.count = 0; i.grant = false; i.predicted_pc = {0xdeadbeef,0xbaadf00d}; i.predicted_taken ^= 3;
    for (unsigned n = 0; n < 4; ++n) c.run(i);
    i.grant = true; i.complete = 0;
    for (unsigned n = 0; n < 3; ++n) c.run(i);
    c.drain();
}
void nested(Check& c, bool fault) {
    c.send(instruction(2,10,0,0,1),0x13,1);
    Input i; i.execute = 2; i.complete = 0; i.grant = false; c.run(i);
    i.execute = 1; i.count = 1; i.insn[0] = instruction(22,0,10,0,16); c.run(i);
    i.insn[0] = instruction(27,16,0,0,fault ? 2 : 16); c.run(i);
    i.count = 0; c.run(i);
    i.grant = true; i.complete = 1; c.run(i); c.run(i);
    i.complete = 2; c.run(i);
    i.complete = 0; c.run(i); c.drain();
    c.send(instruction(2,17,16,0,0)); c.drain();
}
void directed(Check& c) {
    Input i; i.reset = true; c.run(i); c.initialize();
    const uint32_t values[] = {0,1,UINT32_MAX,0x80000000U,0x7fffffffU,0x55555555U,0xaaaaaaaaU,31,32,63};
    for (unsigned n = 0; n < 10; ++n) {
        c.constant(1,values[n]); c.constant(2,values[9-n]);
        for (unsigned op = 0; op < 21; ++op) {
            c.send(instruction(op,3,1,2,values[n]),instruction(op,4,2,1,values[9-n]),2,n == 0 ? 0xfffffffcU : 0x1000U);
            c.drain();
        }
        for (unsigned op = 21; op <= 26; ++op) for (unsigned mode = 0; mode < 3; ++mode)
            cf_case(c,instruction(op,0,1,2,mode == 0 ? 16 : mode == 1 ? uint32_t(-16) : 4),0x1000,mode);
    }
    for (uint32_t off : {0U,4U,uint32_t(-4),1048572U,uint32_t(-1048576),2U})
        cf_case(c,instruction(27,3,0,0,off),0xfffffffcU,1);
    c.constant(1,0x4000);
    for (uint32_t off : {0U,1U,3U,2045U,2047U,uint32_t(-2048),uint32_t(-1)})
        cf_case(c,instruction(28,3,1,0,off),0x1000,0);
    c.constant(1,0x101);
    cf_case(c,instruction(28,1,1,0,0),0x1000,1);
    cf_case(c,instruction(21,0,0,0,2),0x1000,0);
    cf_case(c,instruction(22,0,0,0,2),0x1000,0);
    c.send(instruction(2,5,0,0,0x140),instruction(28,5,5,0,1)); c.drain();
    i = {}; i.count = 2; i.pc = {0x1000,0x1004};
    i.insn = {instruction(2,5,0,0,0x140),instruction(28,5,5,0,1)};
    i.predicted_taken = 2; i.predicted_pc[1] = 0x140; c.run(i); c.drain();

    // Resolve a branch while a younger ALU result is held and another younger entry remains queued.
    c.send(instruction(21,0,0,0,16),0x13,1,0x1000);
    i = {}; i.grant = false; i.complete = 0; i.count = 2;
    i.insn = {instruction(2,6,0,0,66),instruction(2,7,0,0,77)}; c.run(i);
    i.count = 0; i.execute = 2; c.run(i); c.run(i);
    i.grant = true; i.complete = 3; c.run(i); c.drain();
    c.send(instruction(2,8,6,0,1),instruction(2,9,7,0,1)); c.drain();

    // An older held result survives the same recovery and may write back in its winning cycle.
    c.send(instruction(2,10,0,0,111),0x13,1);
    i = {}; i.execute = 2; i.complete = 0; c.run(i);
    i.execute = 1; i.grant = false; i.count = 1; i.insn[0] = instruction(21,0,0,0,16); c.run(i);
    i.count = 0; c.run(i);
    i.grant = true; i.complete = 2; c.run(i); c.drain();
    nested(c,false); nested(c,true);
    // A correct resolution can free a checkpoint while a different checkpoint is allocated.
    c.send(instruction(22,0,0,0,16),0x13,1,0);
    i = {}; i.grant = false; c.run(i);
    i.count = 1; i.insn[0] = instruction(27,0,0,0,4); i.predicted_taken = 1; i.grant = true;
    c.run(i); c.drain();

    i = {}; i.execute = 0; i.count = 1; i.insn[0] = instruction(27,0,0,0,4); i.predicted_taken = 1;
    for (unsigned n = 0; n < 10; ++n) c.run(i);
    c.drain();
    i = {}; i.execute = 0; i.count = 2;
    for (unsigned n = 0; n < 10; ++n) c.run(i);
    c.drain();
    i = {}; i.retire = 0; i.count = 2;
    for (unsigned n = 0; n < 22; ++n) c.run(i);
    c.drain();
    const uint32_t bad[] = {0,0x2003,0x2023,0x1073,0xf,0x02000033,0x02001013,0x40001033,0x2063,0x1067};
    for (auto word : bad) {
        i = {}; i.count = 2; i.insn = {word,0x13}; c.run(i);
        i.insn = {0x13,word}; c.run(i);
    }
    i = {}; i.count = 2; i.pc[1] = 6; c.run(i);

    // Compare age across the ring boundary in both cancellation directions.
    for (unsigned older = 0; older < 2; ++older) {
        i = {}; i.reset = true; c.run(i);
        for (unsigned n = 0; n < (older ? 31U : 30U); ++n) c.send(0x13,0x13,1);
        c.drain();
        c.send(0x13,0x13,1);
        if (older) {
            i = {}; i.execute = 2; i.complete = 0; c.run(i);
            i.count = 1; i.insn[0] = instruction(21,0,0,0,16); i.execute = 1; i.grant = false; c.run(i);
            i.count = 0; c.run(i); i.grant = true; i.complete = 2; c.run(i);
        } else {
            i = {}; i.retire = 0; i.grant = false; i.count = 1;
            i.insn[0] = instruction(21,0,0,0,16); c.run(i);
            i.insn[0] = instruction(2,6,0,0,66); i.complete = 1; c.run(i);
            i.count = 0; i.execute = 2; i.complete = 0; c.run(i);
            i.grant = true; i.complete = 3; c.run(i);
        }
        c.drain();
    }
    for (unsigned reset = 0; reset < 2; ++reset) {
        c.send(instruction(27,4,0,0,16),0x13,1);
        i = {}; i.grant = false; c.run(i); c.run(i);
        i.flush = !reset; i.reset = reset; c.run(i); c.run();
    }
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
            std::string name = argc > 2 ? argv[2] : "";
            if (name == "unresolved_drain") {
                c.send(instruction(27,0,0,0,16),0x13,1); i = {}; i.grant = false; c.run(i);
            }
            c.dut.clk_i = 0; c.dut.rst_i = 0; c.dut.valid_i = name == "prefix" ? 2 : 0;
            c.dut.drained_i = name == "unresolved_drain"; c.dut.eval(); c.dut.clk_i = 1; c.dut.eval();
            throw std::runtime_error("caller assertion did not reject "+name);
        }
        unsigned seed = argc > 1 ? unsigned(std::stoul(argv[1])) : 1;
        unsigned random_cycles = argc > 2 ? unsigned(std::stoul(argv[2])) : 20000;
        directed(c); unsigned directed_cycles = c.cycles;
        std::mt19937 rng(seed);
        for (unsigned n = 0; n < random_cycles; ++n) {
            Input i; i.count = rng()%3; i.execute = rng()%4; i.complete = rng()%4; i.retire = rng()%4; i.grant = rng()%2;
            for (unsigned lane = 0; lane < 2; ++lane) {
                i.insn[lane] = instruction(rng()%29,rng()%32,rng()%32,rng()%32,rng());
                i.pc[lane] = rng()&~3U;
                auto e = c.build(i.insn[lane],i.pc[lane],0,false,0);
                i.predicted_pc[lane] = rng()%2 ? e.next_pc : i.pc[lane]+4;
                i.predicted_taken |= unsigned(rng()%2 ? e.taken : !e.taken) << lane;
            }
            i.flush = rng()%233 == 0 || (c.head_fault() && rng()%3 == 0);
            if (c.exhausted()) { i = {}; i.flush = c.head_fault(); i.drain = c.queue.empty(); }
            c.run(i);
        }
        unsigned random_end = c.cycles; c.drain();
        std::cout << "CONTROL FLOW BACKEND PASS seed=" << seed << " directed=" << directed_cycles
                  << " random=" << random_cycles << " final_drain=" << c.cycles-random_end << " cycles=" << c.cycles;
        for (const auto& [key,value] : c.coverage) std::cout << " " << key << "=" << value;
        std::cout << '\n';
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
