#include "fetch_execution_reference.hpp"

static void initialize(Bench& b) {
    b.memory.fill(0x0000000f);
    for (unsigned r=1;r<32;r++) b.memory[r-1]=instruction(2,r,0,0,r*7);
}
static void run(Bench& b,uint32_t stop,std::mt19937& rng,bool fill=false) {
    for (unsigned n=0;n<30000;n++) {
        Input i; i.trap_ready=true; i.request_ready=rng()%3!=0; i.latency=rng()%7;
        i.execute=rng()%4; i.complete=rng()%4; i.retire=rng()%4; i.grant=rng()%3!=0;
        if (fill && n<35) i.execute=0;
        if (fill && n<100) i.retire=0;
        b.tick(i);
        b.require(!b.fatal,"unexpected fatal");
        if (b.pc==stop && b.d.occupancy_o==0 && b.d.unsupported_o && !b.d.fetch_busy_o) return;
    }
    b.require(false,"system program watchdog pc="+std::to_string(b.pc)+" stop="+std::to_string(stop)+" occupancy="+std::to_string(b.d.occupancy_o)+" drain="+std::to_string(b.d.identity_drain_o)+" unsupported="+std::to_string(b.d.unsupported_o));
}
static void wait_prepared(Bench& b) {
    for (unsigned n=0;n<1000;n++) {
        Input i; i.retire=b.system_prepared ? 0:3; i.complete=3; b.tick(i);
        if (b.system_prepared && b.d.system_busy_o) return;
    }
    b.require(false,"system preparation watchdog");
}
int main(int argc,char** argv) {
    Verilated::commandArgs(argc,argv);
    try {
        const unsigned seed=argc>1 ? std::stoul(argv[1]):1;
        std::mt19937 rng(seed); Bench b; b.system_service=b.trap_service=b.fault_support=true;
        for (unsigned offset=0;offset<8;offset++) {
            initialize(b); unsigned at=31;
            for (unsigned n=0;n<offset;n++) b.memory[at++]=instruction(2,0,0,0,0);
            for (unsigned kind:{1u,2u,3u,5u,6u,7u}) for (unsigned mode=0;mode<4;mode++) {
                const unsigned source=mode==0 ? 0:5, rd=mode==1 ? 0:source ? source:6;
                b.memory[at++]=csr(0x340,kind,source,rd);
                b.memory[at++]=instruction(2,20,rd,0,1);
                b.coverage["kind_"+std::to_string(kind)]++;
            }
            b.memory[at++]=csr(0xb00,2,0,12);
            b.memory[at++]=csr(0xb02,2,0,13);
            b.memory[at++]=csr(0x320,5,5,0);
            b.memory[at++]=csr(0xb00,1,5,0);
            b.memory[at++]=csr(0xb80,5,7,0);
            b.memory[at++]=csr(0xb00,2,0,14);
            b.memory[at++]=csr(0xb80,2,0,15);
            b.memory[at++]=csr(0xb02,1,5,0);
            b.memory[at++]=csr(0xb02,2,0,16);
            b.memory[at++]=csr(0x320,5,0,0);
            b.memory[at++]=csr(0xb02,2,0,17);
            b.reset(); run(b,at*4,rng,true); b.coverage["offset_"+std::to_string(offset)]++;
        }
        for (unsigned offset=0;offset<8;offset++) for (unsigned inhibit:{0u,4u}) {
            initialize(b); unsigned at=31;
            b.memory[at++]=csr(0x320,5,inhibit,0);
            while ((at+1)%8!=offset) b.memory[at++]=instruction(2,4,4,0,1);
            b.memory[at++]=csr(0xb02,2,0,21);
            b.memory[at++]=0x10500073;
            b.memory[at++]=csr(0xb02,2,0,22);
            b.memory[at++]=0x10500073;
            b.memory[at++]=0x10500073;
            b.memory[at++]=instruction(2,20,4,0,1);
            b.memory[at++]=csr(0x320,5,0,0);
            b.memory[at++]=csr(0xb02,2,0,23);
            b.reset(); run(b,at*4,rng,true);
            b.require(b.regs[22]-b.regs[21]==(inhibit ? 0u:2u),"WFI instret count");
            b.coverage["wfi_offset_"+std::to_string(offset)]++;
            b.coverage[inhibit ? "wfi_inhibited":"wfi_counted"]++;
        }
        initialize(b); b.memory[31]=0x10500073; b.memory[32]=csr(0xb02,2,0,10);
        b.reset(); run(b,132,rng,true);
        b.require(b.regs[10]==32,"WFI older work and retirement count"); b.coverage["wfi_older_work"]++;
        // A trap handler observes the fault, advances mepc and returns through the real fetch path.
        for (unsigned fault_kind=0;fault_kind<5;fault_kind++) for (unsigned offset=0;offset<8;offset++) {
            initialize(b); unsigned at=31;
            b.memory[at++]=instruction(2,1,0,0,0x400);
            b.memory[at++]=csr(0x305,1,1,0);
            b.memory[at++]=csr(0x300,5,8,0);
            while (at%8!=offset) b.memory[at++]=instruction(2,4,4,0,1);
            const unsigned fault_pc=at*4;
            b.memory[at++]=fault_kind==0 ? csr(0xfff,2,5,6) : fault_kind==1 ? csr(0xf11,1,5,6) :
                fault_kind==2 ? 0xffffffff : fault_kind==3 ? 0x00000073 : 0x00100073;
            b.memory[at++]=csr(0x340,5,19,7);
            b.memory[at++]=csr(0x300,2,0,8);
            b.memory[at++]=csr(0xb02,2,0,9);
            unsigned h=0x400/4;
            b.memory[h++]=csr(0x342,2,0,10);
            b.memory[h++]=csr(0x343,2,0,11);
            b.memory[h++]=csr(0x341,2,0,12);
            b.memory[h++]=instruction(2,12,12,0,4);
            b.memory[h++]=csr(0x341,1,12,0);
            b.memory[h++]=0x30200073;
            b.memory[h]=csr(0x340,5,31,0);
            const unsigned prior=b.traps; b.reset(); run(b,at*4,rng);
            b.require(b.traps==prior+1,"trap/return count");
            b.require(b.regs[10]==(fault_kind<3 ? 2u:fault_kind==3 ? 11u:3u),"handler mcause");
            b.require(b.regs[11]==(fault_kind<3 ? b.memory[fault_pc/4]:fault_kind==3 ? 0:fault_pc),"handler mtval");
            b.require(b.regs[12]==fault_pc+4,"handler mepc increment");
            b.coverage["trap_return_"+std::to_string(fault_kind)]++;
            if (fault_kind>=3) b.coverage["environment_offset_"+std::to_string(offset)]++;
        }
        // An older unresolved branch discards the waiting wrong-path descriptor without CSR effects.
        for (uint32_t insn:{csr(0x340,5,31,0),0x10500073u}) {
            initialize(b); b.memory[31]=instruction(27,0,0,0,0x800-124);
            b.memory[32]=insn; b.memory[0x800/4]=csr(0x340,2,0,18);
            b.reset();
            for (unsigned n=0;n<150;n++) { Input i; i.grant=false; b.tick(i); }
            run(b,0x804,rng); b.require(b.regs[18]==0,"wrong-path CSR state"); b.coverage[insn==0x10500073 ? "wrong_path_wfi":"wrong_path_system"]++;
        }
        // Held serial responses cannot commit while either retirement mask blocks lane zero.
        for (uint32_t insn:{csr(0x340,1,5,5),0x10500073u}) for (unsigned mask:{0u,2u}) {
            initialize(b); b.memory[31]=insn; b.memory[32]=csr(0x340,2,0,6);
            b.reset(); wait_prepared(b); const auto order=b.order;
            for (unsigned n=0;n<20;n++) { Input i; i.retire=mask; b.tick(i); }
            b.require(b.order==order,"held system retired"); run(b,132,rng);
            b.coverage["hold_mask_"+std::to_string(mask)]++;
            if (insn==0x10500073) b.coverage["wfi_hold_"+std::to_string(mask)]++;
        }
        // Cancel a prepared response, then read the CSR to prove it had no architectural effect.
        for (uint32_t insn:{csr(0x340,5,31,0),0x10500073u}) for (unsigned reset=0;reset<2;reset++) {
            initialize(b); b.memory[31]=insn; b.memory[0x800/4]=csr(0x340,2,0,19);
            b.memory[0x804/4]=csr(0xb02,2,0,20);
            b.reset(); wait_prepared(b);
            Input i; i.reset=reset; i.flush=!reset; i.target=0x800; b.tick(i);
            if (reset) { i={}; i.flush=true; i.target=0x800; b.tick(i); }
            run(b,0x808,rng); b.require(b.regs[19]==0,"canceled CSR state"); b.coverage[reset ? "reset_system":"flush_system"]++;
            if (insn==0x10500073) b.coverage[reset ? "reset_wfi":"flush_wfi"]++;
        }
        for (uint32_t insn:{csr(0x340,5,31,0),0x10500073u}) {
            initialize(b); b.memory[31]=insn; b.reset(); wait_prepared(b);
            Input bad; bad.retire=0; bad.inject_bad=true; b.tick(bad);
            const auto before=b.order;
            for (unsigned n=0;n<8;n++) { Input i; i.trap_ready=true; b.tick(i); }
            b.require(b.fatal && b.order==before && !b.d.system_busy_o,"fatal cancellation"); b.coverage["fatal_system"]++;
            if (insn==0x10500073) b.coverage["fatal_wfi"]++;
        }
        // MRET redirects must tolerate both offered and accepted stale instruction traffic.
        for (unsigned kind=0;kind<2;kind++) {
            b.memory.fill(instruction(2,0,0,0,0));
            b.memory[0]=instruction(0,1,0,0,0x1000); b.memory[1]=instruction(2,1,1,0,-2048);
            b.memory[2]=csr(0x341,1,1,0);
            for (unsigned n=3;n<7;n++) b.memory[n]=instruction(2,0,0,0,0);
            b.memory[7]=0x30200073; b.memory[0x800/4]=csr(0x340,5,7,0); b.memory[0x804/4]=0x0000000f;
            b.reset(); bool ready=false;
            for (unsigned n=0;n<1500;n++) {
                Input i; i.retire=b.system_prepared && b.system_expected.op==30 ? 0:3;
                if (!kind) i.request_ready=!(b.held || (b.d.request_valid_o && get32(b.d.request_o,REQUEST_ADDRESS_OFFSET,32)>=32));
                i.latency=kind ? 100:0; b.tick(i);
                if (b.system_prepared && b.system_expected.op==30 && b.d.system_busy_o && (kind ? b.pending && b.pending_address==32:b.held)) { ready=true; break; }
            }
            b.require(ready,"missing MRET stale fetch traffic");
            Input i; i.request_ready=kind; i.grant=false; b.tick(i);
            b.require(b.pc==0x800,"MRET redirect not accepted"); run(b,0x804,rng);
            b.coverage[kind ? "mret_pending_fetch":"mret_offered_fetch"]++;
        }
        for (unsigned repeat=0;repeat<3;repeat++) {
            initialize(b); unsigned at=31;
            for (unsigned n=0;n<400;n++) {
                const unsigned kind=std::array<unsigned,6>{1,2,3,5,6,7}[rng()%6];
                b.memory[at++]=n%11==0 ? 0x10500073 : csr(n%8==0 ? 0xb02:0x340,kind,rng()%16,rng()%16);
                b.memory[at++]=instruction(2,20,1+rng()%15,0,rng()%1024);
            }
            b.memory[at++]=instruction(2,0,0,0,0);
            b.reset(); run(b,at*4,rng); b.coverage["random_program"]++;
        }
        std::cout<<"SYSTEM CORE PASS seed="<<seed<<" cycles="<<b.cycles<<" retired="<<b.retired<<" traps="<<b.traps;
        for (const auto& [name,count]:b.coverage) std::cout<<' '<<name<<'='<<count;
        std::cout<<'\n';
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
