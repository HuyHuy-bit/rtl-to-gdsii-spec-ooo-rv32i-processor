#include "backend_reference.hpp"

static Completion serial_result(Check& c,unsigned index,std::mt19937& rng) {
    auto r=c.result(index,rng);
    put(r.event,VALID_OFFSET,1,1); put(r.event,RETIRED_OFFSET,1,1);
    if (!c.queue[index].rd) put(r.event,RD_VALUE_OFFSET,32,0);
    return r;
}
static void empty(Check& c,std::mt19937& rng) {
    for (unsigned n=0;!c.queue.empty();n++) {
        c.require(n<200,"drain progress"); Input i; i.trap_ready=true;
        for (unsigned at=0,lane=0;at<c.queue.size() && lane<2;at++) if (!c.queue[at].done)
            i.complete[lane++]=c.result(at,rng);
        for (auto e:c.queue) if (e.cfi && !e.resolved) { i.resolve=true; i.resolve_id=e.id; break; }
        c.run(i);
    }
}
int main(int argc,char** argv) {
    Verilated::commandArgs(argc,argv);
    try {
        unsigned seed=argc>1 ? std::stoul(argv[1]):1; std::mt19937 rng(seed); Check c;
        Input i; i.reset=true; c.run(i);
        for (unsigned rd=0;rd<32;rd++) {
            i={}; i.count=1; i.solo=1; i.rd[0]=rd; c.run(i);
            auto packet=serial_result(c,0,rng); const auto tag=c.queue.front().destination;
            for (unsigned ready:{0,2,0,2}) {
                i={}; i.serial=packet; i.retire_ready=ready; i.count=2; i.rd={1,2};
                i.complete={c.result(0,rng),c.result(0,rng)}; i.read={tag,tag,tag,0}; c.run(i);
            }
            i={}; i.serial=packet; i.read={tag,tag,tag,0}; c.run(i); c.run();
            c.require(c.queue.empty(),"serial head not removed");
            i={}; i.serial=packet; c.run(i); c.coverage["repeated_offer"]++;
            c.coverage["rd_"+std::to_string(rd)]++;
        }
        // An older writer must retire before the solo result; head data cannot bypass it.
        for (unsigned n=0;n<80;n++) {
            i={}; i.count=2; i.solo=2; i.rd={5,5}; i.rs1={5,5}; c.run(i);
            auto packet=serial_result(c,1,rng);
            i={}; i.serial=packet; i.complete[0]=c.result(0,rng); c.run(i);
            i={}; i.serial=packet; c.run(i); c.require(c.queue.size()==1,"older retirement");
            i.read={c.queue.front().destination,c.queue.front().stale,0,0}; c.run(i); c.run();
            c.coverage["older_writer"]++;
        }
        // Stale generations and malformed metadata cannot publish or retire a result.
        for (unsigned kind=0;kind<10;kind++) {
            i={}; i.count=1; i.solo=kind==7 ? 0:1; i.cfi=kind==8 ? 1:0; i.rd[0]=kind==6 ? 0:9; c.run(i);
            auto good=serial_result(c,0,rng), bad=good;
            if (kind==0) bad.id ^= 32;
            if (kind==1) put(bad.event,VALID_OFFSET,1,0);
            if (kind==2) put(bad.event,RETIRED_OFFSET,1,0);
            if (kind==3) put(bad.event,TRAP_OFFSET,1,1);
            if (kind==4) put(bad.event,PC_BEFORE_OFFSET,32,c.queue.front().pc+4);
            if (kind==5) put(bad.event,RD_WRITE_MASK_OFFSET,32,1);
            if (kind==9) put(bad.event,RD_ADDR_OFFSET,5,8);
            if (kind==6) put(bad.event,RD_VALUE_OFFSET,32,99);
            i={}; i.serial=bad; c.run(i);
            c.require(!c.queue.empty() && !c.queue.front().done,"bad head result changed ownership");
            if (kind<7 || kind==9) { i.serial=good; c.run(i); } else empty(c,rng);
            c.coverage["reject_"+std::to_string(kind)]++;
        }
        for (unsigned kind=0;kind<2;kind++) {
            i={}; i.count=1; i.solo=1; i.rd[0]=3; c.run(i);
            auto packet=serial_result(c,0,rng);
            i={}; i.serial=packet; i.retire_ready=0; c.run(i);
            i.retire_ready=3; i.flush=kind==0; i.reset=kind==1; c.run(i); c.run();
            i={}; i.serial=packet; c.run(i); c.coverage[kind ? "reset_offer":"flush_offer"]++;
        }
        // Recovery owns the edge; an older solo entry survives a younger branch's cut.
        i={}; i.count=1; i.solo=1; i.rd[0]=7; c.run(i);
        auto packet=serial_result(c,0,rng);
        i={}; i.count=1; i.cfi=1; i.rd[0]=8; c.run(i);
        i={}; i.serial=packet; i.resolve=true; i.mispredict=true; i.resolve_id=c.queue.back().id; c.run(i);
        i={}; i.serial=packet; c.run(i); empty(c,rng); c.coverage["recovery_priority"]++;
        // A younger queued serial result is rejected and killed by an older branch or trap.
        for (unsigned kind=0;kind<2;kind++) {
            i={}; i.count=1; i.cfi=kind==0 ? 1:0; i.rd[0]=8; c.run(i);
            i={}; i.count=1; i.solo=1; i.rd[0]=9; c.run(i); auto younger=serial_result(c,1,rng);
            if (kind==1) { i={}; i.complete[0]=c.result(0,rng,true); i.retire_ready=0; c.run(i); }
            i={}; i.serial=younger; i.trap_ready=kind==1; i.resolve=kind==0; i.mispredict=true; i.resolve_id=c.queue.front().id; c.run(i);
            i={}; i.serial=younger; c.run(i); empty(c,rng); c.coverage[kind ? "trap_priority":"older_branch_kill"]++;
        }
        // Full-size ownership stress includes WAW chains, head stalls, wrap and stale replay.
        for (unsigned round=0;round<120;round++) {
            for (unsigned n=0;n<16;n++) {
                i={}; i.count=2; i.solo=n==0 ? 1:0; i.rd={1,1}; i.retire_ready=0; c.run(i);
            }
            packet=serial_result(c,0,rng);
            for (unsigned n=0;n<5;n++) {
                i={}; i.serial=packet; i.retire_ready=0; i.complete={c.result(1,rng),c.result(2,rng)};
                i.read={c.queue.front().destination,c.queue.front().stale,0,0}; c.run(i);
            }
            i.retire_ready=3; c.run(i); empty(c,rng); c.run();
            i={}; i.serial=packet; c.run(i); c.coverage["full_window"]++;
        }
        for (unsigned n=0;n<1800;n++) {
            i={}; i.count=1; i.solo=1; i.rd[0]=rng()%32; c.run(i);
            packet=serial_result(c,0,rng); const auto tag=c.queue.front().destination;
            for (unsigned k=0;k<rng()%8;k++) { i={}; i.serial=packet; i.retire_ready=rng()%2 ? 0:2; i.read={tag,tag,0,0}; c.run(i); }
            i={}; i.serial=packet; i.read={tag,tag,0,0};
            if (n%17==0) { i.flush=true; c.coverage["random_cancel"]++; }
            c.run(i); c.run(); c.coverage["random_serial"]++;
            if (n%64==0) { i={}; i.drain=true; c.run(i); }
        }
        std::cout<<"SERIAL RETIREMENT PASS seed="<<seed<<" cycles="<<c.cycles;
        for (const auto& [name,count]:c.coverage) std::cout<<' '<<name<<'='<<count;
        std::cout<<'\n';
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
