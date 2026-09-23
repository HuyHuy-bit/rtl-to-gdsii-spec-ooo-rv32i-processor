#pragma once
#include <cstdint>

namespace packed_bits {
template<class T> unsigned bit(const T& data, unsigned offset) {
    return (data[offset/32] >> (offset%32)) & 1U;
}
template<class T> void put(T& data, unsigned offset, unsigned width, uint64_t value) {
    for (unsigned b=0; b<width; b++) {
        const unsigned p=offset+b;
        data[p/32]=(data[p/32]&~(1U<<(p%32))) | (unsigned((value>>b)&1)<<(p%32));
    }
}
template<class T> uint64_t get(const T& data, unsigned offset, unsigned width) {
    uint64_t value=0;
    for (unsigned b=0; b<width; b++) value |= uint64_t(bit(data,offset+b))<<b;
    return value;
}
template<class T> uint32_t get32(const T& data, unsigned offset, unsigned width) {
    return uint32_t(get(data,offset,width));
}
}
