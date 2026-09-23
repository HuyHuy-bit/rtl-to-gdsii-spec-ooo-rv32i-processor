#pragma once
#include <cstdint>
#include <stdexcept>

namespace rv32 {
inline bool illegal(uint32_t insn) {
    unsigned opcode=insn&127, f=(insn>>12)&7, upper=insn>>25;
    switch (opcode) {
    case 0x37: case 0x17: case 0x6f: return false;
    case 0x67: return f!=0;
    case 0x03: return f==3 || f==6 || f==7;
    case 0x23: return f>2;
    case 0x63: return f==2 || f==3;
    case 0x13: return (f==1 && upper!=0) || (f==5 && upper!=0 && upper!=32);
    case 0x33: return upper!=0 && !(upper==32 && (f==0 || f==5));
    case 0x0f: return f>1;
    case 0x73:
        if (f==1 || f==2 || f==3 || f==5 || f==6 || f==7) return false;
        return insn!=0x00000073 && insn!=0x00100073 && insn!=0x30200073 && insn!=0x10500073;
    default: return true;
    }
}
inline int operation(uint32_t insn) {
    unsigned opcode = insn&127, f = (insn >> 12)&7, upper = insn >> 25;
    if (opcode == 0x37) return 0;
    if (opcode == 0x17) return 1;
    if (opcode == 0x13) {
        const int ops[8] = {2, 3, 4, 5, 6, 7, 9, 10};
        if (f == 1 && upper != 0) return -1;
        if (f == 5) return upper == 0 ? 7 : upper == 32 ? 8 : -1;
        return ops[f];
    }
    if (opcode == 0x33) {
        const int ops[8] = {11, 13, 14, 15, 16, 17, 19, 20};
        if (upper == 0) return ops[f];
        if (upper == 32) return f == 0 ? 12 : f == 5 ? 18 : -1;
    }
    if (opcode == 0x63) {
        const int branch[8] = {21,22,-1,-1,23,24,25,26};
        return branch[f];
    }
    if (opcode == 0x6f) return 27;
    if (opcode == 0x67 && f == 0) return 28;
    return -1;
}
inline uint32_t instruction(unsigned op, unsigned rd, unsigned a, unsigned b, uint32_t imm = 0) {
    if (op >= 21 && op <= 26) {
        const unsigned function[6] = {0,1,4,5,6,7};
        return ((imm >> 12)&1U) << 31 | ((imm >> 5)&63U) << 25 | b << 20 | a << 15
            | function[op-21] << 12 | ((imm >> 1)&15U) << 8 | ((imm >> 11)&1U) << 7 | 0x63;
    }
    if (op == 27) return ((imm >> 20)&1U) << 31 | ((imm >> 1)&1023U) << 21
        | ((imm >> 11)&1U) << 20 | ((imm >> 12)&255U) << 12 | rd << 7 | 0x6f;
    if (op == 28) return (imm&4095U) << 20 | a << 15 | rd << 7 | 0x67;
    if (op < 2) return (imm&0xfffff000U) | (rd << 7) | (op == 0 ? 0x37U : 0x17U);
    const unsigned function[21] = {0,0,0,1,2,3,4,5,5,6,7,0,0,1,2,3,4,5,5,6,7};
    const unsigned alternate = op == 8 || op == 12 || op == 18 ? 32 : 0;
    if (op <= 10) {
        if (op == 3 || op == 7 || op == 8) imm = (alternate << 5) | (imm&31);
        return ((imm&4095) << 20) | (a << 15) | (function[op] << 12) | (rd << 7) | 0x13;
    }
    return (alternate << 25) | (b << 20) | (a << 15) | (function[op] << 12) | (rd << 7) | 0x33;
}
inline int64_t signed_value(uint32_t v) { return v&0x80000000U ? int64_t(v)-INT64_C(4294967296) : int64_t(v); }
inline uint32_t arithmetic_right(uint32_t a, unsigned shift) {
    shift &= 31;
    uint32_t result = a >> shift;
    if (shift && (a&0x80000000U)) result |= UINT32_MAX << (32-shift);
    return result;
}
inline uint32_t evaluate(unsigned op, uint32_t insn, uint32_t pc, uint32_t a, uint32_t b) {
    uint32_t immediate = insn >> 20;
    if (immediate&2048) immediate |= 0xfffff000U;
    if (op >= 2 && op <= 10) b = immediate;
    switch (op) {
    case 0: return insn&0xfffff000U;
    case 1: return pc+(insn&0xfffff000U);
    case 2: case 11: return a+b;
    case 12: return a-b;
    case 3: case 13: return a << (b&31);
    case 4: case 14: return signed_value(a) < signed_value(b);
    case 5: case 15: return a < b;
    case 6: case 16: return a^b;
    case 7: case 17: return a >> (b&31);
    case 8: case 18: return arithmetic_right(a, b);
    case 9: case 19: return a|b;
    case 10: case 20: return a&b;
    case 21: case 22: case 23: case 24: case 25: case 26: return 0;
    case 27: case 28: return pc+4U;
    default: throw std::runtime_error("unsupported reference operation");
    }
}
inline uint32_t extend(uint32_t value, unsigned bits) {
    return value & (1U << (bits-1)) ? value | (UINT32_MAX << bits) : value;
}
}
