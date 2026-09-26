#include "fetch_execution_reference.hpp"

static void until_fault(Bench& b,std::mt19937& rng) {
    for (unsigned n=0;n<2000;++n) {
        Input i; i.execute=rng()%4; i.complete=rng()%4; i.retire=rng()%4;
        i.grant=rng()%4!=0; i.request_ready=rng()%3!=0; i.latency=rng()%5;
        b.tick(i);
        if (b.d.backend_fault_o) {
            for (unsigned hold=0;hold<8;++hold) b.tick();
            return;
        }
    }
    b.require(false,"fault progress watchdog");
}
static void restart(Bench& b,uint32_t target) {
    Input i; i.flush=true; i.target=target; b.tick(i);
}
static void init(Bench& b) {
    b.memory.fill(0x10500073);
    for (unsigned n=0;n<31;++n) b.memory[n]=instruction(2,n+1,0,0,n*11+5);
}
int main(int argc,char** argv) {
    Verilated::commandArgs(argc,argv);
    try {
        unsigned seed=argc>1 ? std::stoul(argv[1]) : 1;
        std::mt19937 rng(seed); Bench b; b.fault_support=true;
        const std::array<uint32_t,20> faults={0,0xffffffff,0x02000033,0x40001033,
            0xfff01013,0x02005013,0x00001067,0x00002063,0x00003003,0x00006003,
            0x00003023,0x0000200f,0x00004073,0x00300073,0x0000002f,0x00000001,
            0x000000f3,0x00108073,0x00000073,0x00100073};
        for (unsigned offset=0;offset<8;++offset) for (uint32_t insn:faults) {
            init(b); unsigned at=32+offset;
            for (unsigned n=31;n<at;++n) b.memory[n]=instruction(2,4,4,0,1);
            b.memory[at]=insn; b.memory[at+1]=instruction(2,5,5,0,9);
            b.reset(); until_fault(b,rng);
            b.require(b.pc==at*4,"fault did not wait for all older retirement");
            b.coverage["fault_offset_"+std::to_string(offset)]++;
        }
        // Flush reuses fault-owned slots with ordinary work without losing committed state/order.
        init(b); b.memory[31]=0xffffffff; b.reset(); until_fault(b,rng);
        for (unsigned iteration=0;iteration<320;++iteration) {
            if (iteration && iteration%128==0) {
                Input i; i.enable=false; i.flush=true; i.target=256; b.tick(i);
                i.flush=false; i.drain=true; b.tick(i);
            }
            b.memory[64]=instruction(2,1,1,0,1); b.memory[65]=faults[iteration%faults.size()];
            restart(b,256); until_fault(b,rng);
            b.require(b.pc==260,"reused fault slot PC");
        }
        b.coverage["slot_reuse"]++;
        // Every real frontend error becomes a source-free ROB fault and stops at the head.
        for (uint32_t target:{2U,0xfffffffeU,0x10000000U,65536U}) {
            restart(b,target); until_fault(b,rng);
            b.require(b.pc==target,"fetch fault PC");
        }
        for (unsigned offset=0;offset<8;++offset) {
            b.error_address=512; restart(b,512+4*offset); until_fault(b,rng);
            b.coverage["bus_offset_"+std::to_string(offset)]++;
        }
        b.error_address=UINT32_MAX;
        // An older unresolved jump can squash queued, held and completed younger faults.
        for (uint32_t insn:{0xffffffffU,0x00000073U,0x00100073U}) for (unsigned kind=0;kind<4;++kind) {
            b.memory.fill(instruction(2,0,0,0,0));
            b.memory[0]=instruction(27,1,0,0,0x1000); b.memory[0x1000/4]=0x10500073;
            if (kind<3) b.memory[1]=insn;
            else b.error_address=32;
            b.reset();
            for (unsigned n=0;n<70;++n) {
                Input i; i.grant=false;
                if (kind==0) i.execute=1;
                if (kind==1) i.complete=1;
                b.tick(i);
            }
            b.require(!b.d.backend_fault_o,"younger fault bypassed older branch");
            b.require(b.d.producer_busy_o==(kind==1 ? 3 : 1),"fault did not reach intended queued/held/completed state");
            b.require(b.d.occupancy_o>=2,"younger fault was not allocated");
            b.run_to(0x1000,rng); b.require(b.d.producer_busy_o==0,"squashed fault still owns producer"); b.coverage["squash_fault_"+std::to_string(kind)]++;
            b.error_address=UINT32_MAX;
        }
        // A lane-zero fault ends the bundle even when the tail is legal but not implemented.
        for (uint32_t insn:{0xffffffffU,0x00000073U,0x00100073U}) {
            b.memory.fill(0x10500073); b.memory[0]=insn; b.reset(); until_fault(b,rng);
            b.require(b.d.occupancy_o==1,"lane-zero fault admitted tail");
            b.coverage["fault_prefix"]++;
        }
        // Legal deferred classes must stall rather than be relabeled as illegal encodings.
        for (uint32_t legal:{0x00002083U,0x00102023U,0x300010f3U,0x0000000fU,0x0000100fU,
                            0x30200073U,0x10500073U}) {
            b.memory.fill(0x10500073); b.memory[0]=legal; b.reset();
            for (unsigned n=0;n<20;++n) b.tick();
            b.require(b.d.unsupported_o && !b.d.backend_fault_o && b.d.occupancy_o==0,"legal deferred instruction misclassified");
            b.coverage["deferred_legal"]++;
        }
        for (unsigned run=0;run<3;++run) {
            program(b,rng); b.memory[0x1000/4]=0x10500073; b.reset(); b.run_to(0x1000,rng,run==0);
            b.coverage["normal_program"]++;
        }
        std::cout<<"FRONTEND FAULTS PASS seed="<<seed<<" cycles="<<b.cycles<<" retired="<<b.retired;
        for (const auto& [name,count]:b.coverage) std::cout<<' '<<name<<'='<<count;
        std::cout<<'\n';
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
