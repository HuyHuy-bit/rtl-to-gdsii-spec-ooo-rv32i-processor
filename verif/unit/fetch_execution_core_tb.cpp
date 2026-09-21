#include "fetch_execution_reference.hpp"

int main(int argc,char** argv) {
    Verilated::commandArgs(argc,argv);
    try {
        Bench b;
        if (argc>1 && std::string(argv[1])=="negative") {
            b.reset(); b.d.rst_i=0; b.d.enable_i=1; b.d.drained_i=1;
            b.d.clk_i=0; b.d.eval(); b.d.clk_i=1; b.d.eval();
            throw std::runtime_error("missing drain assertion");
        }
        unsigned seed=argc>1 ? std::stoul(argv[1]) : 1;
        std::mt19937 rng(seed);
        program(b,rng); b.reset(); b.run_to(0x1000,rng,true);
        for (unsigned iteration=0;iteration<8;++iteration) { program(b,rng); b.reset(); b.run_to(0x1000,rng); }
        // A sequential not-taken stream must not redirect, including lane-one branches.
        b.memory.fill(0); b.memory[0]=instruction(2,1,0,0,1);
        for (unsigned n=1;n<32;++n) b.memory[n]=instruction(21,0,1,0,8);
        b.reset(); unsigned redirects=b.coverage["branch_redirect"]; b.run_to(128,rng);
        b.require(b.coverage["branch_redirect"]==redirects,"not-taken prediction caused redirect");
        // Wrong-path instruction access failure must be discarded when the older jump resolves.
        b.memory.fill(instruction(2,0,0,0,0)); b.memory[0x1000/4]=0;
        b.memory[0]=instruction(27,0,0,0,0x1000); b.error_address=32; b.reset();
        for (unsigned n=0;n<60;++n) { Input i; i.grant=false; b.tick(i); }
        b.require(b.d.fetch_fault_o,"wrong-path fetch fault reached frontend");
        b.run_to(0x1000,rng); b.coverage["wrong_path_fault_recovered"]++; b.error_address=UINT32_MAX;
        // Restart flush preserves committed values and global order while killing speculative work.
        program(b,rng); b.reset();
        for (unsigned n=0;n<40;++n) b.tick();
        Input i; i.flush=true; i.target=0; b.tick(i); b.run_to(0x1000,rng);
        i={}; i.enable=false; i.flush=true; i.target=0x1000; b.tick(i);
        i.flush=false; i.drain=true; b.tick(i); b.tick();
        b.require(!b.d.identity_drain_o,"identity recycle stayed blocked");
        for (uint32_t target : {2U,0x10000000U}) {
            i={}; i.flush=true; i.target=target; b.tick(i);
            for (unsigned n=0;n<8;++n) b.tick();
            b.require(b.d.fetch_fault_o,"local fetch fault missing");
        }
        b.memory.fill(0); b.memory[0]=instruction(27,1,0,0,2); b.reset();
        for (unsigned n=0;n<40;++n) b.tick();
        b.require(b.d.backend_fault_o,"target fault missing");
        program(b,rng); b.reset();
        for (unsigned n=0;n<50;++n) { i={}; i.retire=0; b.tick(i); }
        i={}; i.retire=0; i.inject_bad=true; b.tick(i);
        for (unsigned n=0;n<8;++n) b.tick();
        b.reset(); b.run_to(0x1000,rng);
        std::cout<<"FETCH EXECUTION CORE PASS seed="<<seed<<" cycles="<<b.cycles<<" retired="<<b.retired<<" requests="<<b.requests;
        for (const auto& [key,value]:b.coverage) std::cout<<' '<<key<<'='<<value;
        std::cout<<'\n';
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
