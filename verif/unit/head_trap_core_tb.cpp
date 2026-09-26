#include "fetch_execution_reference.hpp"

static void handler(Bench& b,bool dependencies=false) {
    b.memory[64]=instruction(2,5,dependencies ? 5 : 0,0,7);
    b.memory[65]=instruction(2,6,dependencies ? 6 : 0,0,11);
    b.memory[66]=instruction(27,0,0,0,0x1000-0x108);
    b.memory[0x1000/4]=0x10500073;
}
static void init(Bench& b) {
    b.memory.fill(instruction(2,0,0,0,0));
    for (unsigned n=0;n<31;n++) b.memory[n]=instruction(2,n+1,0,0,n*11+5);
    handler(b,true);
}
static void restart(Bench& b,uint32_t pc) { Input i; i.flush=true; i.target=pc; b.tick(i); }
static void offer(Bench& b,std::mt19937& rng) {
    for (unsigned n=0;n<3000;n++) {
        Input i; i.execute=rng()%4; i.complete=rng()%4; i.retire=rng()%4;
        i.grant=rng()%4!=0; i.request_ready=rng()%3!=0; i.latency=rng()%5;
        b.tick(i);
        if (b.d.trap_valid_o) { for (unsigned k=0;k<7;k++) b.tick(); return; }
    }
    b.require(false,"trap preparation watchdog");
}
static void accept(Bench& b) {
    const unsigned before=b.traps;
    Input i; i.trap_ready=true; i.grant=false; i.complete=0; b.tick(i);
    b.require(b.traps==before+1,"trap acceptance lost");
    b.require(b.d.occupancy_o==0 && b.d.producer_busy_o==0,"trap did not clear backend/producers");
}
static void drain(Bench& b) {
    Input i; i.enable=false; i.flush=true; i.target=0x1000; b.tick(i);
    for (unsigned n=0;n<100 && (b.pending || b.held || b.d.fetch_busy_o);n++) {
        i.flush=false; b.tick(i);
    }
    i.flush=false; i.drain=true; b.tick(i);
}
int main(int argc,char** argv) {
    Verilated::commandArgs(argc,argv);
    try {
        const unsigned seed=argc>1 ? std::stoul(argv[1]) : 1;
        std::mt19937 rng(seed); Bench b; b.fault_support=true; b.trap_service=true;
        const std::array<uint32_t,20> faults={0,0xffffffff,0x02000033,0x40001033,
            0xfff01013,0x02005013,0x00001067,0x00002063,0x00003003,0x00006003,
            0x00003023,0x0000200f,0x00004073,0x00300073,0x0000002f,0x00000001,
            0x000000f3,0x00108073,0x00000073,0x00100073};
        for (unsigned offset=0;offset<8;offset++) for (uint32_t insn:faults) {
            init(b); const unsigned at=32+offset; b.memory[at]=insn;
            b.memory[at+1]=instruction(2,5,5,0,99); b.memory[at+2]=instruction(2,6,6,0,123);
            b.reset(); offer(b,rng); b.require(b.pc==at*4,"older instructions lost");
            accept(b); b.run_to(0x1000,rng);
            b.coverage["fault_offset_"+std::to_string(offset)]++;
        }
        // Repeated accepted traps preserve committed registers, CSR history and event order.
        for (unsigned n=0;n<320;n++) {
            if (n%100==0) drain(b);
            b.memory[128]=instruction(2,5,5,0,1); b.memory[129]=faults[n%faults.size()];
            restart(b,512); offer(b,rng); accept(b); b.run_to(0x1000,rng);
            b.coverage["slot_reuse"]++;
        }
        for (uint32_t pc:{2u,0xfffffffeu,0x10000000u,65536u}) {
            restart(b,pc); offer(b,rng); accept(b); b.run_to(0x1000,rng);
            b.coverage["local_fault"]++;
        }
        for (unsigned offset=0;offset<8;offset++) {
            b.error_address=512; restart(b,512+4*offset); offer(b,rng); accept(b); b.run_to(0x1000,rng);
            b.coverage["bus_offset_"+std::to_string(offset)]++;
        }
        b.error_address=UINT32_MAX;
        for (unsigned op:{21,27,28}) {
            init(b); b.memory[31]=instruction(2,1,0,0,2);
            b.memory[32]=instruction(op,7,op==28 ? 1:0,0,op==28 ? 0:2);
            b.reset(); offer(b,rng); accept(b); b.run_to(0x1000,rng);
            b.coverage["target_fault_"+std::to_string(op)]++;
        }
        // A waiting head fault outranks younger branch resolution and kills both holders.
        b.memory.fill(instruction(2,2,0,0,1)); b.memory[0]=0xffffffff;
        b.memory[1]=instruction(27,1,0,0,0x200-4); handler(b); b.reset();
        offer(b,rng);
        for (unsigned n=0;n<30;n++) { Input i; i.complete=0; b.tick(i); }
        b.require(b.d.producer_busy_o==3,"younger producers not held");
        accept(b); b.run_to(0x1000,rng); b.coverage["trap_over_younger_branch"]++;
        // Trap redirect must preserve an offered request and drain an accepted stale response.
        for (unsigned kind=0;kind<2;kind++) {
            b.memory.fill(instruction(2,0,0,0,0)); b.memory[0]=0xffffffff; handler(b); b.reset();
            for (unsigned n=0;n<200;n++) {
                Input i;
                if (!kind) i.request_ready=!(b.held || (b.d.request_valid_o && get32(b.d.request_o,REQUEST_ADDRESS_OFFSET,32)!=0));
                i.latency=kind ? 40:0;
                b.tick(i);
                if (b.d.trap_valid_o && (kind ? (b.pending && b.pending_address==32) : b.held)) break;
            }
            b.require(b.d.trap_valid_o && (kind ? b.pending && b.pending_address==32 : b.held),"missing stale fetch traffic");
            Input i; i.trap_ready=true; i.request_ready=kind; i.grant=false; b.tick(i);
            b.require(b.pc==0x100,"trap target lost with fetch traffic");
            b.run_to(0x1000,rng); b.coverage[kind ? "trap_pending_fetch" : "trap_offered_fetch"]++;
        }
        // External recovery and reset cancel a prepared trap before its state transition.
        for (uint32_t insn:{0xffffffffU,0x00000073U,0x00100073U}) for (unsigned reset=0;reset<2;reset++) {
            init(b); b.memory[32]=insn; b.reset(); offer(b,rng);
            const unsigned before=b.traps;
            Input i; i.trap_ready=true; i.flush=!reset; i.target=0x1000; i.reset=reset; b.tick(i);
            b.require(b.traps==before && !b.d.trap_valid_o,"canceled trap accepted");
            if (reset) offer(b,rng);
            else { b.memory[128]=0; restart(b,512); offer(b,rng); }
            accept(b); b.run_to(0x1000,rng); b.coverage[reset ? "reset_prepared" : "flush_prepared"]++;
        }
        // Malformed memory traffic cancels a held proposal and prevents architectural progress.
        for (uint32_t insn:{0xffffffffU,0x00000073U,0x00100073U}) {
            init(b); b.memory[32]=insn; b.reset(); offer(b,rng);
            const unsigned before=b.traps; Input corrupt; corrupt.inject_bad=true; b.tick(corrupt);
            for (unsigned n=0;n<5;n++) { Input i; i.trap_ready=true; b.tick(i); }
            b.require(b.traps==before && b.d.fatal_o && !b.d.trap_valid_o,"fatal accepted prepared trap");
            b.coverage["fatal_prepared"]++;
        }
        // A fault in the handler produces another ordered trap with updated old CSR values.
        init(b); b.memory[32]=0xffffffff; b.memory[65]=0; b.reset();
        for (unsigned n=0;n<8;n++) { offer(b,rng); accept(b); }
        b.coverage["handler_fault"]++;
        init(b); b.memory[32]=0xffffffff; b.memory[65]=0; b.reset();
        const unsigned first=b.traps;
        for (unsigned n=0;n<3000 && b.traps<first+12;n++) {
            Input i; i.trap_ready=true; i.execute=rng()%4; i.complete=rng()%4;
            i.retire=rng()%4; i.request_ready=rng()%3!=0; i.latency=rng()%5; b.tick(i);
        }
        b.require(b.traps==first+12,"ready-before-valid trap progress");
        b.coverage["ready_before_fault"]++;
        // Older branch recovery must discard wrong-path faults before CSR preparation.
        for (uint32_t insn:{0xffffffffU,0x00000073U,0x00100073U}) for (unsigned kind=0;kind<4;kind++) {
            b.memory.fill(instruction(2,0,0,0,0)); handler(b);
            b.memory[0]=instruction(27,1,0,0,0x1000); b.memory[0x1000/4]=0x10500073;
            if (kind<3) b.memory[1]=insn; else b.error_address=32;
            b.reset(); const unsigned prior=b.traps;
            for (unsigned n=0;n<70;n++) {
                Input i; i.grant=false; i.trap_ready=true;
                if (kind==0) i.execute=1;
                if (kind==1) i.complete=1;
                b.tick(i);
            }
            b.run_to(0x1000,rng); b.require(b.traps==prior,"wrong-path trap accepted");
            b.error_address=UINT32_MAX; b.coverage["wrong_path_"+std::to_string(kind)]++;
        }
        for (unsigned n=0;n<3;n++) {
            program(b,rng); b.memory[0x1000/4]=0x10500073; b.reset(); b.run_to(0x1000,rng,n==0);
            b.coverage["normal_program"]++;
        }
        std::cout<<"HEAD TRAP CORE PASS seed="<<seed<<" cycles="<<b.cycles<<" retired="<<b.retired<<" traps="<<b.traps;
        for (const auto& [name,count]:b.coverage) std::cout<<' '<<name<<'='<<count;
        std::cout<<'\n';
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
