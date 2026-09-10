#include "Vrename_state.h"
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

struct Entry { unsigned rd, destination, stale; bool done; };
struct Input {
    std::array<unsigned, 2> rs1{}, rs2{}, rd{};
    unsigned valid = 0, checkpoint = 0, commits = 0;
    std::vector<unsigned> wb;
    bool resources = true, reset = false, recover = false, wb_lane1 = false, wb_zero = false;
};
struct Output {
    unsigned accept = 0, source1 = 0, source2 = 0, destination = 0, stale = 0, ready = 0;
    uint64_t allocations = 0;
    std::vector<Entry> entries;
};

class Check {
    Vrename_state dut;
    std::array<unsigned, 32> committed{};
    std::set<unsigned> used_tags;
public:
    std::deque<Entry> pending;
    unsigned cycles = 0;
    std::map<std::string, unsigned> coverage;
    Check() { std::iota(committed.begin(), committed.end(), 0); }
    std::array<unsigned, 32> speculative() const {
        auto map = committed;
        for (const auto& entry : pending) if (entry.rd) map[entry.rd] = entry.destination;
        return map;
    }
    // Reconstruct ownership from the committed map and surviving instruction ledger.
    std::set<unsigned> available() const {
        std::set<unsigned> pool;
        for (unsigned p = 1; p < 64; ++p) pool.insert(p);
        for (auto p : committed) pool.erase(p);
        for (const auto& entry : pending) if (entry.rd) pool.erase(entry.destination);
        return pool;
    }
    uint64_t readiness() const {
        uint64_t bits = 0;
        for (auto p : committed) bits |= UINT64_C(1) << p;
        for (const auto& entry : pending)
            if (entry.rd && entry.done) bits |= UINT64_C(1) << entry.destination;
        return bits;
    }
    void require(bool condition, const std::string& detail) const {
        if (!condition) throw std::runtime_error("rename state mismatch cycle=" + std::to_string(cycles) + " " + detail);
    }
    template <class Packed> static unsigned tag(const Packed& map, unsigned arch) {
        unsigned value = 0;
        for (unsigned bit = 0; bit < 6; ++bit) {
            const auto offset = arch*6+bit;
            value |= ((map[offset/32] >> (offset%32)) & 1U) << bit;
        }
        return value;
    }
    Output run(const Input& in) {
        ++cycles;
        auto pool = available();
        auto map = speculative();
        auto ready = readiness();
        for (auto index : in.wb) {
            require(index < pending.size() && pending[index].rd && !pending[index].done, "bad test WB");
            ready |= UINT64_C(1) << pending[index].destination;
        }
        require(in.commits <= 2 && in.commits <= pending.size(), "bad test retirement");
        dut.clk_i = 0; dut.rst_i = in.reset; dut.recover_i = in.recover;
        dut.valid_i = in.valid; dut.checkpoint_i = in.checkpoint;
        dut.resources_ready_i = in.resources;
        dut.rs1_i = in.rs1[0] | in.rs1[1] << 5;
        dut.rs2_i = in.rs2[0] | in.rs2[1] << 5;
        dut.rd_i = in.rd[0] | in.rd[1] << 5;
        dut.wb_accept_i = 0; dut.wb_destination_i = 0;
        for (unsigned slot = 0; slot < in.wb.size(); ++slot) {
            const unsigned lane = in.wb_lane1 ? 1 : slot;
            dut.wb_accept_i |= 1U << lane;
            dut.wb_destination_i |= pending[in.wb[slot]].destination << (6*lane);
        }
        if (in.wb_zero) { dut.wb_accept_i = 3; dut.wb_destination_i = 0; }
        dut.commit_i = (1U << in.commits)-1;
        dut.commit_rd_i = 0; dut.commit_destination_i = 0; dut.commit_stale_i = 0;
        for (unsigned lane = 0; lane < in.commits; ++lane) {
            const auto& entry = pending[lane];
            require(in.reset || in.recover || !entry.rd || ((ready >> entry.destination) & 1), "test retired unready");
            dut.commit_rd_i |= entry.rd << (5*lane);
            dut.commit_destination_i |= entry.destination << (6*lane);
            dut.commit_stale_i |= entry.stale << (6*lane);
        }
        Output out;
        unsigned count = in.valid == 0 ? 0 : (in.valid == 1 || (in.checkpoint & 1) ? 1 : 2);
        unsigned needed = 0;
        for (unsigned lane = 0; lane < count; ++lane) needed += in.rd[lane] != 0;
        if (!in.reset && !in.recover && in.resources && in.valid != 2 && pool.size() >= needed) {
            for (unsigned lane = 0; lane < count; ++lane) {
                const auto a = map[in.rs1[lane]], b = map[in.rs2[lane]];
                out.accept |= 1U << lane;
                out.source1 |= a << (6*lane); out.source2 |= b << (6*lane);
                out.ready |= unsigned((ready >> a) & 1) << (2*lane);
                out.ready |= unsigned((ready >> b) & 1) << (2*lane+1);
                Entry entry{in.rd[lane], 0, 0, true};
                if (entry.rd) {
                    entry.destination = *pool.begin(); pool.erase(pool.begin());
                    entry.stale = map[entry.rd]; entry.done = false;
                    map[entry.rd] = entry.destination;
                    ready &= ~(UINT64_C(1) << entry.destination);
                    out.destination |= entry.destination << (6*lane);
                    out.stale |= entry.stale << (6*lane);
                    out.allocations |= UINT64_C(1) << entry.destination;
                }
                out.entries.push_back(entry);
            }
        }
        dut.eval();
        require(dut.accept_o == out.accept && dut.source1_o == out.source1 && dut.source2_o == out.source2
                && dut.destination_o == out.destination && dut.stale_o == out.stale
                && dut.source_ready_o == out.ready && dut.allocation_o == out.allocations, "planner outputs");
        coverage["wb_lane1"] += in.wb_lane1 && !in.wb.empty();
        coverage["wb_zero"] += in.wb_zero;
        coverage["dual_rename"] += out.accept == 3;
        coverage["rename_waw"] += out.accept == 3 && in.rd[0] && in.rd[0] == in.rd[1];
        coverage["zero_destination"] += out.accept && (!in.rd[0] || (out.accept == 3 && !in.rd[1]));
        coverage["checkpoint_cut"] += in.valid == 3 && out.accept == 1 && (in.checkpoint & 1);
        coverage["exhausted"] += in.valid && in.resources && !in.reset && !in.recover && available().size() < needed;
        coverage["commit_rename"] += out.accept && in.commits;
        coverage["deferred_reuse"] += in.commits && in.valid && !out.accept && available().size() < needed && !in.reset && !in.recover;
        coverage["recover_collision"] += in.recover && !in.reset && in.commits && !in.wb.empty() && in.valid;
        coverage["reset_live"] += in.reset && !pending.empty();
        coverage["recovery"] += in.recover && !in.reset;
        coverage["commit_waw"] += !in.reset && !in.recover && in.commits == 2 && pending[0].rd && pending[0].rd == pending[1].rd;
        coverage["ooo_wb"] += !in.wb.empty() && in.wb.front() > 0 && !pending[0].done;
        coverage["blocked_wb"] += !in.resources && !in.wb.empty() && !in.reset && !in.recover;
        for (auto index : in.wb) {
            coverage["wb_commit"] += !in.reset && !in.recover && index < in.commits;
            if (out.accept) {
                const auto p = pending[index].destination;
                for (unsigned lane = 0; lane < out.entries.size(); ++lane)
                    coverage["wb_bypass"] += ((out.source1 >> (6*lane)) & 63) == p || ((out.source2 >> (6*lane)) & 63) == p;
            }
        }
        if (in.reset) {
            pending.clear(); std::iota(committed.begin(), committed.end(), 0);
        } else if (in.recover) {
            pending.clear();
        } else {
            for (auto index : in.wb) pending[index].done = true;
            for (unsigned n = 0; n < in.commits; ++n) {
                const auto entry = pending.front(); pending.pop_front();
                if (entry.rd) {
                    require(committed[entry.rd] == entry.stale, "ledger stale chain");
                    committed[entry.rd] = entry.destination;
                }
            }
            for (const auto& entry : out.entries) {
                if (entry.rd) {
                    coverage["tag_reuse"] += used_tags.count(entry.destination);
                    used_tags.insert(entry.destination);
                }
                pending.push_back(entry);
            }
        }
        dut.clk_i = 1; dut.eval();
        map = speculative();
        uint64_t free = 0;
        for (auto p : available()) free |= UINT64_C(1) << p;
        require(dut.free_o == free && dut.ready_o == readiness(), "physical ownership/readiness");
        for (unsigned arch = 0; arch < 32; ++arch)
            require(tag(dut.rat_o, arch) == map[arch] && tag(dut.committed_o, arch) == committed[arch], "map arch=" + std::to_string(arch));
        dut.clk_i = 0; dut.eval();
        return out;
    }
    void reset() { Input in; in.reset = true; run(in); }
    void negative(const std::string& mode) {
        reset(); Input in; in.valid = 3; in.rd = {5, 5}; run(in);
        dut.clk_i = 0; dut.valid_i = 0;
        if (mode == "free_wb") { dut.wb_accept_i = 1; dut.wb_destination_i = 50; }
        else if (mode == "duplicate_wb") { dut.wb_accept_i = 3; dut.wb_destination_i = 32 | 32 << 6; }
        else if (mode == "unready_commit") {
            dut.commit_i = 1; dut.commit_rd_i = 5; dut.commit_destination_i = 32; dut.commit_stale_i = 5;
        } else if (mode == "commit_prefix") { dut.commit_i = 2; }
        else if (mode == "wrong_stale") {
            dut.wb_accept_i = 1; dut.wb_destination_i = 32;
            dut.commit_i = 1; dut.commit_rd_i = 5; dut.commit_destination_i = 32; dut.commit_stale_i = 6;
        } else if (mode == "zero_metadata") { dut.commit_i = 1; dut.commit_destination_i = 32; }
        else throw std::runtime_error("unknown negative case");
        dut.eval(); dut.clk_i = 1; dut.eval();
        throw std::runtime_error("negative stimulus escaped assertions");
    }
};

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    try {
        Check check;
        if (argc > 1 && std::string(argv[1]) == "negative") {
            check.negative(argv[2]); return 1;
        }
        const unsigned seed = argc > 1 ? std::stoul(argv[1]) : 1;
        const unsigned random_cycles = argc > 2 ? std::stoul(argv[2]) : 20000;
        std::mt19937 rng(seed);
        check.reset();
        Input p0; p0.wb_zero = true; check.run(p0);
        Input pair; pair.valid = 3; pair.rd = {5, 5}; pair.rs1 = {5, 5}; pair.rs2 = {0, 5};
        check.run(pair);
        Input overlap = pair; overlap.wb = {0, 1}; overlap.commits = 2; overlap.rd = {5, 6};
        check.run(overlap);
        Input collision = pair; collision.recover = true; collision.wb = {0, 1}; collision.commits = 2;
        check.run(collision);
        check.run(pair); collision.reset = true; check.run(collision);
        pair.rd = {1, 1}; pair.rs1 = {1, 1}; pair.rs2 = {1, 1};
        for (unsigned n = 0; n < 16; ++n) check.run(pair);
        overlap = pair; overlap.wb = {0, 1}; overlap.commits = 2;
        check.run(overlap); check.run(pair);
        Input clear; clear.recover = true; check.run(clear);
        pair.rd = {2, 3}; check.run(pair);
        Input late; late.resources = false; late.wb = {1}; late.wb_lane1 = true; check.run(late);
        Input cut = pair; cut.checkpoint = 1; check.run(cut);
        Input zero; zero.valid = 3; check.run(zero);
        check.run(clear);
        // Exhaustion, branch cuts and control suppression are exercised with live state.
        for (unsigned mask = 0; mask < 4; ++mask)
            for (unsigned cp = 0; cp < 4; ++cp)
                for (unsigned controls = 0; controls < 8; ++controls) {
                    Input in = pair; in.valid = mask; in.checkpoint = cp;
                    in.reset = controls & 1; in.recover = controls & 2; in.resources = controls & 4;
                    check.run(in);
                }
        for (unsigned n = 0; n < random_cycles; ++n) {
            Input in;
            in.valid = rng() % 4; in.checkpoint = rng() % 9 == 0 ? rng() % 4 : 0;
            in.reset = rng() % 503 == 0; in.recover = rng() % 71 == 0;
            in.resources = rng() % 5 != 0;
            for (unsigned lane = 0; lane < 2; ++lane) {
                in.rd[lane] = rng() % 32; in.rs1[lane] = rng() % 32; in.rs2[lane] = rng() % 32;
            }
            if (n % 3 == 0) in.rs1[1] = in.rd[0];
            if (n % 5 == 0) in.rd[1] = in.rd[0];
            std::vector<unsigned> waiting;
            for (unsigned j = 0; j < check.pending.size(); ++j)
                if (check.pending[j].rd && !check.pending[j].done) waiting.push_back(j);
            std::shuffle(waiting.begin(), waiting.end(), rng);
            const auto count = std::min<size_t>(rng() % 3, waiting.size());
            in.wb.assign(waiting.begin(), waiting.begin()+count);
            in.wb_lane1 = count == 1 && rng() % 2;
            for (unsigned j = 0; j < std::min<size_t>(2, check.pending.size()); ++j) {
                if (!check.pending[j].done && std::find(in.wb.begin(), in.wb.end(), j) == in.wb.end()) break;
                if (rng() % 5 == 0) break;
                ++in.commits;
            }
            const unsigned prefix = in.valid == 3 && !(in.checkpoint & 1) ? 2 : in.valid == 0 ? 0 : 1;
            in.resources = in.resources && check.pending.size()-in.commits+prefix <= 32;
            check.run(in);
        }
        check.run(clear);
        for (const auto* name : {"wb_lane1", "wb_zero", "dual_rename", "rename_waw", "zero_destination", "checkpoint_cut", "exhausted",
                                "commit_rename", "deferred_reuse", "recover_collision", "reset_live", "recovery",
                                "commit_waw", "ooo_wb", "blocked_wb", "wb_commit", "wb_bypass", "tag_reuse"})
            check.require(check.coverage[name] > 0, std::string("missing coverage ")+name);
        std::cout << "RENAME STATE PASS seed=" << seed << " cycles=" << check.cycles;
        for (const auto& [name, count] : check.coverage) std::cout << ' ' << name << '=' << count;
        std::cout << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
