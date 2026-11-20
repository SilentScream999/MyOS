#ifndef WYZAR_INTEL_RENDER_H
#define WYZAR_INTEL_RENDER_H
#include "gpu_render_commands.h"
#include "gpu_render_reset.h"

// One kernel-owned logical RCS context. All allocations stay pinned until
// reboot, so neither a delayed context save nor a late GPU write can hit freed
// memory. Submission is serial and waits for both its fence and schedule-out.
static uint8_t* g_render_storage=nullptr;
static uint64_t g_render_pml4=0;
static unsigned g_render_tail=0;
static uint32_t g_render_sequence=0;
static bool g_render_ready=false,g_render_busy=false;
static GpuMappingCache g_render_source_cache;
static uint64_t g_render_map_updates=0,g_render_map_reuses=0;
static uint64_t g_render_draws=0;
static uint64_t g_render_ring_recycles=0;
static constexpr uint32_t RENDER_RING_OFFSET=96*1024;
static constexpr uint32_t RENDER_BATCH_OFFSET=128*1024;
static constexpr uint32_t RENDER_FENCE_OFFSET=132*1024;
static constexpr uint32_t RENDER_BATCH_VA=0x100000;
static constexpr uint32_t RENDER_MASTER_VA=0x2000000;
static constexpr uint32_t RENDER_SOURCE_VA=0x10000000;

