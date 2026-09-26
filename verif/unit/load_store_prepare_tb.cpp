#include "Vload_store_prepare.h"
#include "verilated.h"
#include "platform_memory_layout.hpp"
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
#include <string>

struct Input {
    bool reset=false, flush=false, launch=false, take=false;
    unsigned id=0;
    uint32_t instruction=0x00002083, pc=0, source1=0, source2=0;
};
struct Descriptor {
    unsigned id=0, mask=0, size=0;
    uint32_t instruction=0, pc=0, source1=0, source2=0, address=0, data=0, cause=0, value=0;
    bool store=false, unsign=false, uncached=false, head=false, fault=false;
};
static uint32_t instruction(bool store,unsigned kind,unsigned rs1,unsigned rs2,unsigned rd,int immediate) {
    const unsigned imm=unsigned(immediate)&4095;
    return store ? (imm>>5)<<25 | rs2<<20 | rs1<<15 | kind<<12 | (imm&31)<<7 | 0x23
                 : imm<<20 | rs1<<15 | kind<<12 | rd<<7 | 3;
}
static Descriptor reference(const Input& in) {
    Descriptor e; e.id=in.id; e.instruction=in.instruction; e.pc=in.pc;
    e.store=(in.instruction&127)==0x23;
    const unsigned kind=(in.instruction>>12)&7;
    e.size=kind%4; e.unsign=!e.store && kind>=4;
    const unsigned rs1=(in.instruction>>15)&31, rs2=(in.instruction>>20)&31;
    e.source1=rs1 ? in.source1:0; e.source2=e.store && rs2 ? in.source2:0;
    const unsigned bits=e.store ? ((in.instruction>>25)<<5)|((in.instruction>>7)&31):in.instruction>>20;
    const int offset=bits>=2048 ? int(bits)-4096:int(bits);
    e.address=e.source1+uint32_t(offset);
    const unsigned bytes=1u<<e.size;
    const Region* region=nullptr;
    for (const auto& r:REGIONS) {
        bool match=true;
        for (unsigned n=0;n<bytes;n++) {
            const uint64_t address=uint64_t(e.address)+n;
            match &= address>=r.base && address<uint64_t(r.base)+r.size;
        }
        if (match && (e.store ? r.write:r.read)) region=&r;
    }
    e.fault=e.address%bytes!=0 || !region;
    if (e.fault) {
        e.cause=e.address%bytes ? (e.store ? 6:4):(e.store ? 7:5);
        e.value=e.address; e.head=true;
    } else {
        e.uncached=!region->cacheable;
        e.head=e.store || !region->cacheable || !region->idempotent;
        for (unsigned n=0;n<bytes;n++) {
            const unsigned lane=(e.address+n)%4;
            e.mask |= 1u<<lane;
            if (e.store) e.data |= ((e.source2>>(8*n))&255u)<<(8*lane);
        }
    }
    return e;
}
struct Bench {
    Vload_store_prepare d;
    Descriptor held;
    bool pending=false;
    unsigned cycles=0;
    std::map<std::string,unsigned> coverage;
    void require(bool good,const std::string& why) const {
        if (!good) throw std::runtime_error("load store prepare mismatch cycle="+std::to_string(cycles)+": "+why);
    }
    void tick(const Input& in={}) {
        d.clk_i=0; d.rst_i=in.reset; d.flush_i=in.flush; d.launch_i=in.launch; d.take_i=in.take;
        d.id_i=in.id; d.instruction_i=in.instruction; d.pc_i=in.pc; d.source1_i=in.source1; d.source2_i=in.source2;
        d.eval();
        const bool active=!in.reset && !in.flush, valid=pending && active;
        require(bool(d.valid_o)==valid,"valid");
        require(bool(d.ready_o)==(active && (!pending || in.take)),"ready");
        const Descriptor e=valid ? held:Descriptor{};
        require(d.id_o==e.id,"full ROB identity");
        require(d.instruction_o==e.instruction && d.pc_o==e.pc,"instruction/PC");
        require(d.source1_o==e.source1 && d.source2_o==e.source2,"architectural sources");
        require(d.address_o==e.address,"effective address");
        require(d.write_data_o==e.data && d.byte_mask_o==e.mask,"byte lanes/store data");
        require(d.cause_o==e.cause && d.trap_value_o==e.value && bool(d.fault_o)==e.fault,"fault metadata");
        require(d.size_o==e.size && bool(d.store_o)==e.store && bool(d.load_unsigned_o)==e.unsign,"operation metadata");
        require(bool(d.uncached_o)==e.uncached && bool(d.head_only_o)==e.head,"PMA ordering");
        if (valid) {
            coverage[in.take ? "taken":"held"]++;
            if (in.take) {
                coverage[std::string(e.store ? "store_":"load_")+std::to_string((e.instruction>>12)&7)]++;
                coverage["lane_"+std::to_string(e.address%4)]++;
                coverage["slot_"+std::to_string(e.id%32)]++;
                if (e.fault) coverage["cause_"+std::to_string(e.cause)]++;
                else coverage[e.uncached ? "mmio":"ram"]++;
                if (!e.head) coverage["speculative_eligible"]++;
                if (e.head && !e.fault) coverage["head_only"]++;
            }
        }
        if (pending && in.reset) coverage["reset_live"]++;
        if (pending && in.flush) coverage["flush_live"]++;
        if (valid && in.take && in.launch) coverage["turnover"]++;
        if (in.launch && active) {
            if (in.id>=32) coverage["generation"]++;
            if (((in.instruction>>15)&31)==0) coverage["x0_base"]++;
            if ((in.instruction&127)==0x23 && ((in.instruction>>20)&31)==0) coverage["x0_store"]++;
            if ((in.instruction&127)==3 && ((in.instruction>>7)&31)==0) coverage["x0_load"]++;
        }
        d.clk_i=1; d.eval(); d.clk_i=0; d.eval();
        if (!active) pending=false;
        else {
            if (in.take) pending=false;
            if (in.launch) { held=reference(in); pending=true; }
        }
        cycles++;
    }
    void command(Input in,unsigned stalls=3) {
        in.launch=true; tick(in);
        for (unsigned n=0;n<stalls;n++) {
            Input noise; noise.instruction=~in.instruction; noise.pc=~in.pc; noise.id=in.id^8191;
            noise.source1=~in.source1; noise.source2=~in.source2; tick(noise);
        }
        Input done; done.take=true; tick(done); tick();
    }
};
int main(int argc,char** argv) {
    Verilated::commandArgs(argc,argv);
    try {
        Bench b; Input reset; reset.reset=true; b.tick(reset);
        if (argc>1 && std::string(argv[1])=="negative") {
            const std::string name=argv[2]; Input in; in.launch=true;
            if (name=="capacity") { b.tick(in); b.tick(in); }
            else if (name=="take") { in.launch=false; in.take=true; b.tick(in); }
            else if (name=="pc") { in.pc=2; b.tick(in); }
            else if (name=="operation") { in.instruction=0x00000013; b.tick(in); }
            else if (name=="reserved-load") { in.instruction=0x00003003; b.tick(in); }
            else if (name=="reserved-store") { in.instruction=0x00007023; b.tick(in); }
            else throw std::runtime_error("unknown negative");
            throw std::runtime_error("caller assertion did not fire");
        }
        const unsigned seed=argc>1 ? std::stoul(argv[1]):1;
        const unsigned random_cycles=argc>2 ? std::stoul(argv[2]):20000;
        std::mt19937 rng(seed);
        unsigned sequence=0;
        auto command=[&](bool store,unsigned kind,uint32_t address,int offset,unsigned rs1=3,unsigned rs2=4,unsigned rd=5) {
            Input in; in.id=sequence++%8192; in.pc=(sequence*4)&~3u;
            in.instruction=instruction(store,kind,rs1,rs2,rd,offset);
            in.source1=address-uint32_t(offset); in.source2=0x80ff017e;
            b.command(in);
        };
        for (bool store:{false,true}) for (unsigned kind:{0u,1u,2u,4u,5u}) {
            if (store && kind>2) continue;
            for (const auto& r:REGIONS) for (uint32_t point:{r.base,r.base+r.size-4,r.base+r.size})
                for (unsigned lane=0;lane<4;lane++) for (int offset:{-2048,-1,0,1,2047})
                    command(store,kind,point+lane,offset);
            for (uint32_t address:{0xfffffffcu,0xfffffffdu,0xfffffffeu,0xffffffffu,0u,1u,2u,3u})
                for (int offset:{-2048,-1,1,2047}) command(store,kind,address,offset);
            for (int offset:{-2048,-1,0,1,2047}) command(store,kind,0x1234,offset,0,0,0);
        }
        for (unsigned id=0;id<8192;id++) {
            Input in; in.launch=true; in.take=b.pending; in.id=id; in.pc=0xfffffffc;
            in.instruction=instruction(id%2,id%3,1,2,id%32,0);
            in.source1=4*(id%16384); in.source2=rng(); b.tick(in);
        }
        { Input in; in.take=true; b.tick(in); b.tick(); }
        for (bool reset_live:{false,true}) for (unsigned kind=0;kind<4;kind++) {
            Input in; in.launch=true; in.instruction=instruction(kind%2,2,1,2,3,0);
            in.source1=kind<2 ? 0x10000000:0xffffffff; b.tick(in);
            in.take=true; in.flush=!reset_live; in.reset=reset_live; b.tick(in); b.tick();
        }
        for (unsigned n=0;n<random_cycles;n++) {
            Input in; in.reset=rng()%503==0; in.flush=rng()%97==0;
            in.take=b.pending && rng()%3!=0;
            in.launch=(!b.pending || in.take) && rng()%4!=0;
            in.id=rng()%8192; in.pc=rng()&~3u;
            const bool store=rng()%2;
            const unsigned kind=store ? rng()%3:std::array<unsigned,5>{0,1,2,4,5}[rng()%5];
            const unsigned base=rng()%32, data=rng()%32, rd=rng()%32;
            const int offset=int(rng()%4096)-2048;
            in.instruction=instruction(store,kind,base,data,rd,offset);
            const auto& r=REGIONS[rng()%REGIONS.size()];
            const uint32_t address=rng()%3 ? r.base+rng()%r.size:uint32_t(rng());
            in.source1=address-uint32_t(offset); in.source2=rng(); b.tick(in);
        }
        { Input in; in.take=b.pending; b.tick(in); b.tick(); }
        std::cout<<"LOAD STORE PREPARE PASS seed="<<seed<<" cycles="<<b.cycles;
        for (const auto& [name,count]:b.coverage) std::cout<<' '<<name<<'='<<count;
        std::cout<<'\n';
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
