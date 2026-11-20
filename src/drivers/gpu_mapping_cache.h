#ifndef WYZAR_GPU_MAPPING_CACHE_H
#define WYZAR_GPU_MAPPING_CACHE_H
#include <stdint.h>

struct GpuMappingKey { uint64_t address,bytes; };
struct GpuMappingCache {
    GpuMappingKey keys[32]{};
    unsigned count=0;
    uint64_t epoch=0;
    bool valid=false;
    bool matches(const GpuMappingKey* next,unsigned n,uint64_t generation) const {
        // Palette-only work does not change or consume any source mappings.
        if(!n)return true;
        if(!valid || n>32 || count!=n || epoch!=generation)return false;
        for(unsigned i=0;i<n;++i)
            if(keys[i].address!=next[i].address || keys[i].bytes!=next[i].bytes)return false;
        return true;
    }
    void record(const GpuMappingKey* next,unsigned n,uint64_t generation) {
        if(!n || n>32)return;
        for(unsigned i=0;i<n;++i)keys[i]=next[i];
        count=n;epoch=generation;valid=true;
    }
};

// Gen8/9 GGTT accesses use private PAT entry zero. Preserve the other entries
// and select normal write-back LLC caching (GEN8_PPAT_WB | GEN8_PPAT_LLC).
static inline uint32_t gpu_pat0_wb_llc(uint32_t old_value) {
    return (old_value&~0xFFu)|0x07u;
}
static inline uint32_t gpu_pat0_uncached(uint32_t old_value) {return old_value&~0xFFu;}
#endif
