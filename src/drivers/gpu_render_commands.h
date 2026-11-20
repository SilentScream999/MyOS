#ifndef WYZAR_GPU_RENDER_COMMANDS_H
#define WYZAR_GPU_RENDER_COMMANDS_H
#include <stdint.h>
#include "gpu_gen9_pipeline.h"

// Internal BGRA8 texture contract. Coordinates and endpoints are in pixels.
// Alpha is premultiplied. Window opacity remains a separate WM policy.
struct GpuRenderSurface { uint64_t address; uint32_t width,height,pitch; };
struct GpuRenderRect { int x1,y1,x2,y2; };
struct GpuTextureDraw {
    GpuRenderSurface source,destination;
    GpuRenderRect client,clip;
    bool blend=false;
};
// Hardware readback fixtures use binary-exact scaling. Their pixel centers
// stay away from nearest-sampler selection boundaries on every covered pixel.
static GpuTextureDraw gpu_render_probe_draw(unsigned test,uint64_t source,uint64_t destination) {
    unsigned size=test==1?8:16;
    return {{source,size,size,64},{destination,32,32,128},
            {2,2,18,18},{3,3,17,17},test==2};
}
static constexpr unsigned GPU_RENDER_BATCH_BYTES=4096;
static constexpr unsigned GPU_RENDER_COMMAND_BYTES=1144;
static constexpr unsigned GPU_RENDER_VERTEX_OFFSET=2696;
static constexpr unsigned GPU_RENDER_DST_STATE=2112;
static constexpr unsigned GPU_RENDER_SRC_STATE=2176;

static constexpr uint32_t GPU_RENDER_RING_BYTES=32768;
static constexpr uint32_t GPU_RENDER_PACKET_BYTES=256;
static constexpr uint32_t GPU_RENDER_CONTEXT_FLAGS=0x11Du; // full restore, privileged, 48-bit
static bool gpu_render_needs_recycle(uint32_t tail) {
    return tail>=GPU_RENDER_RING_BYTES-GPU_RENDER_PACKET_BYTES;
}

// The serial submitter retires each packet before this storage is reused.
// Packets occupy one aligned 256-byte block, including at the ring boundary.
static bool gpu_render_packet(uint32_t* ring,uint32_t tail,uint32_t fence,
                              uint32_t sequence,bool texture,uint32_t* next) {
    if(!ring||!next||tail>=GPU_RENDER_RING_BYTES||(tail&255)||(fence&7)||
       fence>UINT32_MAX-15)return false;
    auto* out=ring+tail/4;
    unsigned count=0;
    auto emit=[&](uint32_t value){out[count++]=value;};
    auto control=[&](uint32_t flags,uint32_t address,uint32_t data){
        emit(0x7A000004);emit(flags);emit(address);emit(0);emit(data);emit(0);
    };
    // Gen9 context workarounds, applied in the context before TLB invalidation.
    // i915 gen9_ctx_workarounds_init(): HDC force-noncoherent/context-save
    // settings avoid a known TLB-invalidation/PSD-flush system hang. Disable
    // 4x4 STC optimization and partial resolve in the render cache as well.
    // https://codebrowser.dev/linux/linux/drivers/gpu/drm/i915/gt/intel_workarounds.c.html
    emit(0); // MI_NOOP before MI_LOAD_REGISTER_IMM
    emit(0x11001003); // two register/value pairs, FORCE_POSTED
    emit(0x7300);emit(0x80308030); // HDC_CHICKEN0 bits 15,5,4
    emit(0x7004);emit(0x00420042); // CACHE_MODE_1 bits 6,1
    control(0,0,0);
    control(1u<<5,0,0);
    control((1u<<24)|(1u<<20)|(1u<<18)|(1u<<14)|(1u<<11)|(1u<<10)|
            (1u<<4)|(1u<<3)|(1u<<2),fence+8,0);
    // WaForGAMHang:kbl: DC flush BEFORE invalidation, CS stall AFTER it.
    // Moving this trailing stall ahead of invalidation does not implement the
    // workaround. Keep the Gen9 null PC before VF invalidation as well.
    // https://codebrowser.dev/linux/linux/drivers/gpu/drm/i915/gt/gen8_engine_cs.c.html
    // gen8_emit_flush_rcs(). Apply conservatively
    // without relying on a PCI stepping classification during bring-up.
    control(1u<<20,0,0);
    // Pad the 64-bit batch start so the following PIPE_CONTROL is qword aligned.
    if(texture){emit(0x18800101);emit(0x100000);emit(0);emit(0);}
    else {emit(0);emit(0);emit(0);emit(0);}
    control((1u<<24)|(1u<<20)|(1u<<14)|(1u<<12)|(1u<<7)|(1u<<5)|1u,fence,sequence);
    while(count<GPU_RENDER_PACKET_BYTES/4)emit(0);
    *next=(tail+GPU_RENDER_PACKET_BYTES)&(GPU_RENDER_RING_BYTES-1);
    return true;
}

