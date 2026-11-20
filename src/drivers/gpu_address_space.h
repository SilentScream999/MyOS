#ifndef WYZAR_GPU_ADDRESS_SPACE_H
#define WYZAR_GPU_ADDRESS_SPACE_H
#include <stdint.h>

struct GpuGttSpan { uint64_t base,bytes,pte; };

// A boot takeover may claim a dedicated interval only above all reconstructed
// display/aperture mappings, with guard pages and the top reservation preserved.
static inline uint64_t gpu_takeover_base(uint64_t prefix,uint64_t end,uint64_t bytes) {
    if((prefix&4095)||(end&4095)||(bytes&4095)||!bytes||prefix>=end)return 0;
    if(end-prefix<8192 || bytes>end-prefix-8192)return 0;
    return prefix+4096;
}

static inline bool gpu_firmware_scratch_pte(uint64_t pte,uint64_t framebuffer_min,
                                           uint64_t framebuffer_max) {
    uint64_t physical=pte&0x0000FFFFFFFFF000ULL;
    return (pte&1) && !(pte&~0x0000FFFFFFFFF00FULL) &&
           (physical<framebuffer_min || physical>framebuffer_max);
}

// Inspect every entry, never infer an entire range from one sampled PTE. A
// uniform run is useful for identifying firmware scratch aliases; the caller
// must also exclude display mappings and validate the scratch page itself.
template<class Read,class Accept>
static inline GpuGttSpan gpu_gtt_largest_run(uint64_t begin,uint64_t end,
                                           Read read,Accept accept,bool uniform) {
    GpuGttSpan best{},run{};
    if((begin&4095)||(end&4095)||begin>=end)return best;
    for(uint64_t address=begin;address<end;address+=4096){
        uint64_t pte=read(address);
        if(!accept(pte)){run={};continue;}
        if(!run.bytes || (uniform && run.pte!=pte))run={address,0,pte};
        run.bytes+=4096;
        if(run.bytes>best.bytes)best=run;
    }
    return best;
}

// Gen9 display register layout, matching Intel's SKL plane and IVB cursor
// definitions in i915. Protect a deliberately oversized prefix beyond every
// active plane's programmed AND live base; do not change display registers.
// Compression/unknown reads cannot establish safe ownership, so reject them.
template<class Read>
static inline bool gpu_firmware_display_prefix(Read read,uint64_t aperture,
                                               uint64_t framebuffer_end,
                                               uint64_t limit,uint64_t* result) {
    uint64_t prefix=0x20000000ULL; // keep at least the lower 512 MiB
    if(prefix<aperture)prefix=aperture;
    if(prefix<framebuffer_end)prefix=framebuffer_end;
    unsigned enabled=0;
    auto protect=[&](uint32_t value,uint64_t bytes){
        if(value==UINT32_MAX)return false;
        uint64_t base=value&0xFFFFF000u;
        if(base>=limit)return false;
        uint64_t end=bytes>limit-base?limit:base+bytes;
        if(prefix<end)prefix=end;
        return true;
    };
    for(unsigned pipe=0;pipe<3;++pipe){
        for(unsigned plane=0;plane<3;++plane){
            uint32_t offset=pipe*0x1000+plane*0x100;
            uint32_t ctl=read(0x70180+offset);
            if(ctl==UINT32_MAX)return false;
            if(!(ctl&(1u<<31)))continue;
            ++enabled;
            // Interpret only linear, unrotated XRGB8888. Tiled, compressed and
            // planar formats need a fuller reconstruction of auxiliary planes.
            if((ctl&(1u<<15)) || (ctl&(7u<<10)) || (ctl&3) ||
               (ctl&(15u<<24))!=(4u<<24))return false;
            uint32_t stride=read(0x70188+offset),size=read(0x70190+offset),origin=read(0x701a4+offset);
            if(stride==UINT32_MAX || size==UINT32_MAX || origin==UINT32_MAX)return false;
            uint64_t pitch=(stride&0xFFF)*64ULL;
            uint64_t width=(size&0xFFFF)+1ULL+(origin&0xFFFF);
            uint64_t rows=(size>>16)+1ULL+(origin>>16);
            if(!pitch || width*4>pitch)return false;
            uint64_t span=(rows+64)*pitch; // generous padding beyond observed geometry
            if(span<0x20000000ULL)span=0x20000000ULL;
            if(!protect(read(0x7019c+offset),span) ||
               !protect(read(0x701ac+offset),span))return false;
        }
        uint32_t cursor=read(0x70080+pipe*0x1000);
        if(cursor==UINT32_MAX)return false;
        if((cursor&0x27) && !protect(read(0x70084+pipe*0x1000),0x200000))return false;
    }
    if(!enabled || prefix>=limit)return false;
    *result=(prefix+4095)&~4095ULL;
    return true;
}

// Search only complete pages inside the mapped GGTT. Existing present PTEs
// belong to firmware/display state and must never be treated as spare space.
template<class Present>
static inline uint64_t gpu_find_gtt_hole(uint64_t begin,uint64_t end,
                                       uint64_t bytes,Present present) {
    if((begin&4095)||(end&4095)||(bytes&4095)||!bytes||begin>=end||bytes>end-begin)return 0;
    uint64_t run=0;
    for(uint64_t address=begin;address<end;address+=4096){
        if(present(address))run=0;else run+=4096;
        if(run>=bytes)return address+4096-run;
    }
    return 0;
}
#endif
