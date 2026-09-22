#include "Vhead_system_backend.h"
#include "head_system_reference.hpp"
#include "backend_event_layout.hpp"
#include "verilated.h"
#include <algorithm>
#include <deque>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>

using Event=std::array<uint32_t,(EVENT_BITS+31)/32>;
template<class T> void put(T& data,unsigned offset,unsigned width,uint64_t value) {
    for (unsigned b=0;b<width;b++) {
        unsigned p=offset+b; data[p/32]=(data[p/32]&~(1u<<(p%32)))|unsigned((value>>b)&1)<<(p%32);
    }
}
template<class T> uint64_t get(const T& data,unsigned offset,unsigned width) {
    uint64_t v=0; for (unsigned b=0;b<width;b++) v|=uint64_t((data[(offset+b)/32]>>((offset+b)%32))&1)<<b; return v;
}
struct Entry {
    unsigned id=0,rd=0,rs=0,tag=0,source=0,stale=0;
    uint32_t pc=0,insn=0;
    bool system=false,branch=false,done=false,resolved=false;
    Event event{};
};
struct Input {
    bool reset=false,flush=false,drain=false,resolve=false,mispredict=false,offer=true,stale=false,illegal_ready=true,trap_ready=true;
    unsigned count=0,retire=3;
    std::array<unsigned,2> rd{},rs{};
    unsigned system=0,branch=0;
    std::array<uint32_t,2> insn{};
    std::array<int,2> complete{-1,-1};
    std::array<uint32_t,2> data{};
    bool raw_fault=false,noise=false;
};
struct Bench {
    Vhead_system_backend d;
    CsrModel bank;
    std::deque<Entry> queue;
    std::array<unsigned,32> committed{},uses{};
    std::array<uint32_t,64> payload{};
    std::array<bool,64> known{};
    std::map<std::string,uint64_t> coverage;
    unsigned tail=0,sequence=0,cycles=0;
    uint64_t order=0;
    bool pending=false;
    Reply held;
    Entry saved;
    Event base{};
    Bench() { for (unsigned n=0;n<32;n++) committed[n]=n; known[0]=true; }
    void require(bool v,const std::string& why) const {
        if (!v) throw std::runtime_error("head system mismatch cycle="+std::to_string(cycles)+" "+why);
    }
    std::array<unsigned,32> map() const {
        auto out=committed; for (const auto& e:queue) if (e.rd) out[e.rd]=e.tag; return out;
    }
    uint64_t free() const {
        uint64_t result=~uint64_t(1);
        for (unsigned a=1;a<32;a++) result&=~(uint64_t(1)<<committed[a]);
        for (const auto& e:queue) if (e.rd) result&=~(uint64_t(1)<<e.tag);
        return result;
    }
    uint64_t ready() const {
        uint64_t result=1;
        for (unsigned a=1;a<32;a++) result|=uint64_t(1)<<committed[a];
        for (const auto& e:queue) if (e.rd && e.done && !get(e.event,TRAP_OFFSET,1)) result|=uint64_t(1)<<e.tag;
        return result;
    }
    Event event(const Entry& e,uint32_t value,bool fault=false) const {
        Event out{};
        put(out,VALID_OFFSET,1,1); put(out,PRIVILEGE_OFFSET,2,3);
        put(out,INSTRUCTION_OFFSET,32,e.insn); put(out,PC_BEFORE_OFFSET,32,e.pc);
        put(out,PC_AFTER_OFFSET,32,fault ? e.pc:e.pc+4);
        put(out,RS1_ADDR_OFFSET,5,e.rs); put(out,RS1_VALUE_OFFSET,32,e.rs ? payload[e.source]:0);
        put(out,RETIRED_OFFSET,1,!fault);
        if (fault) { put(out,TRAP_OFFSET,1,1); put(out,TRAP_CAUSE_OFFSET,32,2); put(out,TRAP_VALUE_OFFSET,32,e.insn); }
        else if (e.rd) { put(out,RD_ADDR_OFFSET,5,e.rd); put(out,RD_VALUE_OFFSET,32,value); put(out,RD_WRITE_MASK_OFFSET,32,UINT32_MAX); }
        return out;
    }
    Event final_event(bool trap) const {
        Event out=base;
        put(out,VALID_OFFSET,1,1); put(out,RETIRED_OFFSET,1,!trap);
        put(out,PC_AFTER_OFFSET,32,held.next);
        if (!trap && saved.rd) { put(out,RD_VALUE_OFFSET,32,held.value); put(out,RD_WRITE_MASK_OFFSET,32,UINT32_MAX); }
        for (unsigned e=0;e<4;e++) for (unsigned f=0;f<7;f++)
            put(out,e*EFFECT_BITS+FIELD_OFFSET[f],FIELD_WIDTH[f],held.effects[e][f]);
        return out;
    }
    template<class T> void compare(const T& got,unsigned slot,const Event& want,const std::string& name) {
        for (unsigned b=0;b<EVENT_BITS;b++) require(get(got,slot*EVENT_BITS+b,1)==get(want,b,1),name+" bit="+std::to_string(b));
    }
    void tick(Input in={}) {
        cycles++;
        const bool active=!in.reset && !in.flush;
        const bool recovery=active && in.resolve && in.mispredict;
        const bool fault=active && !queue.empty() && queue.front().done && get(queue.front().event,TRAP_OFFSET,1);
        const bool running=active && !recovery;
        const bool serial=running && pending && held.legal && !held.trap;
        const bool illegal=running && pending && !held.legal;
        const bool trap=running && pending && held.trap;
        const bool serial_accept=serial && (in.retire&1);
        const bool illegal_accept=illegal && in.illegal_ready;
        const bool trap_accept=trap && in.trap_ready;
        auto candidate=std::find_if(queue.begin(),queue.end(),[](const auto& e){return e.system && !e.done;});
        const bool offered=in.offer && candidate!=queue.end();
        const unsigned offered_id=offered ? candidate->id^(in.stale ? 32:0):0;
        const unsigned source=offered && !in.noise ? candidate->source:0;
        const bool prepare_system=running && !pending && !fault && offered && !queue.empty()
            && queue.front().id==offered_id && ((ready()>>source)&1);
        const bool prepare_trap=running && !pending && fault;
        unsigned rv=0;
        if (serial) rv=1;
        else if (running && !queue.empty() && queue[0].done && !fault && (!queue[0].branch || queue[0].resolved)) {
            rv=1;
            if (queue.size()>1 && !queue[0].system && !queue[1].system && queue[1].done
                && !get(queue[1].event,TRAP_OFFSET,1) && (!queue[1].branch || queue[1].resolved)) rv=3;
        }
        unsigned retired=rv&in.retire; if (!(retired&1)) retired=0;
        std::array<Event,2> completed{};
        unsigned ca=0,wb=serial_accept && saved.rd ? 1:0;
        auto live_ready=ready();
        if (wb) live_ready|=uint64_t(1)<<saved.tag;
        for (unsigned n=0;n<2;n++) if (in.complete[n]>=0) {
            const auto& e=queue.at(unsigned(in.complete[n])); completed[n]=event(e,in.data[n],in.raw_fault);
            if (active && !serial && !illegal && !trap_accept && !e.done
                && (!recovery || in.complete[n]==0)) {
                ca|=1u<<n;
                if (e.rd && !in.raw_fault) { wb|=1u<<n; live_ready|=uint64_t(1)<<e.tag; }
            }
        }
        auto rat=map(); auto avail=free(); std::array<Entry,2> allocated{};
        for (unsigned n=0;n<in.count;n++) {
            auto& e=allocated[n]; e.rd=in.rd[n];e.rs=in.rs[n];e.source=rat[e.rs];e.stale=e.rd ? rat[e.rd]:0;
            e.system=(in.system>>n)&1;e.branch=(in.branch>>n)&1;e.insn=in.insn[n];e.pc=0x800+4*(sequence+n);
            e.id=uses[(tail+n)%32]*32+(tail+n)%32;
            if (e.rd) { for (unsigned p=1;p<64;p++) if ((avail>>p)&1) {e.tag=p;break;} require(e.tag!=0,"stimulus free tag"); avail&=~(uint64_t(1)<<e.tag);rat[e.rd]=e.tag; }
        }
        const unsigned alloc=running && !serial && !trap_accept ? (1u<<in.count)-1:0;
        d.clk_i=0; d.rst_i=in.reset;d.flush_i=in.flush;d.drained_i=in.drain;d.resources_ready_i=1;
        d.valid_i=(1u<<in.count)-1;d.solo_i=in.system;d.cfi_i=in.branch;d.rs1_i=in.rs[0]|in.rs[1]<<5;d.rs2_i=0;d.rd_i=in.rd[0]|in.rd[1]<<5;
        d.pc_i=uint64_t(0x800+4*sequence)|(uint64_t(0x804+4*sequence)<<32);
        d.complete_offer_i=0;d.complete_solo_i=0;d.complete_id_i=0;
        for (unsigned w=0;w<(2*EVENT_BITS+31)/32;w++) d.complete_event_i[w]=0;
        for (unsigned n=0;n<2;n++) if (in.complete[n]>=0) {
            d.complete_offer_i|=1u<<n;d.complete_id_i|=queue.at(unsigned(in.complete[n])).id<<(13*n);
            for (unsigned b=0;b<EVENT_BITS;b++) put(d.complete_event_i,n*EVENT_BITS+b,1,get(completed[n],b,1));
        }
        d.resolve_offer_i=in.resolve;d.resolve_grant_i=1;d.resolve_id_i=queue.empty()?0:queue.front().id;d.mispredict_i=in.mispredict;
        d.retire_ready_i=in.retire;d.trap_ready_i=in.trap_ready;d.illegal_ready_i=in.illegal_ready;
        d.system_valid_i=offered;d.system_id_i=offered_id;d.system_instruction_i=offered?(candidate->insn^(in.noise?0x100000u:0)):0;
        d.read_address_i=source;
        const unsigned audit=cycles%64; d.read_address_i|=(audit<<6)|(audit<<12)|(audit<<18);
        d.eval();
        require(d.occupancy_o==queue.size(),"occupancy");
        require(d.system_prepare_o==prepare_system && d.system_busy_o==(running && pending),"preparation/busy");
        require(d.serial_offer_o==serial && d.serial_accept_o==serial_accept,"serial handshake");
        require(d.illegal_offer_o==illegal && d.illegal_accept_o==illegal_accept,"illegal handshake");
        require(d.trap_valid_o==trap && d.trap_accept_o==trap_accept,"trap handshake");
        require(d.allocate_accept_o==alloc && d.complete_accept_o==ca && d.wb_accept_o==wb,
            "backend acceptance alloc="+std::to_string(d.allocate_accept_o)+"/"+std::to_string(alloc)
            +" complete="+std::to_string(d.complete_accept_o)+"/"+std::to_string(ca)
            +" wb="+std::to_string(d.wb_accept_o)+"/"+std::to_string(wb));
        require(d.retire_valid_o==rv && d.retire_accept_o==retired,"retirement prefix");
        const bool redirect=trap_accept || (serial_accept && saved.insn==0x30200073);
        require(d.redirect_o==redirect && d.redirect_pc_o==(redirect?held.next:0),"accepted redirect");
        if (!in.reset) {
            auto expected=map(); require(d.free_o==free() && d.ready_o==ready(),"register ownership");
            for (unsigned a=0;a<32;a++) require(get(d.rat_o,a*6,6)==expected[a] && get(d.committed_o,a*6,6)==committed[a],"register mapping");
        }
        for (unsigned n=0;n<in.count;n++) if ((alloc>>n)&1) {
            const auto& e=allocated[n];require(((d.allocate_id_o>>(13*n))&8191)==e.id,"allocation identity");
            require(((d.destination_o>>(6*n))&63)==e.tag && ((d.source1_o>>(6*n))&63)==e.source && ((d.stale_o>>(6*n))&63)==e.stale,"allocation tags");
        }
        Event serial_event{},illegal_event{},trap_event{};
        if (serial) serial_event=final_event(false);
        if (illegal) { illegal_event=base;put(illegal_event,RETIRED_OFFSET,1,0);put(illegal_event,PC_AFTER_OFFSET,32,saved.pc);put(illegal_event,RD_ADDR_OFFSET,5,0);
            put(illegal_event,TRAP_OFFSET,1,1);put(illegal_event,TRAP_CAUSE_OFFSET,32,2);put(illegal_event,TRAP_VALUE_OFFSET,32,saved.insn); }
        if (trap) trap_event=final_event(true);
        compare(d.serial_event_o,0,serial_event,"serial event");compare(d.illegal_event_o,0,illegal_event,"illegal event");compare(d.trap_event_o,0,trap_event,"trap event");
        for (unsigned n=0;n<2;n++) if ((rv>>n)&1) {auto e=serial?serial_event:queue[n].event;put(e,ORDER_OFFSET,64,order+n);compare(d.retire_event_o,n,e,"retire event");}
        for (unsigned port=0;port<4;port++) {
            unsigned p=port?audit:source; uint32_t value=payload[p];bool defined=known[p];
            if (serial_accept && saved.rd && p==saved.tag) {value=held.value;defined=true;coverage["serial_bypass"]++;}
            for (unsigned n=0;n<2;n++) if ((ca>>n)&1) {const auto& e=queue.at(unsigned(in.complete[n]));if (e.rd && !in.raw_fault && e.tag==p) {value=in.data[n];defined=true;}}
            if (defined) require(get(d.read_data_o,port*32,32)==value,"PRF data");
            bool r=active && !trap_accept && ((live_ready>>p)&1);
            if (recovery) for (unsigned n=1;n<queue.size();n++) if (queue[n].rd && queue[n].tag==p) r=false;
            require(bool((d.read_ready_o>>port)&1)==r,"PRF ready");
        }
        Reply prepared;Entry next_saved;Event next_base{};
        if (prepare_system || prepare_trap) {
            next_saved=queue.front();Command cmd;cmd.pc=next_saved.pc;cmd.instruction=next_saved.insn;
            if (prepare_system) {require(!next_saved.rs || known[next_saved.source],"undefined source");cmd.source=next_saved.rs?payload[next_saved.source]:0;next_base=event(next_saved,0);put(next_base,RETIRED_OFFSET,1,0);put(next_base,PC_AFTER_OFFSET,32,next_saved.pc);put(next_base,RD_WRITE_MASK_OFFSET,32,0);}
            else {next_base=next_saved.event;put(next_base,ORDER_OFFSET,64,order);put(next_base,RETIRED_OFFSET,1,0);cmd.trap=true;cmd.cause=get(next_base,TRAP_CAUSE_OFFSET,32);cmd.value=get(next_base,TRAP_VALUE_OFFSET,32);}
            prepared=bank.propose(cmd);
        }
        const Reply* accepted=serial_accept||illegal_accept||trap_accept?&held:nullptr;
        bank.advance(in.reset,serial_accept?0:unsigned(__builtin_popcount(retired)),accepted);
        coverage["serial"]+=serial_accept;coverage["illegal"]+=illegal_accept;coverage["trap"]+=trap_accept;
        coverage["held_serial"]+=serial&&!serial_accept;coverage["held_illegal"]+=illegal&&!illegal_accept;coverage["held_trap"]+=trap&&!trap_accept;
        coverage["held_input_change"]+=pending&&in.noise;
        coverage["dual_retire"]+=retired==3;coverage["mret"]+=serial_accept&&saved.insn==0x30200073;
        coverage["stale_descriptor"]+=offered&&in.stale;coverage["nonhead_descriptor"]+=offered&&!queue.empty()&&offered_id!=queue.front().id;
        coverage["cancel_pending"]+=pending&&(in.flush||in.reset||recovery);
        if (in.reset) {
            for (unsigned a=0;a<32;a++) committed[a]=a;
            queue.clear();uses.fill(0);tail=sequence=0;order=0;pending=false;
        } else {
            if (serial_accept && saved.rd) {payload[saved.tag]=held.value;known[saved.tag]=true;}
            for (unsigned n=0;n<2;n++) if ((ca>>n)&1) {auto& e=queue.at(unsigned(in.complete[n]));e.done=true;e.event=completed[n];if(e.rd&&!in.raw_fault){payload[e.tag]=in.data[n];known[e.tag]=true;}}
            if (illegal_accept) {queue.front().done=true;queue.front().event=illegal_event;}
            order+=__builtin_popcount(retired)+unsigned(trap_accept);
            if (in.flush || trap_accept) {queue.clear();tail=0;}
            else if (recovery) {queue.front().resolved=true;tail=(queue.front().id%32+1)%32;queue.erase(queue.begin()+1,queue.end());}
            else {
                if (in.resolve) queue.front().resolved=true;
                for (unsigned n=0;n<2;n++) if ((retired>>n)&1) {auto e=queue.front();if(e.rd)committed[e.rd]=e.tag;queue.pop_front();}
                for (unsigned n=0;n<in.count;n++) if ((alloc>>n)&1) {queue.push_back(allocated[n]);uses[tail]++;tail=(tail+1)%32;sequence++;}
            }
            if (in.drain) {require(queue.empty(),"drain empty");uses.fill(0);coverage["drain"]++;}
            if (in.flush||recovery||accepted) pending=false;
            if (prepare_system||prepare_trap) {held=prepared;saved=next_saved;base=next_base;pending=true;}
        }
        d.clk_i=1;d.eval();d.clk_i=0;d.eval();
    }
    void drain() { for(unsigned n=0;!queue.empty();n++) {require(n<150,"progress watchdog");Input i;for(unsigned j=0,l=0;j<queue.size()&&l<2;j++) if(!queue[j].done&&!queue[j].system) {i.complete[l]=int(j);i.data[l++]=0x12340000+j;}tick(i);}tick(); }
    void write_reg(unsigned rd,uint32_t value) { Input i;i.count=1;i.rd[0]=rd;i.insn[0]=0x13;tick(i);i={};i.complete[0]=0;i.data[0]=value;tick(i);tick();tick(); }
    void command(uint32_t insn,unsigned stalls=2,unsigned cancel=0) {
        require(queue.empty(),"command starts empty");
        if (*std::max_element(uses.begin(),uses.end())>=200) {Input quiescent;quiescent.drain=true;tick(quiescent);}
        Input i;i.count=1;i.system=1;i.insn[0]=insn;
        i.rd[0]=(insn>>7)&31;i.rs[0]=(insn&(1u<<14))?0:(insn>>15)&31;tick(i);
        i={};i.stale=true;tick(i);i={};tick(i);require(pending,"head request not prepared");
        for(unsigned n=0;n<stalls;n++) {i={};i.retire=n%2?2:0;i.illegal_ready=false;i.trap_ready=false;i.noise=true;tick(i);}
        if(cancel) {i={};i.flush=cancel==1;i.reset=cancel==2;tick(i);tick();} else drain();
    }
    void audit() {for(const auto& c:CSR_SPEC) command(csr(c.address),1);}
};
int main(int argc,char** argv) {
    Verilated::commandArgs(argc,argv);
    try {
        unsigned seed=argc>1?std::stoul(argv[1]):1;std::mt19937 rng(seed);Bench b;
        Input i;i.reset=true;b.tick(i);
        for(unsigned r=1;r<32;r++) b.write_reg(r,0x12340000+r);
        b.audit();
        for(const auto& c:CSR_SPEC) for(unsigned kind:{1,2,3,5,6,7}) for(unsigned rs:{0,7,31}) for(unsigned rd:{0,5}) {
            b.command(csr(c.address,kind,rs,rd),rng()%5);b.coverage["kind_"+std::to_string(kind)]++;
        }
        for(unsigned addr=0;addr<4096;addr++) {b.command(csr(addr,2,0,0),0);b.coverage["address_sweep"]++;if(addr%512==0){i={};i.drain=true;b.tick(i);}}
        b.write_reg(7,0x80);b.command(csr(0x300,1,7,0));b.command(0x30200073,4);
        for(unsigned n=0;n<40;n++) {
            b.write_reg(7,0x100+n*4);b.command(csr(0x305,1,7,0));
            b.write_reg(7,0x800+n*4);b.command(csr(0x341,1,7,0));
            b.write_reg(7,n%2?8:0);b.command(csr(0x300,1,7,0));
            b.command(0x30200073,5,n%7==0?1:0);
            b.command(csr(0xfff),3);b.audit();
        }
        for(unsigned cancel:{1,2}) for(uint32_t insn:{csr(0x340,1,7,5),csr(0xfff),0x30200073u}) {
            b.command(insn,3,cancel);b.audit();b.coverage["cancel_"+std::to_string(cancel)]++;
        }
        for(unsigned round=0;round<50;round++) {
            i={};i.count=2;i.rd={7,7};i.system=2;i.rs[1]=7;i.insn={0x13,csr(0x340,1,7,7)};b.tick(i);
            for(unsigned n=0;n<4;n++) b.tick();
            i={};i.complete[0]=0;i.data[0]=rng();b.tick(i);b.tick();b.drain();
            b.coverage["older_dependency"]++;
            i={};i.count=2;i.rd={3,4};i.insn={0x13,0x13};b.tick(i);i={};i.complete={0,1};i.data={uint32_t(rng()),uint32_t(rng())};b.tick(i);b.drain();
        }
        b.audit();
        // An older branch may kill the queued descriptor before it reaches the head.
        i={};i.count=1;i.branch=1;b.tick(i);i={};i.count=1;i.system=1;i.rd[0]=5;i.insn[0]=csr(0x340);b.tick(i);
        i={};i.resolve=true;i.mispredict=true;b.tick(i);b.drain();b.coverage["branch_kill"]++;
        // Raw fault entry and cancellation between illegal completion and trap acceptance.
        for(unsigned mode=0;mode<3;mode++) {
            i={};i.count=1;i.rd[0]=6;i.insn[0]=0xdeadbeef;b.tick(i);i={};i.complete[0]=0;i.raw_fault=true;b.tick(i);
            b.tick();for(unsigned n=0;n<5;n++){i={};i.trap_ready=false;b.tick(i);}
            i={};i.flush=mode==1;i.reset=mode==2;b.tick(i);b.drain();b.coverage["raw_trap_"+std::to_string(mode)]++;
        }
        for(unsigned r=1;r<32;r++) b.write_reg(r,r*37);
        for(unsigned n=0;n<1500;n++) {
            unsigned kind=std::array<unsigned,6>{1,2,3,5,6,7}[rng()%6];
            b.command(csr(CSR_SPEC[rng()%CSR_SPEC.size()].address,kind,rng()%32,rng()%32),rng()%8,n%29==0?1:0);
            if(n%100==0){b.audit();i={};i.drain=true;b.tick(i);}
        }
        b.audit();
        std::cout<<"HEAD SYSTEM PASS seed="<<seed<<" cycles="<<b.cycles;
        for(const auto& [name,count]:b.coverage) std::cout<<' '<<name<<'='<<count;
        std::cout<<'\n';
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
