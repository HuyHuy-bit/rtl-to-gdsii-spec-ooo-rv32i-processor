#include "Vrename_checkpoints.h"
#include "verilated.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <iostream>
#include <map>
#include <numeric>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using Map = std::array<unsigned, 32>;
struct Instruction { uint64_t sequence; unsigned rd, tag, stale; };
struct Branch { unsigned slot; uint64_t boundary; Map snapshot; };
struct Input {
    std::vector<unsigned> rd;
    bool create = false, resolve = false, mispredict = false, reset = false, flush = false;
    unsigned slot = 0, commits = 0;
    uint64_t ignored_allocation = 0;
};

class Check {
    Vrename_checkpoints dut;
    Map committed{};
    uint64_t sequence = 0;
    std::set<unsigned> seen_slots;
public:
    std::deque<Instruction> instructions;
    std::vector<Branch> branches;
    std::map<std::string, unsigned> coverage;
    unsigned cycles = 0, probes = 0;
    Check() { std::iota(committed.begin(), committed.end(), 0); }
    void require(bool ok, const std::string& what) const {
        if (!ok) throw std::runtime_error("rename checkpoints mismatch cycle="+std::to_string(cycles)+" "+what);
    }
    Map current() const {
        auto result = committed;
        for (const auto& insn : instructions) if (insn.rd) result[insn.rd] = insn.tag;
        return result;
    }
    std::set<unsigned> free_tags() const {
        std::set<unsigned> result;
        for (unsigned p = 1; p < 64; ++p) result.insert(p);
        for (auto p : committed) result.erase(p);
        for (const auto& insn : instructions) if (insn.rd) result.erase(insn.tag);
        return result;
    }
    unsigned valid() const {
        unsigned mask = 0;
        for (const auto& b : branches) mask |= 1U << b.slot;
        return mask;
    }
    unsigned committable() const {
        unsigned count = 0;
        for (const auto& insn : instructions) {
            if (count == 2 || (!branches.empty() && insn.sequence >= branches.front().boundary)) break;
            ++count;
        }
        return count;
    }
    // Branch order and live instruction sequence define expected recovery, independent of masks.
    uint64_t reclaim(const Branch& branch) const {
        uint64_t bits = 0;
        for (const auto& insn : instructions)
            if (insn.rd && insn.sequence > branch.boundary) bits |= UINT64_C(1) << insn.tag;
        return bits;
    }
    template<class Packed> static void pack(Packed& target, const Map& map) {
        for (unsigned word = 0; word < 6; ++word) target[word] = 0;
        for (unsigned arch = 0; arch < 32; ++arch)
            for (unsigned bit = 0; bit < 6; ++bit) {
                const unsigned offset = arch*6+bit;
                target[offset/32] |= ((map[arch] >> bit) & 1U) << (offset%32);
            }
    }
    template<class Packed> static Map unpack(const Packed& target) {
        Map result{};
        for (unsigned arch = 0; arch < 32; ++arch)
            for (unsigned bit = 0; bit < 6; ++bit) {
                const unsigned offset = arch*6+bit;
                result[arch] |= ((target[offset/32] >> (offset%32)) & 1U) << bit;
            }
        return result;
    }
    void check_restore(unsigned index) {
        unsigned released = 0;
        for (unsigned n = index; n < branches.size(); ++n) released |= 1U << branches[n].slot;
        require(dut.restore_valid_o && unpack(dut.restore_rat_o) == branches[index].snapshot
                && dut.reclaim_o == reclaim(branches[index]) && dut.released_o == released, "restore packet");
        for (auto p : branches[index].snapshot)
            require(!((dut.reclaim_o >> p) & 1), "reclaimed retained snapshot mapping");
    }
    void inspect() {
        dut.clk_i = 0; dut.rst_i = 0; dut.flush_i = 0;
        dut.create_i = 0; dut.allocation_i = 0;
        for (unsigned index = 0; index < branches.size(); ++index) {
            dut.resolve_i = 1; dut.mispredict_i = 1; dut.resolve_id_i = branches[index].slot;
            dut.eval(); check_restore(index); ++probes;
        }
        dut.resolve_i = 0; dut.mispredict_i = 0; dut.eval();
    }
    unsigned run(const Input& in) {
        ++cycles;
        require(in.commits <= committable(), "test commits across unresolved CFI");
        const unsigned before = valid();
        const bool recovery = in.resolve && in.mispredict;
        const bool ready = !in.reset && !in.flush && !recovery && before != 255;
        const bool accept = in.create && ready;
        unsigned candidate = 0;
        while (candidate < 8 && (before & (1U << candidate))) ++candidate;
        auto pool = free_tags();
        auto snapshot = current();
        std::vector<Instruction> added;
        uint64_t allocation = 0;
        if (!in.reset && !in.flush && !recovery && (!in.create || accept)) {
            for (auto rd : in.rd) {
                Instruction insn{++sequence, rd, 0, 0};
                if (rd) {
                    require(!pool.empty(), "test physical exhaustion");
                    insn.tag = *pool.begin(); pool.erase(pool.begin());
                    insn.stale = snapshot[rd]; snapshot[rd] = insn.tag;
                    allocation |= UINT64_C(1) << insn.tag;
                }
                added.push_back(insn);
            }
            require(!accept || !added.empty(), "test checkpoint without CFI");
        }
        dut.clk_i = 0; dut.rst_i = in.reset; dut.flush_i = in.flush;
        dut.create_i = in.create; pack(dut.snapshot_i, snapshot); dut.allocation_i = allocation | in.ignored_allocation;
        dut.resolve_i = in.resolve; dut.mispredict_i = in.mispredict; dut.resolve_id_i = in.slot;
        dut.eval();
        require(dut.valid_o == before && dut.create_ready_o == ready && dut.create_accept_o == accept
                && dut.create_id_o == (accept ? candidate : 0), "creation handshake");
        auto found = std::find_if(branches.begin(), branches.end(), [&](const Branch& b) { return b.slot == in.slot; });
        unsigned index = unsigned(found-branches.begin());
        if (!in.reset && !in.flush && in.resolve) {
            require(found != branches.end(), "test stale resolution");
            if (recovery) check_restore(index);
            else require(!dut.restore_valid_o && dut.reclaim_o == 0 && unpack(dut.restore_rat_o) == Map{}
                         && dut.released_o == (1U << in.slot), "correct resolution");
        } else require(!dut.restore_valid_o && dut.reclaim_o == 0 && unpack(dut.restore_rat_o) == Map{}
                       && dut.released_o == 0, "suppressed restore");
        coverage["suppressed_noise"] += in.ignored_allocation && (in.reset || in.flush || recovery);
        coverage["full_stall"] += before == 255 && in.create && !in.reset && !in.flush && !recovery;
        coverage["deferred_slot_reuse"] += before == 255 && in.create && in.resolve && !in.mispredict && !in.reset && !in.flush;
        coverage["reset_live"] += in.reset && before != 0;
        coverage["flush_live"] += in.flush && !in.reset && before != 0;
        coverage["resolve_create"] += accept && in.resolve;
        coverage["two_lane_cfi"] += accept && added.size() == 2;
        coverage["link_retained"] += accept && added.back().rd != 0;
        coverage["older_commit"] += in.commits && !branches.empty();
        coverage["recycled_old_tag"] += before != 0 && (allocation & UINT64_C(0xfffffffe)) != 0;
        coverage["full_ordinary_rename"] += before == 255 && allocation && !in.create;
        coverage["waw"] += added.size() == 2 && added[0].rd && added[0].rd == added[1].rd;
        if (in.reset || in.flush) {
            branches.clear(); instructions.clear();
            if (in.reset) std::iota(committed.begin(), committed.end(), 0);
        } else if (recovery) {
            coverage["recovery"]++;
            coverage["nested_recovery"] += index > 0;
            coverage["younger_kill"] += index+1 < branches.size();
            coverage["recovery_collision"] += in.create && !in.rd.empty();
            const auto boundary = found->boundary;
            while (!instructions.empty() && instructions.back().sequence > boundary) instructions.pop_back();
            branches.erase(branches.begin()+index, branches.end());
        } else {
            if (in.resolve) {
                coverage["out_of_order_resolve"] += index > 0;
                branches.erase(found);
            }
            for (unsigned n = 0; n < in.commits; ++n) {
                const auto insn = instructions.front(); instructions.pop_front();
                if (insn.rd) { require(committed[insn.rd] == insn.stale, "test stale chain"); committed[insn.rd] = insn.tag; }
            }
            instructions.insert(instructions.end(), added.begin(), added.end());
            if (accept) {
                coverage["slot_reuse"] += seen_slots.count(candidate); seen_slots.insert(candidate);
                branches.push_back({candidate, added.back().sequence, snapshot});
                coverage["created"]++;
            }
        }
        dut.clk_i = 1; dut.eval();
        require(dut.valid_o == valid(), "active checkpoints");
        inspect();
        return candidate;
    }
    void reset() { Input in; in.reset = true; run(in); }
    void negative(const std::string& name) {
        reset(); Input branch; branch.create = true; branch.rd = {0};
        if (name == "duplicate_allocation") {
            run(branch); Input write; write.rd = {1}; run(write);
            dut.allocation_i = UINT64_C(1) << 32;
        } else if (name == "stalled_allocation") {
            for (unsigned i = 0; i < 8; ++i) run(branch);
            dut.create_i = 1; dut.allocation_i = UINT64_C(1) << 32;
        } else if (name == "invalid_resolve") { dut.resolve_i = 1; dut.resolve_id_i = 7; }
        else if (name == "zero_allocation") { dut.allocation_i = 1; }
        else if (name == "map_alias" || name == "map_zero") {
            auto map = current(); map[2] = name == "map_alias" ? 1 : 0;
            pack(dut.snapshot_i, map); dut.create_i = 1;
        } else throw std::runtime_error("unknown negative stimulus");
        dut.eval(); dut.clk_i = 1; dut.eval();
        throw std::runtime_error("negative stimulus escaped assertions");
    }
};

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    try {
        Check check;
        if (argc > 1 && std::string(argv[1]) == "negative") { check.negative(argv[2]); return 1; }
        const unsigned seed = argc > 1 ? std::stoul(argv[1]) : 1;
        const unsigned count = argc > 2 ? std::stoul(argv[2]) : 20000;
        std::mt19937 rng(seed);
        check.reset();
        Input write; write.rd = {1}; check.run(write);
        Input branch; branch.create = true; branch.rd = {5}; const auto outer = check.run(branch);
        Input commit; commit.commits = 1; check.run(commit);
        write.rd = {6}; check.run(write);
        branch.rd = {7}; const auto inner = check.run(branch);
        write.rd = {5, 5}; check.run(write);
        Input recovery; recovery.resolve = true; recovery.mispredict = true; recovery.slot = inner;
        recovery.create = true; recovery.rd = {8}; recovery.ignored_allocation = ~UINT64_C(0); check.run(recovery);
        branch.rd = {8, 9}; const auto reused = check.run(branch);
        branch.resolve = true; branch.slot = outer; branch.rd = {10}; check.run(branch);
        branch.resolve = false; branch.rd = {0}; check.run(branch);
        recovery.slot = reused; check.run(recovery);
        Input flush; flush.flush = true; check.run(branch); check.run(flush);
        for (unsigned i = 0; i < 8; ++i) check.run(branch);
        write.rd = {11}; check.run(write);
        branch.resolve = true; branch.slot = 3; check.run(branch);
        branch.resolve = false; check.run(branch);
        recovery.slot = 5; check.run(recovery);
        check.reset();
        check.run(branch);
        Input collision = recovery; collision.slot = 0; collision.flush = true; check.run(collision);
        check.run(branch); collision.reset = true; check.run(collision);
        for (unsigned n = 0; n < count; ++n) {
            Input in;
            in.reset = rng()%499 == 0; in.flush = rng()%101 == 0;
            if (!check.branches.empty() && rng()%3 == 0) {
                in.resolve = true; in.slot = check.branches[rng()%check.branches.size()].slot;
                in.mispredict = rng()%3 == 0;
            }
            if (rng()%4 != 0) {
                in.rd = {unsigned(rng()%32)};
                if (rng()%2) in.rd.push_back(rng()%32);
                if (in.rd.size() == 2 && n%5 == 0) in.rd[1] = in.rd[0];
                in.create = rng()%3 == 0;
                const unsigned needed = std::count_if(in.rd.begin(), in.rd.end(), [](unsigned rd) { return rd != 0; });
                if (needed > check.free_tags().size() || check.instructions.size()+in.rd.size() > 32) {
                    in.rd.clear(); in.create = false;
                }
            }
            in.commits = rng()%(check.committable()+1);
            check.run(in);
        }
        check.run(flush);
        for (const auto* name : {"suppressed_noise", "full_stall", "deferred_slot_reuse", "reset_live", "flush_live", "resolve_create",
                                "two_lane_cfi", "link_retained", "older_commit", "recycled_old_tag", "full_ordinary_rename",
                                "waw", "recovery", "nested_recovery", "younger_kill", "recovery_collision", "out_of_order_resolve",
                                "slot_reuse", "created"})
            check.require(check.coverage[name] > 0, std::string("missing coverage ")+name);
        std::cout << "RENAME CHECKPOINTS PASS seed=" << seed << " cycles=" << check.cycles << " probes=" << check.probes;
        for (const auto& [name, total] : check.coverage) std::cout << ' ' << name << '=' << total;
        std::cout << '\n';
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
