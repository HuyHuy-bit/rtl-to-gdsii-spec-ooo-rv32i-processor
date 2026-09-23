#include "packed_bits.hpp"
using packed_bits::bit;
using packed_bits::put;
using packed_bits::get32;
#include "Vfetch_two_wide.h"
#include "verilated.h"
#include "fetch_memory_layout.hpp"
#include <cstdint>
#include <deque>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
#include <string>

static uint32_t word(uint32_t address, unsigned serial) {
    return (address * 0x9e3779b9U) ^ (serial * 0x85ebca6bU) ^ 0x12345678U;
}
struct Input {
    bool reset=false, enable=true, redirect=false, request_ready=true, response=false;
    uint32_t target=0;
    unsigned take=0, status=0;
    int corrupt=0;
};
struct Request { uint32_t pc=0; unsigned id=0, serial=0; };
struct Entry { uint32_t pc, instruction; bool fault; unsigned cause; };
struct Bench {
    Vfetch_two_wide d;
    unsigned cycles=0, serial=0, expected_id=0;
    uint32_t pc=0;
    bool offered=false, pending=false, stale=false, stopped=false, fatal=false;
    Request request;
    std::deque<Entry> entries;
    std::map<std::string, unsigned> coverage;
    void require(bool good, const std::string& message) const {
        if (!good) throw std::runtime_error("fetch two wide mismatch cycle="+std::to_string(cycles)+": "+message);
    }
    unsigned available() const { return entries.empty() ? 0 : entries.front().fault || entries.size()==1 ? 1 : 3; }
    unsigned tick(Input i={}) {
        d.clk_i=0; d.rst_i=i.reset; d.enable_i=i.enable; d.redirect_i=i.redirect;
        d.redirect_pc_i=i.target; d.request_ready_i=i.request_ready; d.response_valid_i=i.response;
        d.take_i=i.take;
        for (unsigned n=0; n<(RESPONSE_BITS+31)/32; ++n) d.response_i[n]=0;
        put(d.response_i, RESPONSE_TRANSACTION_ID_OFFSET, 4, request.id ^ (i.corrupt==1 ? 1U : 0U));
        put(d.response_i, RESPONSE_STATUS_OFFSET, 2, i.status);
        if (i.status==0 || i.corrupt==3)
            for (unsigned n=0; n<8; ++n)
                put(d.response_i, RESPONSE_LINE_READ_DATA_OFFSET+32*n, 32, word((request.pc&~31U)+4*n, request.serial));
        if (i.corrupt==2) put(d.response_i, RESPONSE_UNCACHED_READ_DATA_OFFSET, 32, 1);
        d.eval();
        unsigned accepted=0;
        if (i.reset) {
            require(!d.request_valid_o && !d.response_ready_o && !d.valid_o && !d.busy_o, "reset suppresses interfaces");
            coverage[pending ? "reset_pending" : offered ? "reset_offered" : !entries.empty() ? "reset_buffered" : "reset_idle"]++;
            pc=0; expected_id=0; offered=pending=stale=stopped=fatal=false; entries.clear();
        } else if (fatal) {
            require(d.fatal_o && !d.request_valid_o && !d.response_ready_o && !d.valid_o && !d.busy_o, "sticky fatal blocks traffic");
            coverage["fatal_hold"]++;
        } else {
            require(!d.fatal_o, "unexpected fatal");
            const unsigned valid=i.redirect ? 0 : available();
            require(d.valid_o==valid, "instruction availability");
            require(bool(d.fault_o)==bool(valid && entries.front().fault), "fault indication");
            if (valid) {
                coverage[i.take ? "consume" : "output_stall"]++;
                if (!i.enable) coverage["disabled_buffer"]++;
                for (unsigned lane=0; lane<(valid==3 ? 2U : 1U); ++lane) {
                    const Entry& e=entries[lane];
                    require(uint32_t(d.pc_o >> (32*lane))==e.pc, "instruction PC");
                    require(uint32_t(d.instruction_o >> (32*lane))==e.instruction, "instruction data/order");
                }
                if (entries.front().fault) require(d.fault_cause_o==entries.front().cause, "fault cause");
                if (valid==1) require(uint32_t(d.instruction_o >> 32)==0, "invalid upper lane zero");
                if (i.take==1 && valid==3) coverage["partial_pair"]++;
                if (i.take==3) coverage["dual_take"]++;
                if (i.take && entries.front().pc%32==28) coverage["line_tail"]++;
            }
            require(!(i.take & ~valid) && i.take!=2, "test caller prefix");
            require(bool(d.response_ready_o)==pending, "response ownership");
            if (offered) require(d.request_valid_o, "request withdrawn");
            if (d.request_valid_o) {
                require(!pending, "multiple requests");
                if (!offered) {
                    require(entries.empty() && !stopped && pc%4==0 && pc<65536, "request without executable demand");
                    request={pc, expected_id, ++serial}; offered=true;
                    coverage["request"]++;
                    coverage["offset_"+std::to_string(pc%32/4)]++;
                }
                for (unsigned b=0; b<REQUEST_BITS; ++b) {
                    unsigned bit=0;
                    if (b>=REQUEST_ADDRESS_OFFSET && b<REQUEST_ADDRESS_OFFSET+32)
                        bit=((request.pc&~31U) >> (b-REQUEST_ADDRESS_OFFSET)) & 1U;
                    if (b>=REQUEST_TRANSACTION_ID_OFFSET && b<REQUEST_TRANSACTION_ID_OFFSET+4)
                        bit=(request.id >> (b-REQUEST_TRANSACTION_ID_OFFSET)) & 1U;
                    require(get32(d.request_o,b,1)==bit, "request payload/stability");
                }
                if (!i.request_ready) coverage["request_stall"]++;
            }
            require(bool(d.busy_o)==bool(offered || pending), "drain busy indication");
            if (!i.enable && !offered && !pending && entries.empty()) coverage["disabled_empty"]++;
            const bool bad=i.response && (!pending || i.corrupt!=0 || i.status>1);
            const bool local_fault=!pending && !offered && entries.empty() && !stopped && i.enable && !i.redirect
                                 && (pc%4!=0 || pc>=65536);
            if (i.redirect) {
                coverage[pending ? "redirect_pending" : offered ? "redirect_offered" : stopped ? "redirect_stopped" : !entries.empty() ? "redirect_buffered" : "redirect_empty"]++;
                if (stale) coverage["repeated_redirect"]++;
                if (pending || offered) stale=true;
                entries.clear(); stopped=false; pc=i.target;
            } else if (i.take) {
                accepted=i.take==3 ? 2 : 1;
                if (entries.front().fault) { stopped=true; coverage["fault_take"]++; }
                for (unsigned n=0; n<accepted; ++n) {
                    pc=entries.front().pc+4; entries.pop_front();
                }
            }
            if (bad) { fatal=true; coverage["malformed"]++; }
            else if (i.response) {
                if (stale) coverage[i.status ? "discard_fault" : "discard_data"]++;
                else if (i.status) {
                    entries.push_back({request.pc,0,true,1}); coverage["bus_fault"]++;
                } else {
                    for (uint32_t a=request.pc; a<(request.pc&~31U)+32; a+=4)
                        entries.push_back({a,word(a,request.serial),false,0});
                    coverage["fill"]++;
                }
                pending=false; stale=false;
                expected_id=(expected_id+1)%16;
                if (!expected_id) coverage["id_wrap"]++;
                if (i.redirect) coverage[i.status ? "redirect_response_fault" : "redirect_response"]++;
            } else if (offered && i.request_ready) {
                offered=false; pending=true;
                if (i.redirect) coverage["redirect_handshake"]++;
            }
            if (local_fault) {
                entries.push_back({pc,0,true,pc%4 ? 0U : 1U});
                coverage[pc%4 ? "alignment_fault" : "pma_fault"]++;
            }
        }
        d.clk_i=1; d.eval(); ++cycles;
        return accepted;
    }
    void reset() { Input i; i.reset=true; tick(i); }
    void redirect(uint32_t target) { Input i; i.redirect=true; i.target=target; tick(i); }
    void await_request(bool ready=true) {
        for (unsigned n=0; !offered && !pending; ++n) {
            require(n<8, "request progress"); Input i; i.request_ready=ready; tick(i);
        }
    }
    void respond(unsigned status=0) {
        await_request();
        if (offered) tick();
        Input i; i.response=true; i.status=status; tick(i);
    }
    void consume_all() {
        while (!entries.empty()) { Input i; i.take=available(); tick(i); }
    }
};
static void directed(Bench& b) {
    b.reset();
    for (unsigned n=0; n<4; ++n) {
        Input i; i.enable=false; b.tick(i);
        b.require(!b.d.request_valid_o, "disabled demand started a request");
    }
    for (unsigned offset=0; offset<8; ++offset) {
        b.redirect(0x100+offset*4); b.respond();
        for (unsigned n=0; n<3; ++n) { Input i; i.enable=false; b.tick(i); }
        Input i; i.take=1; b.tick(i); b.consume_all();
    }
    for (unsigned n=0; n<40; ++n) { b.respond(); b.consume_all(); }
    b.await_request(false);
    for (unsigned n=0; n<4; ++n) { Input i; i.request_ready=false; i.redirect=true; i.target=0x200+n*4; b.tick(i); }
    Input i; i.redirect=true; i.target=0x300; b.tick(i);
    for (unsigned n=0; n<3; ++n) b.tick();
    b.respond(); b.respond(); b.consume_all();
    b.await_request(); b.redirect(0x400); b.redirect(0x41c); b.respond(1); b.respond(); b.consume_all();
    b.await_request(); i={}; i.response=true; i.redirect=true; i.target=0x504; b.tick(i);
    b.respond(); b.consume_all(); b.await_request();
    i={}; i.response=true; i.status=1; i.redirect=true; i.target=0x600; b.tick(i);
    b.respond(); b.redirect(0x604); b.respond(); b.consume_all();
    b.respond(1);
    for (unsigned n=0; n<4; ++n) b.tick();
    b.consume_all(); for (unsigned n=0; n<4; ++n) b.tick();
    for (uint32_t target : {0x10000000U, 65536U, 2U, 0xffffffffU, 0xfffffffeU}) {
        b.redirect(target); b.tick(); b.tick(); b.consume_all(); b.tick();
    }
    b.redirect(65532); b.respond(); b.consume_all(); b.tick(); b.tick(); b.consume_all();
    b.reset(); b.await_request(false); b.reset();
    b.await_request(); b.reset(); b.respond(); b.reset();
    // Malformed traffic is fatal even when the request has been discarded.
    for (unsigned fault=0; fault<8; ++fault) {
        b.reset();
        if (fault!=4) b.await_request(fault!=5);
        if (fault<4) b.redirect(0x800);
        i={}; i.response=true;
        if (fault==0) i.corrupt=1;
        if (fault==1) i.corrupt=2;
        if (fault==2) { i.corrupt=3; i.status=1; }
        if (fault==3) i.status=2;
        if (fault==6) i.status=3;
        if (fault==7) b.respond();
        b.tick(i);
        b.coverage["malformed_"+std::to_string(fault)]++;
        for (unsigned n=0; n<3; ++n) { i={}; i.redirect=true; i.target=0; b.tick(i); }
    }
    b.reset();
}
int main(int argc, char** argv) {
    Verilated::commandArgs(argc,argv);
    try {
        Bench b;
        if (argc>1 && std::string(argv[1])=="negative") {
            b.reset();
            if (argc>2 && std::string(argv[2])=="empty") b.d.take_i=1;
            else { b.respond(); b.d.take_i=2; } b.d.rst_i=0; b.d.clk_i=0; b.d.eval(); b.d.clk_i=1; b.d.eval();
            throw std::runtime_error("missing prefix assertion");
        }
        const unsigned seed=argc>1 ? std::stoul(argv[1]) : 1;
        const unsigned random_cycles=argc>2 ? std::stoul(argv[2]) : 20000;
        directed(b); const unsigned directed_cycles=b.cycles;
        std::mt19937 rng(seed);
        for (unsigned n=0; n<random_cycles; ++n) {
            Input i;
            i.reset=rng()%503==0;
            i.enable=rng()%4!=0;
            i.request_ready=rng()%3!=0;
            i.response=b.pending && rng()%5==0;
            i.status=rng()%11==0;
            i.redirect=rng()%29==0 || b.stopped;
            i.target=(rng()%1024)*4;
            if (rng()%37==0) i.target|=2;
            if (rng()%41==0) i.target=0x10000000;
            i.take=i.redirect ? 0 : rng()%3==0 ? 0 : b.available()==3 && rng()%2 ? 3 : b.available() ? 1 : 0;
            if (i.reset) i.take=0;
            b.tick(i);
        }
        b.reset(); b.respond(); b.consume_all();
        std::cout << "FETCH TWO WIDE PASS seed=" << seed << " directed=" << directed_cycles
                  << " random=" << random_cycles << " cycles=" << b.cycles;
        for (const auto& [name,count] : b.coverage) std::cout << ' ' << name << '=' << count;
        std::cout << '\n';
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