static uint32_t render_read(uint32_t offset) {
    return *(volatile uint32_t*)(g_igpu.mmio_virt+offset);
}
static void render_write(uint32_t offset,uint32_t value) {
    *(volatile uint32_t*)(g_igpu.mmio_virt+offset)=value;
    (void)render_read(offset);
}
static bool render_reset() {
    struct ResetIO {
        uint32_t read(uint32_t reg){return render_read(reg);}
        void write(uint32_t reg,uint32_t value){render_write(reg,value);}
        uint64_t now(){return gpu_tsc();}
        void pause(){__asm__ volatile("pause");}
    } io;
    // Refuse to reset if the engine cannot acknowledge preparation. Existing
    // callers disable/quarantine RCS rather than forcing a potentially fatal
    // reset through outstanding context-save or batch traffic.
    return gpu_render_domain_reset(io,tsc_hz?tsc_hz/5:GPU_TIMEOUT_TSC);
}
static bool render_forcewake() {
    render_write(0xA278,0x00010001);
    uint64_t start=gpu_tsc();
    while(!(render_read(0xD84)&1)){
        if(gpu_tsc()-start>(tsc_hz?tsc_hz/20:GPU_TIMEOUT_TSC)){
            render_write(0xA278,0x00010000);
            print((char*)"GPU: render forcewake did not acknowledge");return false;
        }
        __asm__ volatile("pause");
    }
    return true;
}
static uint64_t render_new_table() {
    uint64_t physical=alloc_phys_page();
    if(!physical)return 0;
    auto* table=(uint64_t*)(HHDM+physical);
    for(unsigned i=0;i<512;++i)table[i]=0;
    gpu_clflush_nofence(table,4096);gpu_mfence();
    return physical;
}
static bool render_map(uint64_t address,const uint8_t* cpu,uint64_t bytes) {
    if((address&4095)||((uint64_t)cpu&4095)||!bytes)return false;
    uint64_t pages=(bytes+4095)/4096;
    for(uint64_t page=0;page<pages;++page){
        uint64_t physical;
        if(!try_virt_to_phys((uint64_t)cpu+page*4096,&physical))return false;
        uint64_t va=address+page*4096;
        auto* table=(uint64_t*)(HHDM+g_render_pml4);
        for(int shift=39;shift>=21;shift-=9){
            unsigned index=(va>>shift)&511;
            if(!(table[index]&1)){
                uint64_t next=render_new_table();if(!next)return false;
                table[index]=next|3;
                gpu_clflush_nofence(table+index,8);
            }
            table=(uint64_t*)(HHDM+(table[index]&0x0000FFFFFFFFF000ULL));
        }
        // Gen8 PPGTT leaf: present + writable, PAT index zero (UC).
        table[(va>>12)&511]=physical|3;
        gpu_clflush_nofence(table+((va>>12)&511),8);
    }
    gpu_mfence();return true;
}
static bool render_stop(const char* reason) {
    print((char*)reason);g_render_ready=false;g_gpu_render_hook=nullptr;
    g_render_source_cache.valid=false;
    gpu_log_hex("GPU RCS error:",render_read(0x2064));
    gpu_log_hex("GPU RCS instruction:",render_read(0x2068));
    gpu_log_hex("GPU RCS active head:",render_read(0x2074));
    gpu_log_hex("GPU RCS execlist status:",render_read(0x2234));
    gpu_log_hex("GPU RCS context status pointer:",render_read(0x23A0));
    if(!render_reset()){
        // A late render-target write requires the same destination quarantine
        // as a failed BCS reset, even when BCS itself can be stopped.
        g_gpu_quarantine_sources=true;
        if(g_gpu_recovery_buffer){
            g_master_backbuffer=g_gpu_recovery_buffer;g_gpu_recovery_buffer=nullptr;
        }
        gpu_scene_abort("GPU: RCS reset failed; quarantining GPU destination");
    }else{
        render_write(0xA278,0x00010000);
        gpu_status_set(g_gpu_present_ready?"GPU:opaque+rainbow flip UC":"GPU:opaque+rainbow LLC");
    }
    g_render_busy=false;return false;
}
static bool render_program_context() {
    // Caller has confirmed RCS reset. Reinitialize context/HWS and ring only;
    // batch, fence, page tables and pixel buffers have separate ownership.
    for(unsigned i=0;i<RENDER_BATCH_OFFSET;++i)g_render_storage[i]=0;
    g_render_tail=0;
    gpu_init_render_context((uint32_t*)(g_render_storage+4096),
        (uint32_t)g_gpu_render_gfx+RENDER_RING_OFFSET,g_render_pml4);
    gpu_clflush_nofence(g_render_storage,RENDER_BATCH_OFFSET);gpu_mfence();
    render_write(0xC800,9);
    render_write(0xB020,(render_read(0xB020)&0xFFFF0000u)|0x10);
    if(render_read(0xC800)!=9 || (render_read(0xB020)&0xFFFF)!=0x10)return false;
    render_write(0x2080,(uint32_t)g_gpu_render_gfx);
    render_write(0x2098,UINT32_MAX);
    render_write(0x229C,0x80208000);
    render_write(0x209C,0x01000000);
    return (render_read(0x229C)&0x8000)!=0;
}
static bool render_submit(bool texture) {
    if(g_render_busy || !g_render_storage)return false;
    if(gpu_render_needs_recycle(g_render_tail)){
        // Previous fence AND schedule-out have completed. Avoid the unverified
        // hardware wrap path by resetting the idle RCS domain before reuse.
        print((char*)"GPU: recycling idle RCS context before ring wrap");
        gpu_log_hex("GPU RCS last completed sequence:",g_render_sequence);
        if(!render_reset() || !render_program_context())
            return render_stop("GPU: idle RCS ring recycle failed");
        ++g_render_ring_recycles;
        print((char*)"GPU: idle RCS ring recycle completed");
    }
    g_render_busy=true;
    auto* ring=(uint32_t*)(g_render_storage+RENDER_RING_OFFSET);
    auto* state=(uint32_t*)(g_render_storage+4096);
    auto* fence=(volatile uint32_t*)(g_render_storage+RENDER_FENCE_OFFSET);
    uint32_t sequence=++g_render_sequence;
    unsigned start=g_render_tail;
    if(!gpu_render_packet(ring,start,(uint32_t)g_gpu_render_gfx+RENDER_FENCE_OFFSET,
                          sequence,texture,&g_render_tail))
        return render_stop("GPU: invalid RCS ring packet");
    gpu_clflush_nofence(ring+start/4,256);
    gpu_clflush_nofence(state,4096);gpu_mfence();
    // The previous context has scheduled out. Only its tail changes on reuse.
    state[7]=g_render_tail;
    gpu_clflush_nofence(state+7,4);gpu_mfence();
    uint32_t csb=render_read(0x23A0)&7;
    if(csb>=6)csb=5;
    render_write(0x23A0,(0x700u<<16)|(csb<<8));
    // Serial submissions have already scheduled out. Force a full context
    // restore instead of relying on idle/lite restore across ring wraps.
    uint64_t descriptor=g_gpu_render_gfx|GPU_RENDER_CONTEXT_FLAGS;
    {
        TtyIrqGuard guard;
        // ELSP is a write-only four-dword submission port. Do not post reads
        // from it between descriptor words; post via EXECLIST_STATUS instead.
        auto* elsp=(volatile uint32_t*)(g_igpu.mmio_virt+0x2230);
        *elsp=0;*elsp=0;*elsp=1;*elsp=(uint32_t)descriptor;
        (void)render_read(0x2234);
    }
    uint64_t started=gpu_tsc(),timeout=tsc_hz?tsc_hz/5:GPU_TIMEOUT_TSC;
    bool retired=false;
    uint64_t flags;__asm__ volatile("pushfq; pop %0":"=r"(flags));
    while(gpu_tsc()-started<timeout){
        uint32_t write=render_read(0x23A0)&7;
        if(write<6){
            while(csb!=write){
                csb=(csb+1)%6;
                uint32_t status=render_read(0x2370+csb*8);
                uint32_t id=render_read(0x2374+csb*8);
                if((status&(1u<<3)) && id==1)retired=true;
            }
            render_write(0x23A0,(0x700u<<16)|(csb<<8));
        }
        gpu_clflush_nofence(fence,8);gpu_mfence();
        if(*fence==sequence && retired){g_render_busy=false;return true;}
        if((flags&(1u<<9))&&g_current_task)task_sleep_ms(1);
        else __asm__ volatile("pause");
    }
    return render_stop("GPU: RCS fence/context retirement timed out");
}
static bool render_texture(const GpuTextureDraw& draw) {
    auto* batch=(uint32_t*)(g_render_storage+RENDER_BATCH_OFFSET);
    if(!gpu_build_texture_batch(batch,RENDER_BATCH_VA,draw))return false;
    gpu_clflush_nofence(batch,4096);gpu_mfence();
    if(!render_submit(true))return false;
    ++g_render_draws;return true;
}
static bool gpu_render_scene(const GpuScenePlan& plan) {
    if(!g_render_ready || !g_gpu_ready || plan.client_count>GpuScenePlan::max_clients ||
       plan.count>GpuScenePlan::capacity)return false;
    for(unsigned i=0;i<plan.count;++i){
        const auto& copy=plan.copies[i];
        if(copy.source>=plan.client_count)return render_stop("GPU: invalid RCS scene source");
        const auto& c=plan.clients[copy.source];
        uint64_t bytes=(uint64_t)c.pitch*c.height,offset=(uint64_t)c.pixels&4095;
        if(bytes+offset>GPU_SOURCE_BYTES)return render_stop("GPU: RCS source exceeds mapping arena");
        GpuMappingKey key{(uint64_t)c.pixels-offset,(bytes+offset+4095)&~4095ULL};
        if(!g_render_source_cache.matches(&key,1,g_gpu_surface_epoch)){
            if(!render_map(RENDER_SOURCE_VA,(const uint8_t*)key.address,key.bytes))
                return render_stop("GPU: RCS source mapping failed");
            g_render_source_cache.record(&key,1,g_gpu_surface_epoch);++g_render_map_updates;
        }else ++g_render_map_reuses;
        // Explicit ownership handoff for RCS's UC surfaces. The BCS coherent
        // sharing probe alone does not establish RCS/CPU cache coherence.
        // Explicit pixel handoffs during sustained-stability investigation.
        // Keep rectangle-sized flushing and the retained partial scene path.
        gpu_clflush_nofence(c.pixels,bytes);
        for(int y=copy.destination.y1;y<copy.destination.y2;++y)
            gpu_clflush_nofence(g_master_backbuffer+(uint64_t)y*fb->pitch+copy.destination.x1*4,
                               (copy.destination.x2-copy.destination.x1)*4);
        gpu_mfence();
        GpuTextureDraw draw{{RENDER_SOURCE_VA+offset,c.width,c.height,c.pitch},
            {RENDER_MASTER_VA,(uint32_t)fb->width,(uint32_t)fb->height,(uint32_t)fb->pitch},
            {c.client.x1,c.client.y1,c.client.x2,c.client.y2},
            {copy.destination.x1,copy.destination.y1,copy.destination.x2,copy.destination.y2},false};
        if(!render_texture(draw))return g_render_ready?render_stop("GPU: RCS texture draw failed"):false;
        for(int y=copy.destination.y1;y<copy.destination.y2;++y)
            gpu_clflush_nofence(g_master_backbuffer+(uint64_t)y*fb->pitch+copy.destination.x1*4,
                               (copy.destination.x2-copy.destination.x1)*4);
        gpu_mfence();
    }
    return true;
}
static bool gpu_render_initialize() {
    if(!GPU_RENDER || !g_gpu_render_gfx ||
       (uint64_t)fb->pitch*fb->height>RENDER_SOURCE_VA-RENDER_MASTER_VA)return false;
    print((char*)"GPU: initializing experimental Gen9 logical render context");
    g_render_storage=gpu_alloc_aligned(GPU_RENDER_OWNED_BYTES);
    g_render_pml4=render_new_table();
    if(!g_render_storage||!g_render_pml4){
        print((char*)"GPU: render context allocation failed");return false;
    }
    if(!render_forcewake())return false;
    if(!render_reset())return render_stop("GPU: RCS boot reset failed");
    if(!gtt_map_range(g_gpu_render_gfx,g_render_storage,GPU_RENDER_OWNED_BYTES))
        return render_stop("GPU: RCS context GGTT mapping failed");
    gtt_flush();
    if(!render_map(RENDER_BATCH_VA,g_render_storage+RENDER_BATCH_OFFSET,4096) ||
       !render_map(RENDER_MASTER_VA,g_master_backbuffer,(uint64_t)fb->pitch*fb->height))
        return render_stop("GPU: RCS page table setup failed");
    gpu_clflush_nofence(g_render_storage,GPU_RENDER_OWNED_BYTES);gpu_mfence();
    // Explicit UC cache policy for this private render context. Preserve the
    // adjacent L3 MOCS entry and leave the blitter/display cache policy alone.
    if(!render_program_context())return render_stop("GPU: RCS context programming failed");
    if(!render_submit(false))return false;
    print((char*)"GPU: RCS logical-context fence and retirement PASSED");
    // Pixel verification targets dedicated RAM, never scanout or the desktop.
    auto* source=(uint32_t*)gpu_alloc_aligned(4096);
    auto* destination=(uint32_t*)gpu_alloc_aligned(4096);
    if(!source||!destination||!render_map(RENDER_SOURCE_VA,(uint8_t*)source,4096)||
       !render_map(RENDER_MASTER_VA,(uint8_t*)destination,4096))
        return render_stop("GPU: RCS readback allocation failed");
    for(unsigned test=0;test<3;++test){
        for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)
            source[y*16+x]=test==2?0x80402010u:0xFF000000u|(x*13u<<16)|(y*11u<<8)|((x+y)*7u);
        for(unsigned i=0;i<32*32;++i)destination[i]=0xFF204060;
        gpu_clflush_nofence(source,4096);gpu_clflush_nofence(destination,4096);gpu_mfence();
        // 1:1 copy and exact 2x enlargement sample texel centers or quarter
        // positions. Fractional 16-to-12 scaling put samples exactly on texel
        // boundaries, where interpolation roundoff can legitimately pick either
        // neighbor. Keep exact comparisons, using unambiguous sample positions.
        GpuTextureDraw draw=gpu_render_probe_draw(test,RENDER_SOURCE_VA,RENDER_MASTER_VA);
        if(!render_texture(draw))return false;
        gpu_clflush_nofence(destination,4096);gpu_mfence();
        for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x){
            uint32_t expected=0xFF204060;
            if(x>=3&&y>=3&&x<(unsigned)draw.clip.x2&&y<(unsigned)draw.clip.y2){
                unsigned sx=(2*(x-2)+1)*draw.source.width/32;
                unsigned sy=(2*(y-2)+1)*draw.source.height/32;
                expected=test==2?0xFF504040u:source[sy*16+sx];
            }
            uint32_t actual=((volatile uint32_t*)destination)[y*32+x];
            if(actual!=expected){
                gpu_log_hex("GPU RCS readback test:",test);
                gpu_log_hex("GPU RCS readback x:",x);
                gpu_log_hex("GPU RCS readback y:",y);
                gpu_log_hex("GPU RCS expected pixel:",expected);
                gpu_log_hex("GPU RCS actual pixel:",actual);
                return render_stop("GPU: RCS texture/scaling/alpha readback FAILED");
            }
        }
    }
    // Gen9's context hang workaround forces non-coherent HDC operation. Keep
    // explicit pixel ownership transfers; a no-flush probe would test outside
    // that contract rather than establish a supported coherent rendering mode.
    print((char*)"GPU: Gen9 RCS hang workarounds enabled; explicit pixel handoffs");
    if(!render_map(RENDER_MASTER_VA,g_master_backbuffer,(uint64_t)fb->pitch*fb->height))
        return render_stop("GPU: RCS desktop mapping failed");
    g_render_ready=true;
    print((char*)"GPU: RCS textured copy, scaling and premultiplied alpha readback PASSED");
    gpu_status_set(g_gpu_present_ready?"GPU:rainbow+RCS flip UC":"GPU:rainbow+RCS LLC");
    return true;
}
#endif
