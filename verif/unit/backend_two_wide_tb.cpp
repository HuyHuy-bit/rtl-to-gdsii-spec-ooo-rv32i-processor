#include "backend_reference.hpp"

void empty(Check& c,std::mt19937& rng) {
    for(unsigned limit=0;!c.queue.empty();++limit) {
        c.require(limit<200,"drain progress"); Input in; in.trap_ready=true;
        for(unsigned i=0,lane=0;i<c.queue.size()&&lane<2;++i) if(!c.queue[i].done) {
            in.complete[lane]=c.result(i,rng);in.read[lane]=c.queue[i].destination;++lane;
        }
        for(auto e:c.queue) if(e.cfi&&!e.resolved) {in.resolve=true;in.resolve_id=e.id;break;}
        c.run(in);
    }
}
void directed(Check& c,std::mt19937& rng) {
    Input in;in.reset=true;c.run(in);
    for(unsigned r=1;r<32;++r) {in={};in.count=1;in.rd[0]=r;c.run(in);empty(c,rng);}
    in={};in.count=2;in.rd={5,5};in.rs1={5,5};in.rs2={5,5};c.run(in);
    in={};in.complete[0]=c.result(1,rng);in.read[0]=c.queue[1].destination;c.run(in);
    in={};in.complete[0]=c.result(0,rng);in.read[0]=c.queue[0].destination;c.run(in);
    in={};in.retire_ready=2;c.run(in);in.retire_ready=1;c.run(in);empty(c,rng);
    in={};in.count=2;in.rd={6,7};c.run(in);
    in={};in.complete={c.result(0,rng),c.result(1,rng)};in.complete[1].solo=true;c.run(in);
    c.run();c.run();
    in={};in.count=1;in.rd[0]=1;in.cfi=1;c.run(in);
    in={};in.count=2;in.rd={2,3};c.run(in);
    in={};in.complete={c.result(1,rng),c.result(2,rng)};c.run(in);
    in={};in.resolve=true;in.mispredict=true;in.resolve_id=c.queue.front().id;
    in.complete[0]=c.result(0,rng);in.read={c.queue[1].destination,c.queue[2].destination,0,0};c.run(in);
    empty(c,rng);
    in={};in.reset=true;c.run(in);
    for(unsigned i=0;i<16;++i){in={};in.count=2;in.retire_ready=0;c.run(in);}
    in={};in.count=2;c.run(in);
    for(int i=28;i>=0;i-=2){in={};in.retire_ready=0;in.complete={c.result(i,rng),c.result(i+1,rng)};c.run(in);}
    for(unsigned i=0;i<15;++i)c.run();
    in={};in.count=2;in.cfi=1;in.rd[0]=1;c.run(in);
    in={};in.count=2;in.cfi=2;in.rd={2,3};c.run(in);
    const auto killed=c.result(3,rng);
    in={};in.resolve=true;in.mispredict=true;in.resolve_id=c.queue[2].id;
    in.complete={c.result(1,rng),c.result(2,rng)};in.read={c.queue[1].destination,c.queue[2].destination,0,0};c.run(in);
    in={};in.count=2;in.rd={4,5};c.run(in);
    in={};in.complete[0]=killed;in.read[0]=c.queue.back().destination;c.run(in);empty(c,rng);
    for(unsigned i=0;i<8;++i){in={};in.count=1;in.cfi=1;in.rd[0]=i+1;c.run(in);}
    in={};in.count=1;in.cfi=1;in.rd[0]=9;in.resolve=true;in.resolve_id=c.queue[3].id;c.run(in);
    in={};in.count=1;in.cfi=1;in.rd[0]=9;c.run(in);
    in={};in.resolve=true;in.resolve_id=c.queue[3].id;in.mispredict=true;c.run(in);
    in={};in.count=1;in.cfi=1;in.rd[0]=10;in.resolve=true;in.resolve_id=c.queue[6].id;c.run(in);
    in={};in.resolve=true;in.resolve_id=c.queue[1].id;in.mispredict=true;in.complete[0]=c.result(0,rng);c.run(in);empty(c,rng);
    for(unsigned i=0;i<16;++i){in={};in.count=2;in.rd={1,1};in.retire_ready=0;c.run(in);}
    in={};in.count=2;in.rd={1,2};c.run(in);empty(c,rng);
    in={};in.count=2;in.rd={3,4};in.cfi=2;c.run(in);
    in={};in.complete[0]=c.result(0,rng,true);in.read[0]=c.queue[0].destination;c.run(in);
    in={};in.trap_ready=true;in.resolve=true;in.resolve_id=c.queue[1].id;in.mispredict=true;in.count=2;in.rd={5,6};c.run(in);
    in={};in.count=2;in.rd={3,4};c.run(in);
    in={};in.complete[0]=c.result(0,rng);in.complete[1]=in.complete[0];in.read[0]=c.queue[0].destination;c.run(in);
    in={};in.flush=true;in.complete[0]=c.result(1,rng);c.run(in);
    in={};in.count=1;in.rd[0]=1;c.run(in);in={};in.reset=true;c.run(in);
    for(unsigned round=0;round<256;++round){for(unsigned i=0;i<16;++i){in={};in.count=2;c.run(in);}empty(c,rng);}
    in={};in.count=2;c.run(in);in={};in.drain=true;c.run(in);
    in={};in.count=2;in.rd={1,2};c.run(in);empty(c,rng);
}
int main(int argc,char** argv) {
    Verilated::commandArgs(argc,argv);
    try {
        Check c;
        if(argc>1&&std::string(argv[1])=="negative") {Input in;in.reset=true;c.run(in);in={};in.drain=true;in.count=1;c.run(in);throw std::runtime_error("negative survived");}
        unsigned seed=argc>1?std::stoul(argv[1]):1, n=argc>2?std::stoul(argv[2]):20000;
        std::mt19937 rng(seed);directed(c,rng);const auto d=c.cycles;
        for(unsigned step=0;step<n;++step){
            Input in;in.count=rng()%3;in.retire_ready=rng()%4;in.resources=rng()%5!=0;in.trap_ready=rng()%2;
            for(unsigned l=0;l<2;++l){in.rd[l]=rng()%32;in.rs1[l]=rng()%32;in.rs2[l]=rng()%32;}
            if(rng()%5==0)in.cfi=1U<<(rng()%2);
            for(auto& p:in.read)p=rng()%64;
            std::vector<unsigned> undone,branch;
            for(unsigned i=0;i<c.queue.size();++i){if(!c.queue[i].done)undone.push_back(i);if(c.queue[i].cfi&&!c.queue[i].resolved)branch.push_back(i);}
            std::shuffle(undone.begin(),undone.end(),rng);
            for(unsigned l=0;l<2&&l<undone.size();++l)if(rng()%4){in.complete[l]=c.result(undone[l],rng,rng()%100==0);in.read[l]=c.queue[undone[l]].destination;}
            if(!branch.empty()&&rng()%3==0){in.resolve=true;in.resolve_id=c.queue[branch[rng()%branch.size()]].id;in.grant=rng()%4!=0;in.mispredict=rng()%3==0;}
            if(!c.old_ids.empty()&&rng()%9==0){in.complete[0]={};in.complete[0].offer=true;in.complete[0].id=c.old_ids[rng()%c.old_ids.size()];}
            if(rng()%300==0)in.flush=true;
            if(c.queue.empty()&&std::any_of(c.uses.begin(),c.uses.end(),[](unsigned v){return v==256;})){in={};in.drain=true;}
            c.run(in);
        }
        std::cout<<"BACKEND TWO WIDE PASS seed="<<seed<<" cycles="<<c.cycles<<" directed="<<d;
        for(const auto& [k,v]:c.coverage)std::cout<<" "<<k<<"="<<v;
        std::cout<<"\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
