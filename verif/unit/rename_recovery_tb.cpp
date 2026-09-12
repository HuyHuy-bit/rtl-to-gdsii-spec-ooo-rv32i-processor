#include "Vrename_recovery.h"
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
struct Entry { uint64_t sequence; unsigned rd, tag, stale; bool done; };
struct Branch { unsigned slot; uint64_t boundary; };
struct WB { unsigned tag = 0; bool offer = false, live = false; };
struct Input {
    std::array<unsigned, 2> rs1{}, rs2{}, rd{};
    std::array<WB, 2> wb{};
    unsigned valid = 0, checkpoint = 0, commits = 0, slot = 0;
    bool resources = true, reset = false, flush = false;
    bool resolve = false, resolve_live = true, mispredict = false;
};
struct Output {
    unsigned accept = 0, source1 = 0, source2 = 0, destination = 0, stale = 0, ready = 0;
    unsigned wb = 0, commit = 0, checkpoint_id = 0;
    uint64_t allocation = 0;
};
class Check {
    Vrename_recovery dut;
    Map committed{};
    uint64_t sequence = 0;
public:
    std::deque<Entry> pending;
    std::vector<Branch> branches;
    std::map<std::string, unsigned> coverage;
    unsigned cycles = 0;
    Check() { std::iota(committed.begin(), committed.end(), 0); }
    void require(bool ok, const std::string& message) const {
        if (!ok) throw std::runtime_error("rename recovery mismatch cycle="+std::to_string(cycles)+" "+message);
    }
    Map speculative() const {
        auto map = committed;
        for (const auto& e : pending) if (e.rd) map[e.rd] = e.tag;
        return map;
    }
    std::set<unsigned> available() const {
        std::set<unsigned> tags;
        for (unsigned tag = 1; tag < 64; ++tag) tags.insert(tag);
        for (auto tag : committed) tags.erase(tag);
        for (const auto& e : pending) if (e.rd) tags.erase(e.tag);
        return tags;
    }
    uint64_t readiness() const {
        uint64_t mask = 0;
        for (auto tag : committed) mask |= UINT64_C(1) << tag;
        for (const auto& e : pending) if (e.rd && e.done) mask |= UINT64_C(1) << e.tag;
        return mask;
    }
    unsigned valid() const {
        unsigned mask = 0;
        for (const auto& b : branches) mask |= 1U << b.slot;
        return mask;
    }
    WB result(unsigned index) const { return {pending.at(index).tag, true, true}; }
    unsigned committable(const Input& in) const {
        unsigned count = 0;
        for (const auto& e : pending) {
            if (count == 2 || (!branches.empty() && e.sequence >= branches.front().boundary)) break;
            bool ready = e.done;
            for (const auto& wb : in.wb) ready |= wb.offer && wb.live && wb.tag == e.tag;
            if (!ready) break;
            ++count;
        }
        return count;
    }
    template<class Packed> static unsigned tag(const Packed& packed, unsigned arch) {
        unsigned value = 0;
        for (unsigned bit = 0; bit < 6; ++bit) {
            const auto offset = arch*6+bit;
            value |= ((packed[offset/32] >> (offset%32)) & 1U) << bit;
        }
        return value;
    }
    // Rebuild ownership from committed state and surviving instructions, rather than RTL masks.
    Output run(const Input& in) {
        ++cycles;
        auto pool = available(); auto map = speculative(); auto ready = readiness();
        const unsigned before = valid();
        auto branch = std::find_if(branches.begin(), branches.end(), [&](const Branch& b) { return b.slot == in.slot; });
        const unsigned branch_index = unsigned(branch-branches.begin());
        const bool resolve = !in.reset && !in.flush && in.resolve && in.resolve_live && branch != branches.end();
        const bool recovery = resolve && in.mispredict;
        uint64_t reclaim = 0, boundary = recovery ? branch->boundary : 0;
        unsigned released = 0;
        if (resolve) {
            released = 1U << in.slot;
            if (recovery) {
                for (const auto& e : pending) if (e.rd && e.sequence > boundary) reclaim |= UINT64_C(1) << e.tag;
                for (unsigned n = branch_index; n < branches.size(); ++n) released |= 1U << branches[n].slot;
            }
        }
        Output out;
        for (unsigned lane = 0; lane < 2; ++lane) {
            const auto& wb = in.wb[lane];
            if (!in.reset && !in.flush && wb.offer && wb.live && wb.tag && !pool.count(wb.tag)
                && !((ready >> wb.tag) & 1) && !((reclaim >> wb.tag) & 1)) {
                out.wb |= 1U << lane;
                ready |= UINT64_C(1) << wb.tag;
            }
        }
        require(in.commits <= 2 && in.commits <= pending.size(), "bad test commit count");
        if (!in.reset && !in.flush && !recovery) {
            require(in.commits <= committable(in), "test retired unready or unresolved CFI");
            out.commit = (1U << in.commits)-1;
        }
        const unsigned count = in.valid == 0 ? 0 : in.valid == 1 || (in.checkpoint & 1) ? 1 : 2;
        const bool need_checkpoint = count && in.valid != 2 && (in.checkpoint & ((1U << count)-1));
        unsigned needed = 0;
        for (unsigned lane = 0; lane < count; ++lane) needed += in.rd[lane] != 0;
        std::vector<Entry> added;
        if (!in.reset && !in.flush && !recovery && in.resources && in.valid != 2
            && needed <= pool.size() && (!need_checkpoint || branches.size() < 8)) {
            for (unsigned lane = 0; lane < count; ++lane) {
                out.accept |= 1U << lane;
                const auto a = map[in.rs1[lane]], b = map[in.rs2[lane]];
                out.source1 |= a << (6*lane); out.source2 |= b << (6*lane);
                out.ready |= unsigned((ready >> a) & 1) << (2*lane);
                out.ready |= unsigned((ready >> b) & 1) << (2*lane+1);
                Entry e{++sequence, in.rd[lane], 0, 0, true};
                if (e.rd) {
                    e.tag = *pool.begin(); pool.erase(pool.begin());
                    e.stale = map[e.rd]; e.done = false; map[e.rd] = e.tag;
                    ready &= ~(UINT64_C(1) << e.tag);
                    out.destination |= e.tag << (6*lane); out.stale |= e.stale << (6*lane);
                    out.allocation |= UINT64_C(1) << e.tag;
                }
                added.push_back(e);
            }
        }
        const bool create = out.accept && need_checkpoint;
        if (create) while (before & (1U << out.checkpoint_id)) ++out.checkpoint_id;
        dut.clk_i = 0; dut.rst_i = in.reset; dut.recover_i = in.flush;
        dut.resources_ready_i = in.resources; dut.valid_i = in.valid; dut.checkpoint_i = in.checkpoint;
        dut.rs1_i = in.rs1[0] | in.rs1[1] << 5; dut.rs2_i = in.rs2[0] | in.rs2[1] << 5;
        dut.rd_i = in.rd[0] | in.rd[1] << 5;
        dut.resolve_i = in.resolve; dut.resolve_live_i = in.resolve_live; dut.mispredict_i = in.mispredict;
        dut.resolve_id_i = in.slot; dut.wb_offer_i = 0; dut.wb_live_i = 0; dut.wb_destination_i = 0;
        for (unsigned lane = 0; lane < 2; ++lane) {
            dut.wb_offer_i |= unsigned(in.wb[lane].offer) << lane;
            dut.wb_live_i |= unsigned(in.wb[lane].live) << lane;
            dut.wb_destination_i |= in.wb[lane].tag << (6*lane);
        }
        dut.commit_i = (1U << in.commits)-1; dut.commit_rd_i = 0; dut.commit_destination_i = 0; dut.commit_stale_i = 0;
        for (unsigned lane = 0; lane < in.commits; ++lane) {
            dut.commit_rd_i |= pending[lane].rd << (5*lane);
            dut.commit_destination_i |= pending[lane].tag << (6*lane);
            dut.commit_stale_i |= pending[lane].stale << (6*lane);
        }
        dut.eval();
        require(dut.accept_o == out.accept && dut.source1_o == out.source1 && dut.source2_o == out.source2
                && dut.destination_o == out.destination && dut.stale_o == out.stale && dut.source_ready_o == out.ready
                && dut.allocation_o == out.allocation, "rename packet");
        require(dut.wb_accept_o == out.wb && dut.commit_accept_o == out.commit, "final event acceptance");
        require(dut.resolve_accept_o == resolve && dut.branch_recover_o == recovery && dut.reclaim_o == reclaim
                && dut.checkpoint_released_o == released && dut.checkpoint_valid_o == before, "recovery packet");
        require(dut.checkpoint_accept_o == create && dut.checkpoint_id_o == out.checkpoint_id, "atomic checkpoint creation");
        coverage["resource_stall"] += in.valid && !in.resources && !in.reset && !in.flush && !recovery;
        coverage["physical_stall"] += in.valid && in.resources && needed > available().size() && !in.reset && !in.flush && !recovery;
        coverage["deferred_tag_reuse"] += in.commits && need_checkpoint && needed > available().size() && !in.reset && !in.flush && !recovery;
        coverage["flush_collision"] += in.flush && !in.reset && in.wb[0].offer && in.valid && in.commits;
        coverage["created"] += create;
        coverage["lane1_cfi"] += create && out.accept == 3;
        coverage["cut"] += create && in.valid == 3 && out.accept == 1;
        coverage["waw"] += out.accept == 3 && in.rd[0] && in.rd[0] == in.rd[1];
        coverage["full_stall"] += before == 255 && need_checkpoint && !in.reset && !in.flush && !recovery;
        coverage["full_non_cfi"] += before == 255 && out.accept && !create;
        coverage["recovery"] += recovery;
        coverage["nested"] += recovery && branch_index > 0;
        coverage["younger_kill"] += recovery && branch_index+1 < branches.size();
        coverage["commit_suppressed"] += recovery && in.commits;
        coverage["retained_wb"] += recovery && out.wb;
        coverage["resolve_create"] += resolve && !recovery && create;
        coverage["live_reset"] += in.reset && !pending.empty();
        coverage["full_flush"] += in.flush && !in.reset && !pending.empty();
        coverage["reject_resolve"] += in.resolve && !in.resolve_live && !in.reset && !in.flush;
        coverage["commit_rename"] += out.commit && out.accept;
        for (unsigned lane = 0; lane < 2; ++lane) {
            const auto& wb = in.wb[lane];
            coverage["reject_zero"] += wb.offer && wb.live && !wb.tag && !in.reset && !in.flush;
            coverage["reject_free"] += wb.offer && wb.live && available().count(wb.tag) && !in.reset && !in.flush;
            coverage["reject_ready"] += wb.offer && wb.live && wb.tag && ((readiness() >> wb.tag) & 1) && !in.reset && !in.flush;
            coverage["killed_wb"] += recovery && wb.offer && wb.live && ((reclaim >> wb.tag) & 1);
            coverage["reject_stale"] += wb.offer && !wb.live && wb.tag && !available().count(wb.tag)
                                         && !((readiness() >> wb.tag) & 1);
        }
        if (in.reset || in.flush) {
            pending.clear(); branches.clear();
            if (in.reset) std::iota(committed.begin(), committed.end(), 0);
        } else {
            for (auto& e : pending)
                for (unsigned lane = 0; lane < 2; ++lane)
                    if ((out.wb & (1U << lane)) && e.tag == in.wb[lane].tag) e.done = true;
            if (recovery) {
                while (!pending.empty() && pending.back().sequence > boundary) pending.pop_back();
                branches.erase(branches.begin()+branch_index, branches.end());
            } else {
                if (resolve) branches.erase(branch);
                for (unsigned n = 0; n < in.commits; ++n) {
                    const auto e = pending.front(); pending.pop_front();
                    if (e.rd) { require(committed[e.rd] == e.stale, "test stale chain"); committed[e.rd] = e.tag; }
                }
                pending.insert(pending.end(), added.begin(), added.end());
                if (create) branches.push_back({out.checkpoint_id, added.back().sequence});
            }
        }
        dut.clk_i = 1; dut.eval();
        uint64_t free = 0;
        for (auto p : available()) free |= UINT64_C(1) << p;
        require(dut.free_o == free && dut.ready_o == readiness() && dut.checkpoint_valid_o == valid(), "post-edge ownership/readiness");
        map = speculative();
        for (unsigned arch = 0; arch < 32; ++arch)
            require(tag(dut.rat_o, arch) == map[arch] && tag(dut.committed_o, arch) == committed[arch], "post-edge maps");
        dut.clk_i = 0; dut.eval(); return out;
    }
    void reset() { Input in; in.reset = true; run(in); }
    void negative(const std::string& name) {
        reset(); Input i; i.valid = 1; i.rd = {5, 0}; run(i);
        dut.valid_i = 0; dut.clk_i = 0;
        if (name == "commit_prefix") dut.commit_i = 2;
        else if (name == "wrong_stale") {
            dut.wb_offer_i = 1; dut.wb_live_i = 1; dut.wb_destination_i = 32;
            dut.commit_i = 1; dut.commit_rd_i = 5; dut.commit_destination_i = 32; dut.commit_stale_i = 6;
        } else throw std::runtime_error("unknown negative case");
        dut.eval(); dut.clk_i = 1; dut.eval(); throw std::runtime_error("negative case escaped assertions");
    }
};
int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    try {
        Check c;
        if (argc > 1 && std::string(argv[1]) == "negative") { c.negative(argv[2]); return 1; }
        unsigned seed = argc > 1 ? std::stoul(argv[1]) : 1;
        unsigned count = argc > 2 ? std::stoul(argv[2]) : 20000;
        std::mt19937 rng(seed); c.reset();
        Input write; write.valid = 1; write.rd = {1, 0}; c.run(write);
        Input branch; branch.valid = 3; branch.checkpoint = 1; branch.rd = {5, 6}; c.run(branch);
        Input wb; wb.wb[0] = c.result(0); c.run(wb);
        Input commit; commit.commits = 1; c.run(commit);
        write.rd = {6, 0}; const auto recycled = c.run(write).destination;
        Input inner; inner.valid = 3; inner.checkpoint = 2; inner.rd = {7, 7}; c.run(inner);
        write.rd = {8, 0}; c.run(write);
        Input recover; recover.resolve = true; recover.mispredict = true; recover.slot = 1;
        recover.valid = 3; recover.rd = {9, 10}; recover.commits = 1;
        recover.wb[0] = c.result(3); recover.wb[1] = c.result(4); c.run(recover);
        write.rd = {8, 0}; c.run(write);
        Input stale; stale.wb[0] = {c.pending.back().tag, true, false}; c.run(stale);
        stale.resolve = true; stale.resolve_live = false; stale.mispredict = true; stale.slot = 0; c.run(stale);
        recover.slot = 0; recover.commits = 1; recover.wb[0] = c.result(0); recover.wb[1] = {recycled, true, true}; c.run(recover);
        Input read; read.valid = 1; read.rs1 = {1, 0}; read.rs2 = {5, 0}; c.run(read);
        branch.checkpoint = 2; branch.rd = {2, 3}; c.run(branch);
        branch.resolve = true; branch.slot = 0; c.run(branch);
        c.reset();
        Input zero; zero.valid = 1; zero.checkpoint = 1;
        for (unsigned n = 0; n < 8; ++n) c.run(zero);
        c.run(zero); read.rs1 = {}; read.rs2 = {}; c.run(read);
        zero.resolve = true; zero.slot = 3; c.run(zero);
        zero.resolve = false; c.run(zero);
        Input kill; kill.resolve = true; kill.mispredict = true; kill.slot = 5; c.run(kill);
        Input flush; flush.flush = true; flush.valid = 3; flush.checkpoint = 3; c.run(flush);
        write.rd = {2, 0}; c.run(write); wb = {}; wb.wb[0] = c.result(0); wb.commits = 1;
        wb.valid = 1; wb.rd = {2, 0}; c.run(wb);
        Input duplicate; duplicate.wb[0] = c.result(0); duplicate.wb[1] = duplicate.wb[0]; c.run(duplicate);
        c.run(duplicate);
        Input invalid; invalid.wb[0] = {0, true, true}; invalid.wb[1] = {63, true, true}; c.run(invalid);
        Input blocked; blocked.valid = 1; blocked.checkpoint = 1; blocked.rd = {2, 0}; blocked.resources = false; c.run(blocked);
        c.reset();
        Input fill; fill.valid = 3; fill.rd = {1, 1};
        for (unsigned n = 0; n < 16; ++n) c.run(fill);
        Input need = blocked; need.resources = true; c.run(need);
        Input release = need; release.wb[0] = c.result(0); release.wb[1] = c.result(1); release.commits = 2; c.run(release);
        c.run(need);
        Input collision = flush; collision.wb[0] = c.result(0); collision.commits = 1; c.run(collision);
        for (unsigned n = 0; n < count; ++n) {
            Input in; in.valid = rng()%4; in.checkpoint = rng()%4; in.resources = rng()%5 != 0;
            in.reset = rng()%499 == 0; in.flush = rng()%103 == 0;
            for (unsigned lane = 0; lane < 2; ++lane) { in.rd[lane] = rng()%32; in.rs1[lane] = rng()%32; in.rs2[lane] = rng()%32; }
            if (n%3 == 0) in.rs1[1] = in.rd[0];
            if (n%5 == 0) in.rd[1] = in.rd[0];
            std::vector<unsigned> waiting;
            for (unsigned j = 0; j < c.pending.size(); ++j) if (c.pending[j].rd && !c.pending[j].done) waiting.push_back(j);
            std::shuffle(waiting.begin(), waiting.end(), rng);
            for (unsigned lane = 0; lane < 2; ++lane)
                if (lane < waiting.size() && rng()%3) { in.wb[lane] = c.result(waiting[lane]); in.wb[lane].live = rng()%7 != 0; }
            if (!c.branches.empty() && rng()%3 == 0) {
                in.resolve = true; in.slot = c.branches[rng()%c.branches.size()].slot;
                in.mispredict = rng()%3 == 0; in.resolve_live = rng()%5 != 0;
            }
            in.commits = rng()%(c.committable(in)+1);
            const unsigned prefix = in.valid == 3 && !(in.checkpoint & 1) ? 2 : in.valid == 0 ? 0 : 1;
            in.resources = in.resources && c.pending.size()-in.commits+prefix <= 32;
            c.run(in);
        }
        c.run(flush);
        for (const auto* name : {"resource_stall", "physical_stall", "deferred_tag_reuse", "flush_collision", "reject_zero", "reject_free", "reject_ready", "created", "lane1_cfi", "cut", "waw", "full_stall", "full_non_cfi", "recovery", "nested",
                                "younger_kill", "commit_suppressed", "retained_wb", "killed_wb", "resolve_create", "live_reset",
                                "full_flush", "reject_resolve", "reject_stale", "commit_rename"})
            c.require(c.coverage[name] > 0, std::string("missing coverage ")+name);
        std::cout << "RENAME RECOVERY PASS seed=" << seed << " cycles=" << c.cycles;
        for (const auto& [name, total] : c.coverage) std::cout << ' ' << name << '=' << total;
        std::cout << '\n';
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
