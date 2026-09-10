#include "Vrename_bundle.h"
#include "verilated.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

struct Input {
    std::array<unsigned, 32> map{};
    std::array<unsigned, 2> rs1{}, rs2{}, rd{};
    uint64_t free = UINT64_C(0xffffffff00000000), ready = UINT64_C(0xffffffff);
    unsigned valid = 3, checkpoint = 0;
    bool resources = true, reset = false, recover = false;
    Input() { std::iota(map.begin(), map.end(), 0); }
};

struct Output {
    unsigned accept = 0, rs1 = 0, rs2 = 0, destination = 0, stale = 0, ready = 0;
    uint64_t allocations = 0;
};

// Apply each instruction to a temporary architectural map, then admit the whole prefix.
Output reference(const Input& in) {
    Output out;
    if (in.reset || in.recover || !in.resources || in.valid == 2) return out;
    auto map = in.map;
    auto ready = in.ready;
    std::vector<unsigned> pool;
    for (unsigned p = 1; p < 64; ++p)
        if ((in.free >> p) & 1) pool.push_back(p);
    const unsigned count = in.valid == 0 ? 0 : (in.valid == 1 || (in.checkpoint & 1) ? 1 : 2);
    unsigned needed = 0;
    for (unsigned lane = 0; lane < count; ++lane) needed += in.rd[lane] != 0;
    if (pool.size() < needed) return out;
    for (unsigned lane = 0; lane < count; ++lane) {
        const auto a = map[in.rs1[lane]], b = map[in.rs2[lane]];
        out.accept |= 1 << lane;
        out.rs1 |= a << (6*lane);
        out.rs2 |= b << (6*lane);
        out.ready |= unsigned((ready >> a) & 1) << (2*lane);
        out.ready |= unsigned((ready >> b) & 1) << (2*lane+1);
        if (in.rd[lane]) {
            const unsigned tag = pool.front();
            pool.erase(pool.begin());
            out.stale |= map[in.rd[lane]] << (6*lane);
            out.destination |= tag << (6*lane);
            out.allocations |= UINT64_C(1) << tag;
            ready &= ~(UINT64_C(1) << tag);
            map[in.rd[lane]] = tag;
        }
    }
    return out;
}

class Check {
    Vrename_bundle dut;
public:
    unsigned cases = 0, raw = 0, waw = 0, blocked = 0, cut = 0, zero = 0;
    void run(const Input& in) {
        ++cases;
        dut.rst_i = in.reset;
        dut.recover_i = in.recover;
        dut.resources_ready_i = in.resources;
        dut.valid_i = in.valid;
        dut.checkpoint_i = in.checkpoint;
        dut.free_i = in.free;
        dut.ready_i = in.ready;
        dut.rs1_i = in.rs1[0] | (in.rs1[1] << 5);
        dut.rs2_i = in.rs2[0] | (in.rs2[1] << 5);
        dut.rd_i = in.rd[0] | (in.rd[1] << 5);
        for (unsigned word = 0; word < 6; ++word) dut.rat_i[word] = 0;
        for (unsigned arch = 0; arch < 32; ++arch)
            for (unsigned bit = 0; bit < 6; ++bit) {
                const unsigned offset = 6*arch + bit;
                dut.rat_i[offset/32] |= ((in.map[arch] >> bit) & 1) << (offset%32);
            }
        dut.eval();
        const auto out = reference(in);
        if (dut.accept_o != out.accept || dut.source1_o != out.rs1 || dut.source2_o != out.rs2
            || dut.source_ready_o != out.ready || dut.destination_o != out.destination
            || dut.stale_o != out.stale || dut.allocation_o != out.allocations)
            throw std::runtime_error("rename bundle mismatch case=" + std::to_string(cases));
        raw += out.accept == 3 && in.rd[0] && (in.rs1[1] == in.rd[0] || in.rs2[1] == in.rd[0]);
        waw += out.accept == 3 && in.rd[0] && in.rd[1] == in.rd[0];
        blocked += in.valid && !out.accept;
        cut += in.valid == 3 && out.accept == 1 && (in.checkpoint & 1);
        zero += out.accept && (!in.rd[0] || (out.accept == 3 && !in.rd[1]));
    }
};

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    try {
        const unsigned seed = argc > 1 ? std::stoul(argv[1]) : 1;
        const unsigned random_cases = argc > 2 ? std::stoul(argv[2]) : 20000;
        std::mt19937 rng(seed);
        Check check;
        Input in;
        for (unsigned first = 0; first < 32; ++first)
            for (unsigned second = 0; second < 32; ++second)
                for (unsigned pattern = 0; pattern < 8; ++pattern) {
                    in.rd = {first, second};
                    in.rs1 = {first, pattern & 1 ? first : second};
                    in.rs2 = {second, pattern & 2 ? first : 0};
                    in.ready = pattern & 4 ? ~UINT64_C(0) : UINT64_C(1);
                    check.run(in);
                }
        for (unsigned tag = 1; tag < 64; ++tag) {
            in = Input{};
            if (tag < 32) in.map[tag] = 32;
            for (unsigned mask = 0; mask < 4; ++mask)
                for (unsigned checkpoint = 0; checkpoint < 4; ++checkpoint)
                    for (unsigned destinations = 0; destinations < 4; ++destinations) {
                        in.free = UINT64_C(1) << tag;
                        in.valid = mask;
                        in.checkpoint = checkpoint;
                        in.rd = {destinations & 1 ? 5U : 0U, destinations & 2 ? 5U : 0U};
                        in.rs1 = {5, 5}; in.rs2 = {0, 5};
                        check.run(in);
                        in.free = 0;
                        check.run(in);
                    }
        }
        in = Input{};
        in.rd = {5, 5}; in.rs1 = {5, 5}; in.rs2 = {5, 5};
        for (unsigned controls = 0; controls < 8; ++controls) {
            in.reset = controls & 1; in.recover = controls & 2; in.resources = controls & 4;
            check.run(in);
        }
        for (unsigned n = 0; n < random_cases; ++n) {
            in = Input{};
            std::array<unsigned, 63> permutation;
            std::iota(permutation.begin(), permutation.end(), 1);
            std::shuffle(permutation.begin(), permutation.end(), rng);
            in.free = (uint64_t(rng()) << 32) | rng();
            in.free &= ~UINT64_C(1);
            for (unsigned arch = 1; arch < 32; ++arch) {
                in.map[arch] = permutation[arch-1];
                in.free &= ~(UINT64_C(1) << in.map[arch]);
            }
            in.ready = (uint64_t(rng()) << 32) | rng() | UINT64_C(1);
            in.valid = rng() % 4; in.checkpoint = rng() % 4;
            in.reset = rng() % 31 == 0; in.recover = rng() % 17 == 0; in.resources = rng() % 4 != 0;
            for (unsigned lane = 0; lane < 2; ++lane) {
                in.rd[lane] = rng() % 32; in.rs1[lane] = rng() % 32; in.rs2[lane] = rng() % 32;
            }
            if (n % 3 == 0) in.rs1[1] = in.rd[0];
            if (n % 5 == 0) in.rd[1] = in.rd[0];
            if (n % 7 == 0) in.free = 0;
            check.run(in);
        }
        if (!check.raw || !check.waw || !check.blocked || !check.cut || !check.zero)
            throw std::runtime_error("missing rename bundle coverage");
        std::cout << "RENAME BUNDLE PASS seed=" << seed << " cases=" << check.cases
                  << " raw=" << check.raw << " waw=" << check.waw << " blocked=" << check.blocked
                  << " cut=" << check.cut << " zero=" << check.zero << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