static bool gpu_render_surface_valid(const GpuRenderSurface& s) {
    return s.width && s.height && s.width<=16384 && s.height<=16384 &&
           s.pitch>=s.width*4 && s.pitch<=262144 && !(s.pitch&3) &&
           !(s.address&63) && s.address<0x100000000ULL &&
           (uint64_t)s.pitch*s.height<=0x100000000ULL-s.address;
}
static bool gpu_texture_draw_valid(const GpuTextureDraw& d) {
    return gpu_render_surface_valid(d.source) && gpu_render_surface_valid(d.destination) &&
        d.client.x1<d.client.x2 && d.client.y1<d.client.y2 &&
        (int64_t)d.client.x2-d.client.x1<=32767 && (int64_t)d.client.y2-d.client.y1<=32767 &&
        d.clip.x1>=0 && d.clip.y1>=0 && d.clip.x1<d.clip.x2 && d.clip.y1<d.clip.y2 &&
        d.clip.x1>=d.client.x1 && d.clip.y1>=d.client.y1 &&
        d.clip.x2<=d.client.x2 && d.clip.y2<=d.client.y2 &&
        d.clip.x2<=(int)d.destination.width && d.clip.y2<=(int)d.destination.height;
}
static uint32_t gpu_float_bits(float value) {
    uint32_t bits;__builtin_memcpy(&bits,&value,4);return bits;
}
static void gpu_render_surface_state(uint32_t* words,const GpuRenderSurface& s) {
    // Linear 2D BGRA8, HALIGN/VALIGN=4, MOCS[0] (explicit UC in the driver).
    words[0]=(1u<<29)|(0xC0u<<18)|(1u<<16)|(1u<<14)|(1u<<8);
    words[2]=(s.width-1)|((s.height-1)<<16);
    words[3]=s.pitch-1;words[5]=1u<<8;
    words[7]=(4u<<25)|(5u<<22)|(6u<<19)|(7u<<16);
    words[8]=(uint32_t)s.address;words[9]=(uint32_t)(s.address>>32);
}
static bool gpu_build_texture_batch(uint32_t* words,uint64_t batch_address,
                                    const GpuTextureDraw& draw) {
    if(!words || (batch_address&4095) || batch_address>0xFFFFF000ULL ||
       !gpu_texture_draw_valid(draw))return false;
    for(unsigned i=0;i<1024;++i)words[i]=0;
    for(unsigned i=0;i<sizeof(gpu_gen9_pipeline)/4;++i)words[i]=gpu_gen9_pipeline[i];
    for(const auto& state:gpu_gen9_state)words[state.index]=state.value;
    // STATE_BASE_ADDRESS surface/dynamic/instruction heaps and vertex buffer.
    words[18]=(uint32_t)batch_address|1;words[20]=(uint32_t)batch_address|1;
    words[24]=(uint32_t)batch_address|1;
    words[262]=(uint32_t)batch_address+GPU_RENDER_VERTEX_OFFSET;
    words[258]=(draw.destination.width-1)|((draw.destination.height-1)<<16);
    words[2048/4]=GPU_RENDER_DST_STATE;words[2048/4+1]=GPU_RENDER_SRC_STATE;
    gpu_render_surface_state(words+GPU_RENDER_DST_STATE/4,draw.destination);
    gpu_render_surface_state(words+GPU_RENDER_SRC_STATE/4,draw.source);
    // IGT blit.g7a Gen9 SIMD16 sampler and render-target-write shader.
    static constexpr uint32_t shader[]={
        0x0080005A,0x2F403AE8,0x3A0000C0,0x008D0040,
        0x0080005A,0x2F803AE8,0x3A0000D0,0x008D0040,
        0x02800031,0x2E203A48,0x0E8D0F40,0x08840001,
        0x05800031,0x20003A40,0x0E8D0E20,0x90031000};
    for(unsigned i=0;i<16;++i)words[2304/4+i]=shader[i];
    if(draw.blend){
        // Source-over with premultiplied alpha, ADD, ONE / INV_SRC_ALPHA.
        words[2432/4+1]=0x86603300u;
        words[222]=(1u<<30)|(1u<<29)|(1u<<14)|(0x13u<<9)|
                   (1u<<24)|(0x13u<<19);
    }
    const int x[]={draw.clip.x2,draw.clip.x1,draw.clip.x1};
    const int y[]={draw.clip.y2,draw.clip.y2,draw.clip.y1};
    for(unsigned i=0;i<3;++i){
        unsigned offset=GPU_RENDER_VERTEX_OFFSET/4+i*3;
        words[offset]=(uint32_t)x[i]|((uint32_t)y[i]<<16);
        words[offset+1]=gpu_float_bits((float)(x[i]-draw.client.x1)/(draw.client.x2-draw.client.x1));
        words[offset+2]=gpu_float_bits((float)(y[i]-draw.client.y1)/(draw.client.y2-draw.client.y1));
    }
    words[GPU_RENDER_COMMAND_BYTES/4]=0x05000000; // MI_BATCH_BUFFER_END
    return true;
}

// Gen9 logical RCS register-state page (the context's second page).
static void gpu_init_render_context(uint32_t* state,uint32_t ring,uint64_t pml4) {
    for(unsigned i=0;i<1024;++i)state[i]=0;
    state[1]=0x1100101B; // MI_LOAD_REGISTER_IMM(14), FORCE_POSTED
    const uint32_t regs[]={0x2244,0x2034,0x2030,0x2038,0x203C,0x2168,0x2140,
                           0x2110,0x211C,0x2114,0x2118,0x21C0,0x21C4,0x21C8};
    for(unsigned i=0;i<14;++i)state[2+i*2]=regs[i];
    state[3]=0x00090009; // inhibit synchronous switch and initial engine restore
    state[9]=ring;state[11]=(32768-4096)|1;
    state[17]=1u<<5; // RING_BB_PPGTT
    state[0x21]=0x11001011; // MI_LOAD_REGISTER_IMM(9), FORCE_POSTED
    state[0x22]=0x23A8;
    for(unsigned i=0;i<4;++i){
        state[0x24+i*4]=0x2274+(3-i)*8;
        state[0x26+i*4]=0x2270+(3-i)*8;
    }
    state[0x31]=(uint32_t)(pml4>>32);state[0x33]=(uint32_t)pml4;
    state[0x41]=0x11000001;state[0x42]=0x20C8;
    // RPCS=0 requests the hardware's default slice/subslice/EU availability.
}
#endif
