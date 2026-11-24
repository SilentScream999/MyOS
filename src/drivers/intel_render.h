#ifndef WYZAR_INTEL_RENDER_H
#define WYZAR_INTEL_RENDER_H
#include "gpu_render_commands.h"
#include "gpu_render_reset.h"
#include "gpu_gen9_workarounds.h"
#include "gpu_upload_cache.h"
#include "gpu_alpha_coverage.h"

// One kernel-owned logical RCS context. All allocations stay pinned until
// reboot, so neither a delayed context save nor a late GPU write can hit freed
// memory. Submission is serial and waits for both its fence and schedule-out.
static uint8_t* g_render_storage=nullptr;
static uint64_t g_render_pml4=0;
static unsigned g_render_tail=0;
static uint32_t g_render_sequence=0;
static uint32_t g_render_power_request=0;
static bool g_render_ready=false,g_render_busy=false;
static bool g_render_cached_sources=false;
static bool g_render_source_cache_rejected=false;
static unsigned g_render_ps_threads=16;
static uint64_t g_render_timestamp_hz=0;
static bool g_render_timestamp_ready=false;
static GpuExecutionTiming g_render_timing_probe{};
struct RenderUpload {
    uint8_t* pixels=nullptr;
    uint64_t capacity=0,last_used=0;
    GpuUploadCache cache{};
    GpuAlphaCoverage coverage{};
    unsigned width=0,height=0,pitch=0;
};
static RenderUpload g_render_uploads[4]{};
static unsigned g_render_upload_slot=0;
static uint64_t g_render_upload_clock=0;
static uint32_t render_source_address(unsigned slot){return 0x40000000u+slot*0x04000000u;}
static uint64_t g_render_map_updates=0,g_render_map_reuses=0;
static uint64_t g_render_draws=0;
static GpuTextureDraw g_render_current_draw{};
static bool g_render_isolated_probe=false;
static bool g_render_tlb_dirty=true;
static constexpr uint32_t RENDER_RING_OFFSET=96*1024;
static constexpr uint32_t RENDER_BATCH_OFFSET=128*1024;
static constexpr uint32_t RENDER_FENCE_OFFSET=132*1024;
static constexpr uint32_t RENDER_SCRATCH_OFFSET=136*1024;
static_assert(RENDER_SCRATCH_OFFSET+128*128*4<=GPU_RENDER_OWNED_BYTES);
static constexpr uint32_t RENDER_BATCH_VA=0x100000;
static constexpr uint32_t RENDER_MASTER_VA=0x2000000;
static constexpr uint32_t RENDER_SOURCE_VA=0x10000000;
static constexpr uint32_t RENDER_SCANOUT_VA[2]={0x20000000,0x24000000};

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
static bool render_disable_firmware_power_gating() {
    // GT/render wake are already held. Take media too, as in i915's
    // __intel_rc6_disable(), before disabling inherited Gen9 power gating.
    render_write(0xA270,0x00010001);
    uint64_t started=gpu_tsc(),timeout=tsc_hz/20;
    while(!(render_read(0xD88)&1)){
        if(gpu_tsc()-started>=timeout){
            render_write(0xA270,0x00010000);
            return false;
        }
        __asm__ volatile("pause");
    }
    gpu_log_hex("GPU RCS inherited RC control:",render_read(0xA090));
    gpu_log_hex("GPU RCS inherited power gating:",render_read(0xA210));
    render_write(0xA210,0);
    render_write(0xA090,0);
    render_write(0xA094,0);
    uint32_t rc=render_read(0xA090),pg=render_read(0xA210);
    gpu_log_hex("GPU RCS disabled RC control:",rc);
    gpu_log_hex("GPU RCS disabled power gating:",pg);
    render_write(0xA270,0x00010000);
    return rc==0&&pg==0;
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
    g_render_tlb_dirty=true;
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
        // Gen8 PPGTT leaf: present + writable, PAT index zero. Surface
        // accesses use the explicit surface MOCS policies programmed below.
        table[(va>>12)&511]=physical|3;
        gpu_clflush_nofence(table+((va>>12)&511),8);
    }
    gpu_mfence();return true;
}
static bool render_stop(const char* reason,const char* code="RCSFAIL") {
    gpu_direct_end();
    if(g_render_isolated_probe){
        // This probe writes only dedicated pinned RAM before the first display
        // flip. Leave the stalled context pinned and boot the CPU desktop.
        g_render_ready=false;g_render_busy=false;
        g_gpu_ready=false;g_gpu_compose_ready=false;g_gpu_present_ready=false;
        g_gpu_render_hook=nullptr;g_gpu_compose_hook=nullptr;
        g_gpu_present_hook=nullptr;g_gpu_flip_hook=nullptr;
        gpu_status_set("GPU:offscreen probe -> CPU");
        print((char*)reason);print((char*)"GPU: isolated offscreen probe FAILED; CPU desktop selected");
        return false;
    }
    if(g_render_ready && g_gpu_recovery_buffer){
        // Desktop RCS runs after all BCS submissions retire and before a new
        // flip is armed. The target is either the old master or a hidden
        // scanout. Leave that target, its sources and the context pinned.
        // CPU presentation can keep using the currently latched framebuffer.
#if GPU_TRACE
        g_gpu_trace_failure=code;
#endif
        gpu_trace_stage(code);
        g_render_ready=false;g_render_busy=false;
        g_gpu_ready=false;g_gpu_compose_ready=false;g_gpu_present_ready=false;
        g_gpu_render_hook=nullptr;g_gpu_compose_hook=nullptr;
        g_gpu_present_hook=nullptr;g_gpu_flip_hook=nullptr;
        g_gpu_quarantine_sources=true;
        g_master_backbuffer=g_gpu_recovery_buffer;g_gpu_recovery_buffer=nullptr;
        gpu_status_set("GPU:CPU fallback ");gpu_status_cat(code);
        // The caller retries the whole scene using CPU rendering. Avoid
        // engine MMIO, reset and console rendering on this error stack.
        return false;
    }
    gpu_trace_stage("RECOVERY");
    print((char*)reason);g_render_ready=false;g_gpu_render_hook=nullptr;
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
    if((g_igpu.devid>>8)==0x59 || (g_igpu.devid>>8)==0x19){
        struct WorkaroundIO {
            uint32_t read(uint32_t reg){return render_read(reg);}
            void write(uint32_t reg,uint32_t value){render_write(reg,value);}
        } io;
        const uint32_t registers[]={0xFDC,0xB004,0xB008,0xB118,0xB11C,0x20D4};
        for(uint32_t reg:registers){
            gpu_log_hex("GPU Gen9 workaround register:",reg);
            gpu_log_hex("GPU Gen9 workaround inherited:",render_read(reg));
        }
        if(!gpu_apply_gen9_engine_workarounds(io,render_read(0x9120)))return false;
        for(uint32_t reg:registers){
            gpu_log_hex("GPU Gen9 workaround register:",reg);
            gpu_log_hex("GPU Gen9 workaround verified:",render_read(reg));
        }
    }
    // Caller has confirmed RCS reset. Reinitialize context/HWS and ring only;
    // batch, fence, page tables and pixel buffers have separate ownership.
    for(unsigned i=0;i<RENDER_BATCH_OFFSET;++i)g_render_storage[i]=0;
    g_render_tail=0;
    g_render_tlb_dirty=true;
    gpu_init_render_context((uint32_t*)(g_render_storage+4096),
        (uint32_t)g_gpu_render_gfx+RENDER_RING_OFFSET,g_render_pml4,g_render_power_request);
    gpu_clflush_nofence(g_render_storage,RENDER_BATCH_OFFSET);gpu_mfence();
    // WaDisableSbeCacheDispatchPortSharing:kbl. The first restore inhibits
    // engine-state restore; subsequent saves retain this context register.
    // i915 kbl_ctx_workarounds_init(), GEN8_HALF_SLICE_CHICKEN1 bit 4.
    if((g_igpu.devid>>8)==0x59){
        // i915 gen9_ctx_workarounds_init: command-boundary preemption.
        // This private context does not provide mid-thread preemption state.
        gpu_log_hex("GPU RCS preemption mode before:",render_read(0x2580));
        render_write(0x2580,0x00070004);
        uint32_t preempt=render_read(0x2580);
        gpu_log_hex("GPU RCS preemption mode after:",preempt);
        if((preempt&7)!=4)return false;
        render_write(0xE100,0x00100010);
        if(!(render_read(0xE100)&(1u<<4)))return false;
        // i915 gen9_ctx_workarounds_init():
        // WaDisableSamplerPowerBypassForSOPingPong and
        // WaDisableSTUnitPowerOptimization (both apply to Kaby Lake).
        render_write(0xE184,0x00020002);
        render_write(0xE180,0x20002000);
        if(!(render_read(0xE184)&2)||!(render_read(0xE180)&(1u<<13)))return false;
        // WaClearFlowControlGpgpuContextSave and
        // WaDisablePartialInstShootdown: gen9_ctx_workarounds_init.
        render_write(0xE4F0,0x81008100);
        if((render_read(0xE4F0)&0x8100)!=0x8100)return false;
        // Disable SBE replay-buffer optimization. i915 applies this on
        // KBL C0+ as WaToEnableHwFixForPushConstHWBug. Disabling the
        // optimization also gives the private context a conservative baseline.
        render_write(0x7014,0x01000100);
        if(!(render_read(0x7014)&(1u<<8)))return false;
    }
    render_write(0xC800,9);
    render_write(0xB020,(render_read(0xB020)&0xFFFF0000u)|0x10);
    if(render_read(0xC800)!=9 || (render_read(0xB020)&0xFFFF)!=0x10)return false;
    // Read-only texture cache policy, separate from the UC render destination.
    // Match skl_mocs_table's LLC/eLLC WB + LRU3 and L3 WB, without skip caching.
    // https://codebrowser.dev/linux/linux/drivers/gpu/drm/i915/gt/intel_mocs.c.html
    // MOCS index one occupies the upper half of the packed L3 register.
    if(GPU_RCS_ALPHA&&!g_render_source_cache_rejected){
        render_write(0xC804,0x3B);
        render_write(0xB020,(render_read(0xB020)&0xFFFFu)|0x00300000u);
        g_render_cached_sources=render_read(0xC804)==0x3B&&(render_read(0xB020)>>16)==0x30;
    }else g_render_cached_sources=false;
    // Gen9 hardware uses MOCS[63] for L3 evictions, independently of the
    // surface's MOCS index. Match i915 skl_mocs_table[63]: LLC WB, LRU=3,
    // L3 UC. Preserve entry 62 in the shared packed L3 register.
    // Firmware-provided eviction policy must not define our render handoff.
    gpu_log_hex("GPU RCS inherited eviction MOCS63:",render_read(0xC8FC));
    gpu_log_hex("GPU RCS inherited L3 MOCS62/63:",render_read(0xB09C));
    render_write(0xC8FC,0x37);
    render_write(0xB09C,(render_read(0xB09C)&0xFFFFu)|0x00100000u);
    if(render_read(0xC8FC)!=0x37 || (render_read(0xB09C)>>16)!=0x10)return false;
    gpu_log_hex("GPU RCS programmed eviction MOCS63:",render_read(0xC8FC));
    gpu_log_hex("GPU RCS programmed L3 MOCS62/63:",render_read(0xB09C));
    render_write(0x2080,(uint32_t)g_gpu_render_gfx);
    render_write(0x2098,UINT32_MAX);
    render_write(0x229C,0x80208000);
    render_write(0x209C,0x01000000);
    return (render_read(0x229C)&0x8000)!=0;
}
static bool render_submit(bool texture,bool timing_probe=false) {
    gpu_trace_stage("RING");
    if(g_render_busy || !g_render_storage)return false;
    g_render_busy=true;
    auto* ring=(uint32_t*)(g_render_storage+RENDER_RING_OFFSET);
    auto* state=(uint32_t*)(g_render_storage+4096);
    // The previous submit waited for both fence and schedule-out. Confirm
    // that its saved HEAD consumed the packet before reusing ring storage.
    gpu_clflush_nofence(state,64);gpu_mfence();
    uint32_t head=((volatile uint32_t*)state)[5]&GPU_HEAD_MASK;
    if(head!=g_render_tail){
        if(!g_render_ready){
            gpu_log_hex("GPU RCS saved head:",head);
            gpu_log_hex("GPU RCS expected head:",g_render_tail);
        }
        return render_stop("GPU: RCS ring did not retire to the submitted tail","HEAD");
    }
    auto* fence=(volatile uint32_t*)(g_render_storage+RENDER_FENCE_OFFSET);
    bool timed=g_render_timestamp_hz&&(timing_probe||
        (g_gpu_timeline.begin&&g_gpu_timeline.measure&&g_render_timestamp_ready));
    if(timed){
        uint32_t sentinel=render_read(0x2358)-(uint32_t)g_render_timestamp_hz;
        for(unsigned i=16;i<32;++i)fence[i]=sentinel;
        gpu_clflush_nofence(fence+16,64);gpu_mfence();
    }
    uint32_t sequence=++g_render_sequence;
    unsigned start=g_render_tail;
    if(!gpu_render_packet(ring,start,(uint32_t)g_gpu_render_gfx+RENDER_FENCE_OFFSET,
                          sequence,texture,&g_render_tail,g_render_tlb_dirty,RENDER_BATCH_VA,
                          timed?(uint32_t)g_gpu_render_gfx+RENDER_FENCE_OFFSET+64:0))
        return render_stop("GPU: invalid RCS ring packet","PACKET");
    gpu_clflush_nofence(ring+start/4,GPU_RENDER_PACKET_BYTES);
    if(start==GPU_RENDER_RING_BYTES-GPU_RENDER_PACKET_BYTES)
        gpu_clflush_nofence(ring,GPU_RENDER_PACKET_BYTES);
    gpu_mfence();
    // The previous context has scheduled out. Only its tail changes on reuse.
    state[7]=g_render_tail;
    gpu_clflush_nofence(state+7,4);gpu_mfence();
    uint32_t csb=render_read(0x23A0)&7;
    if(csb>=6)csb=5;
    render_write(0x23A0,(0x700u<<16)|(csb<<8));
    // Use normal execlist restore semantics for this serial context, matching
    // i915's RCS descriptor. Force-restore on every reuse bypasses the normal
    // idle/lite restore path; the updated context image supplies the new tail.
    uint64_t descriptor=g_gpu_render_gfx|GPU_RENDER_CONTEXT_FLAGS;
    bool trace_alpha=g_rain_trace.update && texture && g_render_current_draw.blend;
    gpu_trace_stage("SUBMIT");
    gpu_timeline_submit(gpu_tsc());
    uint32_t submitted=0;uint64_t timing_cpu_first=0;
    {
        TtyIrqGuard guard;
        // ELSP is a write-only four-dword submission port. Do not post reads
        // from it between descriptor words; post via EXECLIST_STATUS instead.
        auto* elsp=(volatile uint32_t*)(g_igpu.mmio_virt+0x2230);
        if(trace_alpha){
            if(!g_rain_trace.alpha_draws)g_rain_trace.alpha_submit=gpu_tsc();
            ++g_rain_trace.alpha_draws;
        }
        if(timed){timing_cpu_first=gpu_tsc();submitted=render_read(0x2358);}
        *elsp=0;*elsp=0;*elsp=1;*elsp=(uint32_t)descriptor;
        (void)render_read(0x2234);
    }
    uint64_t started=gpu_tsc(),timeout=tsc_hz?tsc_hz/5:GPU_TIMEOUT_TSC;
    uint64_t spin=tsc_hz?tsc_hz/20000:150000;
    uint64_t wait_flags;
    __asm__ volatile("pushfq; pop %0":"=r"(wait_flags));
    gpu_trace_stage("WAIT");
    bool retired=false,fenced=false;
    for(;;){
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
        // Poll context status in MMIO while the GPU owns the stamp line.
        // On timeout take a final sample even if no retirement was reported.
        bool expired=gpu_tsc()-started>=timeout;
        if(retired || expired){
            gpu_clflush_nofence(fence,40);gpu_mfence();
            fenced=(*fence==sequence);
        }
        if(fenced && retired){
            gpu_timeline_ready(gpu_tsc());
            if(fence[2]!=sequence||fence[4]!=sequence||fence[5]!=sequence||fence[6]!=sequence||fence[8]!=sequence)
                return render_stop("GPU: RCS command checkpoint mismatch","BADSTAMP");
            if(trace_alpha)g_rain_trace.alpha_done=gpu_tsc();
            if(timed){
                uint32_t observed=render_read(0x2358);uint64_t cpu_last=gpu_tsc();
                gpu_clflush_nofence(fence+16,64);gpu_mfence();
                uint32_t stamps[5];for(unsigned i=0;i<5;++i)stamps[i]=fence[16+i];
                auto& result=timing_probe?g_render_timing_probe:g_gpu_timeline.rcs_gpu;
                gpu_execution_record(result,g_render_timestamp_hz,submitted,stamps,observed,
                    cpu_last-timing_cpu_first,tsc_hz);
            }
            g_render_tlb_dirty=false;g_render_busy=false;return true;
        }
        // Always sample completion after waking, even if scheduling delayed
        // this task past its deadline.
        if(expired)break;
        // Short draws retire without a scheduler round trip. Longer work
        // yields the compositor CPU while preserving both retirement checks.
        if(g_render_ready && !g_render_isolated_probe &&
           gpu_tsc()-started>spin && (wait_flags&(1u<<9)) && g_current_task)
            gpu_perf_sleep_one(g_gpu_perf.rcs_sleep,tsc_hz,[](uint64_t ms){task_sleep_ms(ms);});
        else __asm__ volatile("pause");
    }
    const char* missing=fenced?"NOEXIT":retired?"NOFENCE":
                        fence[5]==sequence?"DRAINED":fence[8]==sequence?"RTCLEAN":fence[6]==sequence?"SCORE":
                        fence[4]==sequence?"PARSED":
                        fence[2]==sequence?"PREDRAW":"NOSTAMP";
    bool result=render_stop("GPU: RCS fence/context retirement timed out",missing);
    // Desktop error handling has switched to CPU-owned pixels. Capture the
    // fault address before another boot destroys evidence of this submission.
    print((char*)"GPU RCS timeout checkpoint:");print((char*)missing);
    gpu_log_hex("GPU RCS sequence:",sequence);
    gpu_log_hex("GPU RCS submission requested TLB invalidate:",g_render_tlb_dirty);
    gpu_log_hex("GPU RCS parsed/scoreboard/drained stamps:",
                ((uint64_t)(fence[4]==sequence)<<2)|
                ((uint64_t)(fence[6]==sequence)<<1)|(fence[5]==sequence));
    gpu_log_hex("GPU RCS fault:",render_read(0x4094));
    gpu_log_hex("GPU RCS fault address low:",render_read(0x4B10));
    gpu_log_hex("GPU RCS fault address high:",render_read(0x4B14));
    gpu_log_hex("GPU RCS instruction error:",render_read(0x2064));
    gpu_log_hex("GPU RCS active head:",render_read(0x2074));
    gpu_log_hex("GPU RCS instruction done:",render_read(0x206C));
    gpu_log_hex("GPU RCS slice common done:",render_read(0x7100));
    gpu_log_hex("GPU RCS sampler done:",render_read(0xE160));
    gpu_log_hex("GPU RCS row done:",render_read(0xE164));
    gpu_log_hex("GPU RCS render forcewake ack:",render_read(0xD84));
    gpu_log_hex("GPU RCS GT forcewake ack:",render_read(0x130044));
    gpu_log_hex("GPU RCS RC control:",render_read(0xA090));
    gpu_log_hex("GPU RCS L3 allocation:",render_read(0x7034));
    gpu_log_hex("GPU RCS active power request:",render_read(0x20C8));
    gpu_log_hex("GPU RCS active MOCS0:",render_read(0xC800));
    gpu_log_hex("GPU RCS active L3 MOCS0/1:",render_read(0xB020));
    gpu_log_hex("GPU RCS active eviction MOCS63:",render_read(0xC8FC));
    gpu_log_hex("GPU RCS active L3 MOCS62/63:",render_read(0xB09C));
    gpu_log_hex("GPU RCS active MCR steering:",render_read(0xFDC));
    gpu_log_hex("GPU RCS active L3 flush/atomics:",render_read(0xB118));
    gpu_log_hex("GPU RCS active L3 retry/eviction:",render_read(0xB11C));
    gpu_log_hex("GPU RCS active LNCF atomics:",render_read(0xB008));
    gpu_log_hex("GPU RCS active credit workaround:",render_read(0xB004));
    gpu_log_hex("GPU RCS active context/TLB workaround:",render_read(0x20D4));
    // Sample the active settings on timeout before a reboot discards them.
    // The initialization readbacks precede thousands of context switches.
    gpu_log_hex("GPU RCS HDC_CHICKEN0 (expect bits 8030):",render_read(0x7300));
    gpu_log_hex("GPU RCS CACHE_MODE_1 (expect bits 0042):",render_read(0x7004));
    gpu_log_hex("GPU RCS GAM_ECOCHK (expect bits 02000100):",render_read(0x4090));
    gpu_log_hex("GPU RCS SBE port sharing disable (expect bit 0010):",render_read(0xE100));
    gpu_log_hex("GPU RCS ROW_CHICKEN (expect bits 8100):",render_read(0xE4F0));
    gpu_log_hex("GPU RCS submitted ring tail:",g_render_tail);
    gpu_log_hex("GPU RCS hardware ring head:",render_read(0x2034));
    gpu_log_hex("GPU RCS hardware ring tail:",render_read(0x2030));
#if GPU_RENDER && GPU_TRACE
    trace_nvidia_power("GPU PCI power snapshot: RCS timeout");
#endif
    const auto& draw=g_render_current_draw;
    gpu_log_hex("GPU RCS source width/height:",((uint64_t)draw.source.width<<32)|draw.source.height);
    gpu_log_hex("GPU RCS source pitch:",draw.source.pitch);
    gpu_log_hex("GPU RCS client left/top:",((uint64_t)(uint32_t)draw.client.x1<<32)|(uint32_t)draw.client.y1);
    gpu_log_hex("GPU RCS client right/bottom:",((uint64_t)(uint32_t)draw.client.x2<<32)|(uint32_t)draw.client.y2);
    gpu_log_hex("GPU RCS clip left/top:",((uint64_t)(uint32_t)draw.clip.x1<<32)|(uint32_t)draw.clip.y1);
    gpu_log_hex("GPU RCS clip right/bottom:",((uint64_t)(uint32_t)draw.clip.x2<<32)|(uint32_t)draw.clip.y2);
    return result;
}
static bool render_texture(const GpuTextureDraw& draw,bool solid_probe=false,
                           const GpuRenderRect* rects=nullptr,unsigned count=1) {
    GpuPerfTimer timer(g_gpu_perf.rcs_wait);++g_gpu_perf.rcs_draws;
    auto submitted=draw;
    submitted.cache_source=g_render_cached_sources;
    submitted.threads=g_render_ps_threads;
    g_render_current_draw=submitted;
    auto* batch=(uint32_t*)(g_render_storage+RENDER_BATCH_OFFSET);
    // The page stays pinned at the address installed during initialization.
    // Its contents can change after retirement without changing its PTE.
    if(!gpu_build_texture_batch(batch,RENDER_BATCH_VA,submitted,solid_probe,rects,count))return false;
    gpu_clflush_nofence(batch,4096);gpu_mfence();
    if(!render_submit(true))return false;
#if GPU_WORKLOAD
    if(g_gpu_timeline.begin){
        gpu_workload_render(g_gpu_timeline.workload,rects?rects:&draw.clip,count,draw.blend);
    }
#endif
    ++g_render_draws;return true;
}
static bool render_staged_texture(const GpuTextureDraw& draw,uint64_t destination_gfx,bool solid=false) {
    if(draw.clip.x2-draw.clip.x1>128||draw.clip.y2-draw.clip.y1>128)
        return render_stop("GPU: staged draw exceeds scratch bounds","TILESIZE");
    GpuTextureDraw tile=draw;
    tile.destination={RENDER_MASTER_VA,128,128,512};
    tile.client={draw.client.x1-draw.clip.x1,draw.client.y1-draw.clip.y1,
                 draw.client.x2-draw.clip.x1,draw.client.y2-draw.clip.y1};
    tile.clip={0,0,128,128};tile.padded=true;
    if(!render_texture(tile,solid))return false;
    if(!gpu_emit_copy(&g_gpu,g_gpu_render_gfx+RENDER_SCRATCH_OFFSET,512,0,0,
                      destination_gfx,draw.destination.pitch,draw.clip.x1,draw.clip.y1,
                      draw.clip.x2-draw.clip.x1,draw.clip.y2-draw.clip.y1)||!gpu_scene_submit())
        return render_stop("GPU: staged render copy failed","TILECOPY");
    return true;
}
static bool render_upload_source(const void* source,uint64_t bytes,uint64_t revision=0) {
    GpuPerfTimer timer(g_gpu_perf.upload_ticks);
    if(!source||!bytes||bytes>GPU_SOURCE_BYTES)
        return render_stop("GPU: invalid RCS upload size","SIZE");
    unsigned slot=0;
    for(unsigned i=0;i<4;++i){
        if(g_render_uploads[i].cache.source==source){slot=i;break;}
        if(g_render_uploads[i].last_used<g_render_uploads[slot].last_used)slot=i;
    }
    g_render_upload_slot=slot;
    auto& upload=g_render_uploads[slot];
    upload.last_used=++g_render_upload_clock;
    if(upload.cache.matches(source,bytes,revision,g_gpu_surface_epoch)){
        ++g_gpu_perf.upload_reuses;return true;
    }
    if(bytes>upload.capacity){
        uint64_t capacity=1024*1024;
        while(capacity<bytes)capacity*=2;
        auto* pixels=gpu_alloc_aligned(capacity);
        if(!pixels||!render_map(render_source_address(slot),pixels,capacity))
            return render_stop("GPU: RCS upload allocation/mapping failed","UPLOAD");
        // Old allocations remain pinned, as with the private context itself.
        upload.pixels=pixels;upload.capacity=capacity;
        ++g_render_map_updates;
    }else ++g_render_map_reuses;
    memcpy(upload.pixels,source,bytes);
    upload.width=0;
    gpu_clflush_nofence(upload.pixels,bytes);gpu_mfence();
    upload.cache.record(source,bytes,revision,g_gpu_surface_epoch);
    ++g_gpu_perf.uploads;g_gpu_perf.upload_bytes+=bytes;
    return true;
}

static bool gpu_render_scene(const GpuScenePlan& plan) {
    if(!g_render_ready || !g_gpu_ready || plan.client_count>GpuScenePlan::max_clients ||
       plan.count>GpuScenePlan::capacity)return false;
    unsigned uploaded_source=UINT32_MAX;
    for(unsigned i=0;i<plan.count;++i){
        const auto& copy=plan.copies[i];
        if(copy.source>=plan.client_count)return render_stop("GPU: invalid RCS scene source","SOURCE");
        const auto& c=plan.clients[copy.source];
        gpu_trace_stage("MAP");
        uint64_t bytes=(uint64_t)c.pitch*c.height;
        if(uploaded_source!=copy.source){
            if(!render_upload_source(c.pixels,bytes,c.revision))return false;
            uploaded_source=copy.source;
        }
        auto& upload=g_render_uploads[g_render_upload_slot];
        GpuTextureDraw draw{{render_source_address(g_render_upload_slot),c.width,c.height,c.pitch},
            {g_gpu_direct_back<0?RENDER_MASTER_VA:RENDER_SCANOUT_VA[g_gpu_direct_back],
             (uint32_t)fb->width,(uint32_t)fb->height,(uint32_t)fb->pitch},
            {c.client.x1,c.client.y1,c.client.x2,c.client.y2},
            {copy.destination.x1,copy.destination.y1,copy.destination.x2,copy.destination.y2},c.premultiplied};
        if(!gpu_texture_draw_valid(draw))return render_stop("GPU: invalid RCS draw","DRAW");
        GpuRenderRect rects[GPU_RENDER_MAX_RECTS]{draw.clip};
        unsigned count=1;
        if(!draw.blend){
            // Occlusion splits one scaled client into disjoint rectangles.
            // Keep their sampling coordinates, but retire one draw for the
            // group instead of scheduling the same context for each piece.
            while(i+1<plan.count && count<GPU_RENDER_MAX_RECTS &&
                  plan.copies[i+1].source==copy.source){
                const auto& r=plan.copies[++i].destination;
                rects[count++]={r.x1,r.y1,r.x2,r.y2};
                if(r.x1<draw.clip.x1)draw.clip.x1=r.x1;
                if(r.y1<draw.clip.y1)draw.clip.y1=r.y1;
                if(r.x2>draw.clip.x2)draw.clip.x2=r.x2;
                if(r.y2>draw.clip.y2)draw.clip.y2=r.y2;
            }
        }
        if(draw.blend){
            if(upload.width!=c.width||upload.height!=c.height||upload.pitch!=c.pitch){
                GpuPerfTimer timer(g_gpu_perf.upload_ticks);
                upload.coverage.scan(upload.pixels,c.width,c.height,c.pitch);
                upload.width=c.width;upload.height=c.height;upload.pitch=c.pitch;
            }
            count=upload.coverage.clips(draw,rects,GPU_ALPHA_MERGE);
            uint64_t area=(uint64_t)(draw.clip.x2-draw.clip.x1)*(draw.clip.y2-draw.clip.y1);
            while(i+1<plan.count && plan.copies[i+1].source==copy.source){
                const auto& r=plan.copies[i+1].destination;
                GpuTextureDraw next=draw;next.clip={r.x1,r.y1,r.x2,r.y2};
                if(!gpu_texture_draw_valid(next))return render_stop("GPU: invalid RCS alpha fragment","DRAW");
                if(!upload.coverage.append_clips(next,rects,count,GPU_ALPHA_MERGE))break;
                ++i;
                area+=(uint64_t)(r.x2-r.x1)*(r.y2-r.y1);
                if(r.x1<draw.clip.x1)draw.clip.x1=r.x1;
                if(r.y1<draw.clip.y1)draw.clip.y1=r.y1;
                if(r.x2>draw.clip.x2)draw.clip.x2=r.x2;
                if(r.y2>draw.clip.y2)draw.clip.y2=r.y2;
            }
            uint64_t pixels=0;
            for(unsigned j=0;j<count;++j)
                pixels+=(uint64_t)(rects[j].x2-rects[j].x1)*(rects[j].y2-rects[j].y1);
            g_gpu_perf.rcs_alpha_pixels+=pixels;
            g_gpu_perf.rcs_alpha_skipped+=area-pixels;
            if(!count)continue;
        }
        // Give the UC render target ownership only of the lines touched by
        // the submitted geometry, including cache-line rounding at the edges.
        auto handoff=[&](){
            GpuPerfTimer timer(g_gpu_perf.handoff_ticks);
            // Direct targets have a single WC CPU mapping and explicit UC
            // GPU surface policy. They cannot hold dirty CPU cache lines.
            // The hidden-scanout boot readback checks this ownership sequence.
            // Keep the full cache handoff for the WB retained master.
            for(unsigned j=0;g_gpu_direct_back<0 && j<count;++j){
                const auto& r=rects[j];
                for(int y=r.y1;y<r.y2;++y)
                    gpu_clflush_nofence(g_master_backbuffer+(uint64_t)y*fb->pitch+r.x1*4,(r.x2-r.x1)*4);
            }
            gpu_mfence();
        };
        gpu_trace_stage("FLUSH");
        handoff();
        gpu_trace_stage("BATCH");
        // Render the clipped region directly into the pinned master buffer.
        // Retirement still precedes CPU overlays and presentation. This
        // removes per-tile context submissions and scratch-to-master copies.
        if(!render_texture(draw,false,rects,count))
            return g_render_ready?render_stop("GPU: RCS texture draw failed","DRAW"):false;
        gpu_trace_stage("READBACK");
        handoff();
    }
    return true;
}
static uint32_t render_probe_pixel(unsigned x,unsigned y,unsigned frame) {
    unsigned red=x*255/479,green=y*255/319;
    unsigned blue=(x*111/479+y*111/319)/2+frame;
    return 0xFF000000u|(red<<16)|(green<<8)|blue;
}
static bool render_resize_probe() {
    constexpr unsigned width=1920,height=1080,pitch=width*4;
    auto* source=(uint32_t*)gpu_alloc_aligned(480*320*4);
    auto* destination=(uint32_t*)gpu_alloc_aligned(pitch*height);
    if(!source||!destination||!render_map(RENDER_SOURCE_VA,(uint8_t*)source,480*320*4)||
       !render_map(RENDER_MASTER_VA,g_render_storage+RENDER_SCRATCH_OFFSET,128*128*4)||
       !gtt_map_range(g_gpu_source_base,(uint8_t*)destination,pitch*height))
        return render_stop("GPU: resize probe allocation/mapping failed");
    gtt_flush();g_gpu_mapping_cache.valid=false;
    print((char*)"GPU: isolated STATIC SOLID repetition starting");
    for(unsigned i=0;i<width*height;++i)destination[i]=0xFF102030;
    gpu_clflush_nofence(destination,pitch*height);gpu_mfence();
    GpuTextureDraw fixed{{RENDER_SOURCE_VA,480,320,1920},
        {RENDER_MASTER_VA,width,height,pitch},{16,16,144,144},{16,16,144,144},false};
    for(unsigned repeat=0;repeat<1024;++repeat)
        if(!render_staged_texture(fixed,g_gpu_source_base,true))return false;
    gpu_clflush_nofence(destination,pitch*height);gpu_mfence();
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){
        bool inside=x>=16&&x<144&&y>=16&&y<144;
        if(((volatile uint32_t*)destination)[y*width+x]!=(inside?0xFFFFFFFF:0xFF102030))
            return render_stop("GPU: static solid repetition pixel mismatch");
    }
    print((char*)"GPU: isolated STATIC SOLID repetition readback PASSED");
    const unsigned shapes[][2]={{851,706},{1140,789},{1345,841},{1054,860}};
    for(unsigned test=0;test<12;++test){
        bool solid=test<8,aligned=test<4;
        if(test==0)print((char*)"GPU: isolated ALIGNED SOLID shader sweep starting");
        if(test==4)print((char*)"GPU: isolated ALIGNED SOLID PASSED; original SOLID sweep starting");
        if(test==8)print((char*)"GPU: isolated original SOLID PASSED; TEXTURED sweep starting");
        gpu_log_hex("GPU isolated resize probe case:",test);
        uint32_t color=solid?0xFFFFFFFF:0xFF206080u+(test-8)*0x101010;
        for(unsigned i=0;i<480*320;++i)source[i]=color;
        for(unsigned i=0;i<width*height;++i)destination[i]=0xFF102030;
        gpu_clflush_nofence(source,480*320*4);
        gpu_clflush_nofence(destination,pitch*height);gpu_mfence();
        GpuTextureDraw draw{{RENDER_SOURCE_VA,480,320,1920},
            {RENDER_MASTER_VA,width,height,pitch},
            {220,160,220+(int)shapes[test%4][0],160+(int)shapes[test%4][1]},{},false};
        if(aligned){
            draw.client.x1=224;
            draw.client.x2=224+((shapes[test%4][0]+7)&~7u);
            draw.client.y2=160+((shapes[test%4][1]+7)&~7u);
        }
        for(unsigned repeat=0;repeat<8;++repeat){
            // Match the demo's frame cadence while keeping scheduler/desktop
            // activity out of this isolated test. All prior GPU work retired.
            uint64_t idle_started=gpu_tsc(),idle_ticks=tsc_hz/30;
            while(gpu_tsc()-idle_started<idle_ticks)__asm__ volatile("pause");
            // Exercise live uploads with changing pixels and a fixed mapping.
            for(unsigned y=0;y<320;++y)for(unsigned x=0;x<480;++x)
                source[y*480+x]=solid?color:render_probe_pixel(x,y,repeat);
            if(!render_upload_source(source,480*320*4))return false;
            for(int top=draw.client.y1;top<draw.client.y2;top+=128)
                for(int left=draw.client.x1;left<draw.client.x2;left+=128){
                    draw.clip={left,top,left+128<draw.client.x2?left+128:draw.client.x2,
                               top+128<draw.client.y2?top+128:draw.client.y2};
                    if(!render_staged_texture(draw,g_gpu_source_base,solid))return false;
                }
        }
        gpu_clflush_nofence(destination,pitch*height);gpu_mfence();
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){
            bool inside=x>=(unsigned)draw.client.x1&&x<(unsigned)draw.client.x2&&
                        y>=(unsigned)draw.client.y1&&y<(unsigned)draw.client.y2;
            uint32_t actual=((volatile uint32_t*)destination)[y*width+x];
            uint32_t expected=inside?color:0xFF102030;
            if(inside&&!solid){
                unsigned sx=(uint64_t)(2*(x-draw.client.x1)+1)*480/(2*(draw.client.x2-draw.client.x1));
                unsigned sy=(uint64_t)(2*(y-draw.client.y1)+1)*320/(2*(draw.client.y2-draw.client.y1));
                expected=render_probe_pixel(sx,sy,7);
                // Fractional nearest sampling may select an adjacent texel at
                // quantized boundaries. This gradient changes <=2 per channel.
                bool match=(actual>>24)==255;
                for(unsigned shift=0;shift<24;shift+=8){
                    int difference=(int)((actual>>shift)&255)-(int)((expected>>shift)&255);
                    if(difference < -2||difference > 2)match=false;
                }
                if(match)continue;
            }else if(actual==expected)continue;
            gpu_log_hex("GPU probe mismatch x/y:",((uint64_t)x<<32)|y);
            gpu_log_hex("GPU probe expected:",expected);
            gpu_log_hex("GPU probe actual:",actual);
            return render_stop("GPU: isolated resize probe pixel mismatch");
        }
    }
    print((char*)"GPU: isolated resized-window geometry readback PASSED");
    return true;
}

static bool render_source_cache_probe(uint32_t* source,uint32_t* destination){
    if(!g_render_cached_sources)return true;
    // Reuse the same source address with new premultiplied pixels. This tests
    // texture invalidation as well as sampling, scaling and source-over.
    for(unsigned frame=0;frame<8;++frame){
        for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x){
            unsigned alpha=(x+frame)%3==0?0:(x+frame)%3==1?128:255;
            unsigned red=((x*17+frame*13)&255)*alpha/255;
            unsigned green=((y*11+frame*7)&255)*alpha/255;
            source[y*16+x]=(alpha<<24)|(red<<16)|(green<<8)|(alpha/2);
        }
        uint32_t background=0xff204060+frame*0x010203;
        for(unsigned i=0;i<32*32;++i)destination[i]=background;
        gpu_clflush_nofence(source,4096);gpu_clflush_nofence(destination,4096);gpu_mfence();
        auto draw=gpu_render_probe_draw(2,RENDER_SOURCE_VA,RENDER_MASTER_VA);
        if(frame&1){draw.client={2,2,34,34};draw.clip={3,3,31,31};} // exact 2x scaling
        if(!render_texture(draw))return false;
        gpu_clflush_nofence(destination,4096);gpu_mfence();
        for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x){
            uint32_t expected=background;
            if(x>=(unsigned)draw.clip.x1&&x<(unsigned)draw.clip.x2&&
               y>=(unsigned)draw.clip.y1&&y<(unsigned)draw.clip.y2){
                unsigned width=draw.client.x2-draw.client.x1,height=draw.client.y2-draw.client.y1;
                unsigned sx=(2*(x-draw.client.x1)+1)*16/(2*width);
                unsigned sy=(2*(y-draw.client.y1)+1)*16/(2*height);
                expected=window_source_over(source[sy*16+sx],background);
            }
            if(((volatile uint32_t*)destination)[y*32+x]!=expected){
                g_render_cached_sources=false;g_render_source_cache_rejected=true;
                print((char*)"GPU: RCS cached source readback mismatch; UC sampling selected");
                return true; // Ordinary UC readback tests still must pass.
            }
        }
    }
    print((char*)"GPU: RCS cached texture source readback PASSED (8 updates)");
    return true;
}
static bool render_dispatch_probe(uint32_t* source){
    if(!GPU_RCS_ALPHA||!tsc_hz)return true;
    unsigned width=fb->width<1920?fb->width:1920;
    unsigned height=fb->height<1080?fb->height:1080;
    if(width<64||height<64)return true;
    unsigned pitch=(width*4+63)&~63u;
    uint64_t bytes=(uint64_t)pitch*height;
    auto* destination=(uint32_t*)gpu_alloc_aligned(bytes);
    if(!destination){
        print((char*)"GPU: dispatch probe allocation unavailable; retaining 16 threads");
        return true;
    }
    if(!render_map(RENDER_MASTER_VA,(uint8_t*)destination,bytes))
        return render_stop("GPU: dispatch probe mapping failed","DISPATCH");
    GpuTextureDraw draw{{RENDER_SOURCE_VA,16,16,64},{RENDER_MASTER_VA,width,height,pitch},
        {0,0,(int)width,(int)height},{2,2,(int)width-2,(int)height-2},true};
    const unsigned choices[]={16,32,63,16};
    unsigned selected=16;uint64_t times[3]{};bool higher_rejected=false;
    auto report=[&](unsigned threads,uint64_t average,bool verified){
        char line[192];char* end=line;
        auto text=[&](const char* value){while(*value&&end<line+sizeof(line)-1)*end++=*value++;};
        auto number=[&](uint64_t value){char digits[24];to_str(value,digits);text(digits);};
        text("GPU dispatch: threads=");number(threads);
        text(" averageUs=");number(average*1000000/tsc_hz);
        text(" verified=");number(verified);
        text(" width=");number(width);text(" height=");number(height);
        *end=0;print(line);
    };
    for(unsigned profile=0;profile<4;++profile){
        unsigned choice=choices[profile];
        g_render_ps_threads=choice;
        uint64_t total=0;bool verified=true;
        // Warm the pipeline once, then measure three completed submissions.
        // CPU filling/flush/readback is outside the measured draw interval.
        for(unsigned repeat=0;repeat<4;++repeat){
            uint32_t foreground=0x80201008u+repeat*0x00010301u;
            uint32_t background=0xff204060u;
            for(unsigned i=0;i<256;++i)source[i]=foreground;
            for(uint64_t i=0;i<bytes/4;++i)destination[i]=background;
            gpu_clflush_nofence(source,4096);gpu_clflush_nofence(destination,bytes);gpu_mfence();
            uint64_t started=gpu_tsc();
            if(!render_texture(draw))return false; // Preserve timeout/retirement fallback.
            uint64_t elapsed=gpu_tsc()-started;
            if(repeat)total+=elapsed;
            gpu_clflush_nofence(destination,bytes);gpu_mfence();
            uint32_t blended=window_source_over(foreground,background);
            for(unsigned y=0;y<height&&verified;++y)for(unsigned x=0;x<pitch/4;++x){
                bool inside=x>=2&&x<width-2&&y>=2&&y<height-2;
                if(((volatile uint32_t*)destination)[(uint64_t)y*(pitch/4)+x]!=(inside?blended:background)){
                    verified=false;break;
                }
            }
            if(!verified)break;
        }
        uint64_t average=verified?total/3:0;
        report(choice,average,verified);
        if(!verified){
            g_render_ps_threads=selected;
            if(choice==16)return render_stop("GPU: conservative dispatch readback failed","DISPATCH");
            higher_rejected=true;
            print((char*)"GPU: higher dispatch readback rejected; retaining verified limit");
            break;
        }
        if(profile<3)times[profile]=average;
        else if(average<times[0])times[0]=average;
    }
    // Recheck the baseline after the sweep so clock warmup alone cannot make
    // a higher limit appear faster. Compare against the better baseline run.
    uint64_t best=times[0];
    if(!higher_rejected)for(unsigned profile=1;profile<3;++profile)
        if(gpu_render_dispatch_faster(best,times[profile])){selected=choices[profile];best=times[profile];}
    g_render_ps_threads=selected;
    print((char*)"GPU: selected RCS pixel-shader threads:");
    char count[24];to_str(selected,count);print(count);
    return true;
}
static bool render_timestamp_probe(){
    if(!GPU_RCS_ALPHA)return true;
    g_bcs_timestamp_hz=gpu_calibrate_timestamp_clock(GPU_BCS_BASE);
    g_render_timestamp_hz=gpu_calibrate_timestamp_clock(0x2000);
    // These batches write only the already pinned fence pages. Unknown clock
    // behavior disables timing; a command timeout uses isolated CPU fallback.
    g_render_isolated_probe=true;
    bool ok=true;
    if(g_bcs_timestamp_hz){
        g_bcs_timing_probe={};
        if(!gpu_scene_timing_begin(true)||!gpu_scene_submit()){
            render_stop("GPU: BCS timestamp probe failed","TIMING");ok=false;
        }else g_bcs_timestamp_ready=g_bcs_timing_probe.samples==1&&!g_bcs_timing_probe.invalid;
    }
    if(ok&&g_render_timestamp_hz){
        g_render_timing_probe={};
        ok=render_submit(false,true);
        if(ok)g_render_timestamp_ready=g_render_timing_probe.samples==1&&!g_render_timing_probe.invalid;
    }
    g_render_isolated_probe=false;
    if(!ok)return false;
    char line[192];char* end=line;
    auto text=[&](const char* value){while(*value&&end<line+sizeof(line)-1)*end++=*value++;};
    auto number=[&](uint64_t value){char digits[24];to_str(value,digits);text(digits);};
    text("GPU timestamp clocks: bcsHz=");number(g_bcs_timestamp_hz);
    text(" bcsVerified=");number(g_bcs_timestamp_ready);
    text(" rcsHz=");number(g_render_timestamp_hz);
    text(" rcsVerified=");number(g_render_timestamp_ready);*end=0;print(line);
    return true;
}
static bool gpu_render_initialize() {
    if(!GPU_RENDER || !g_gpu_render_gfx ||
       (uint64_t)fb->pitch*fb->height>RENDER_SOURCE_VA-RENDER_MASTER_VA)return false;
    print((char*)"GPU: initializing experimental Gen9 logical render context");
    print((char*)"GPU: direct rendering with Gen9 workarounds (SCORE v11)");
    g_render_storage=gpu_alloc_aligned(GPU_RENDER_OWNED_BYTES);
    g_render_pml4=render_new_table();
    if(!g_render_storage||!g_render_pml4){
        print((char*)"GPU: render context allocation failed");return false;
    }
    if(!render_forcewake())return false;
    if((g_igpu.devid>>8)==0x59&&!render_disable_firmware_power_gating()){
        print((char*)"GPU: firmware power-gating disable failed");
        return false;
    }
    if((g_igpu.devid>>8)==0x59){
        // i915 gen9_gt_workarounds_init()/kbl_gt_workarounds_init().
        // The global HDC invalidation setting is coupled to the context's
        // force-noncoherent mode. Preserve every unrelated global GT bit.
        uint32_t gam=render_read(0x4090),clock=render_read(0x940C);
        if(gam==UINT32_MAX||clock==UINT32_MAX)return false;
        gpu_log_hex("GPU RCS GAM workaround before:",gam);
        gpu_log_hex("GPU RCS EU clock workaround before:",clock);
        render_write(0x4090,gam|(1u<<25)|(1u<<8));
        render_write(0x940C,clock|(1u<<14));
        uint32_t gam_after=render_read(0x4090),clock_after=render_read(0x940C);
        gpu_log_hex("GPU RCS GAM workaround after:",gam_after);
        gpu_log_hex("GPU RCS EU clock workaround after:",clock_after);
        if((gam_after&((1u<<25)|(1u<<8)))!=((1u<<25)|(1u<<8))||
           !(clock_after&(1u<<14)))return false;
    }
    uint32_t fuse2=render_read(0x9120),eu_disable[3]{};
    for(unsigned s=0;s<3;++s)
        if(fuse2&(1u<<(25+s)))eu_disable[s]=render_read(0x9134+4*s);
    if(!gpu_render_power_request(fuse2,eu_disable,g_render_power_request)){
        print((char*)"GPU: invalid render topology; skipping RCS initialization");
        render_write(0xA278,0x00010000);
        return false;
    }
    gpu_log_hex("GPU RCS context power request:",g_render_power_request);
    if(!render_reset())return render_stop("GPU: RCS boot reset failed");
    if(!gtt_map_range(g_gpu_render_gfx,g_render_storage,GPU_RENDER_OWNED_BYTES))
        return render_stop("GPU: RCS context GGTT mapping failed");
    gtt_flush();
    if(!render_map(RENDER_BATCH_VA,g_render_storage+RENDER_BATCH_OFFSET,4096) ||
       !render_map(RENDER_MASTER_VA,g_master_backbuffer,(uint64_t)fb->pitch*fb->height))
        return render_stop("GPU: RCS page table setup failed");
    gpu_clflush_nofence(g_render_storage,GPU_RENDER_OWNED_BYTES);gpu_mfence();
    // Explicit UC destination policy plus an optional read-only texture cache
    // entry. Leave the blitter/display cache policy alone.
    if(!render_program_context())return render_stop("GPU: RCS context programming failed");
    gpu_log_hex("GPU RCS initial L3 allocation:",render_read(0x7034));
    if(!render_submit(false))return false;
    print((char*)"GPU: RCS logical-context fence and retirement PASSED");
    // Reach the padded final slot before the first pixel probe. That probe
    // wraps to slot zero, with fenced readback before enabling desktop RCS.
    if(GPU_TRACE)
        for(unsigned slot=1;slot<GPU_RENDER_RING_BYTES/GPU_RENDER_PACKET_BYTES-1;++slot)
            if(!render_submit(false))return false;
    // Pixel verification targets dedicated RAM, never scanout or the desktop.
    auto* source=(uint32_t*)gpu_alloc_aligned(4096);
    auto* destination=(uint32_t*)gpu_alloc_aligned(4096);
    if(!source||!destination||!render_map(RENDER_SOURCE_VA,(uint8_t*)source,4096)||
       !render_map(RENDER_MASTER_VA,(uint8_t*)destination,4096))
        return render_stop("GPU: RCS readback allocation failed");
    if(!render_source_cache_probe(source,destination))return false;
    if(GPU_RCS_ALPHA){
        g_render_isolated_probe=true;
        bool dispatch_ok=render_dispatch_probe(source);
        g_render_isolated_probe=false;
        if(!dispatch_ok)return false;
        // Restore the small fixture after calibration, then run all normal
        // pixel/alpha/scanout tests at the chosen limit.
        if(!render_map(RENDER_MASTER_VA,(uint8_t*)destination,4096))
            return render_stop("GPU: dispatch probe restore failed","DISPATCH");
    }
    for(unsigned test=0;test<(GPU_RCS_ALPHA?4u:3u);++test){
        for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)
            source[y*16+x]=test==3?(x>=2&&x<(y<8?10u:8u)?0:0x80402010u):
                test==2?0x80402010u:0xFF000000u|(x*13u<<16)|(y*11u<<8)|((x+y)*7u);
        for(unsigned i=0;i<32*32;++i)destination[i]=0xFF204060;
        gpu_clflush_nofence(source,4096);gpu_clflush_nofence(destination,4096);gpu_mfence();
        // 1:1 copy and exact 2x enlargement sample texel centers or quarter
        // positions. Fractional 16-to-12 scaling put samples exactly on texel
        // boundaries, where interpolation roundoff can legitimately pick either
        // neighbor. Keep exact comparisons, using unambiguous sample positions.
        GpuTextureDraw draw=gpu_render_probe_draw(test==3?2:test,RENDER_SOURCE_VA,RENDER_MASTER_VA);
        if(test==3){
            GpuAlphaCoverage coverage;
            coverage.scan(reinterpret_cast<const uint8_t*>(source),16,16,64);
            GpuRenderRect rects[GPU_RENDER_MAX_RECTS];
            unsigned count=coverage.clips(draw,rects);
            if(count<2)return render_stop("GPU: RCS alpha coverage probe missing bands");
            if(!render_texture(draw,false,rects,count))return false;
        }else if(!render_texture(draw))return false;
        gpu_clflush_nofence(destination,4096);gpu_mfence();
        for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x){
            uint32_t expected=0xFF204060;
            if(x>=3&&y>=3&&x<(unsigned)draw.clip.x2&&y<(unsigned)draw.clip.y2){
                unsigned sx=(2*(x-2)+1)*draw.source.width/32;
                unsigned sy=(2*(y-2)+1)*draw.source.height/32;
                if(test>=2)expected=source[sy*16+sx]?0xFF504040u:0xFF204060u;
                else expected=source[sy*16+sx];
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
    if(GPU_TRACE){
        g_render_isolated_probe=true;
        bool resize_ok=render_resize_probe();
        g_render_isolated_probe=false;
        if(!resize_ok)return false;
        print((char*)"GPU: RCS ring wrap and slot reuse readback PASSED");
    }
    // Gen9's context hang workaround forces non-coherent HDC operation. Keep
    // explicit pixel ownership transfers; a no-flush probe would test outside
    // that contract rather than establish a supported coherent rendering mode.
    print((char*)"GPU: Gen9 RCS hang workarounds enabled; explicit pixel handoffs");
    if(!render_map(RENDER_MASTER_VA,g_master_backbuffer,(uint64_t)fb->pitch*fb->height))
        return render_stop("GPU: RCS desktop mapping failed");
    // Both scanouts are still hidden. Check the actual desktop pitch, the
    // BCS -> RCS -> CPU ownership sequence and independent PPGTT bindings.
    uint64_t scanout_bytes=(uint64_t)fb->pitch*fb->height;
    if(GPU_RCS_ALPHA && g_gpu_present_ready && fb->width>=32 && fb->height>=32 &&
       scanout_bytes<=RENDER_SCANOUT_VA[1]-RENDER_SCANOUT_VA[0]){
        for(unsigned i=0;i<256;++i)source[i]=0x80402010u;
        gpu_clflush_nofence(source,1024);gpu_mfence();
        bool matches=render_map(RENDER_SOURCE_VA,(uint8_t*)source,4096);
        for(unsigned i=0;i<2 && matches;++i){
            matches=render_map(RENDER_SCANOUT_VA[i],g_gpu_scanout_cpu[i],scanout_bytes);
            if(!matches)break;
            uint32_t background=i?0xff204020:0xff204060;
            if(!gpu_emit_fill(&g_gpu,g_gpu_scanout_gfx[i],fb->pitch,0,0,32,32,0xff000000) ||
               !gpu_scene_submit())return gpu_scene_abort("GPU: hidden scanout fill probe failed");
            // RCS must observe these WC CPU stores without CLFLUSH, including
            // destination reads for blending. Readback then checks RCS -> CPU.
            for(unsigned y=0;y<32;++y){
                auto* row=(volatile uint32_t*)(g_gpu_scanout_cpu[i]+y*fb->pitch);
                for(unsigned x=0;x<32;++x)row[x]=background;
            }
            gpu_mfence();
            auto draw=gpu_render_probe_draw(2,RENDER_SOURCE_VA,RENDER_SCANOUT_VA[i]);
            draw.destination={(uint64_t)RENDER_SCANOUT_VA[i],(uint32_t)fb->width,
                              (uint32_t)fb->height,(uint32_t)fb->pitch};
            if(!render_texture(draw))return false;
            for(unsigned j=0;j<=i;++j)for(unsigned y=0;y<32;++y){
                auto* row=(volatile uint32_t*)(g_gpu_scanout_cpu[j]+y*fb->pitch);
                for(unsigned x=0;x<32;++x){
                    bool inside=x>=3 && x<17 && y>=3 && y<17;
                    uint32_t expected=inside?(j?0xff504020:0xff504040):(j?0xff204020:0xff204060);
                    if(row[x]!=expected)matches=false;
                }
            }
        }
        g_gpu_direct_verified=matches;
        print((char*)(matches?"GPU: direct scanout BCS/RCS readback PASSED":
                              "GPU: direct scanout readback unavailable; using master copy"));
    }
    if(!render_timestamp_probe())return false;
    g_render_ready=true;
    print((char*)"GPU: RCS textured copy, scaling and premultiplied alpha readback PASSED");
    if(GPU_RCS_ALPHA)print((char*)"GPU: RCS interior alpha gap readback PASSED");
    if(GPU_RCS_ALPHA)print((char*)(g_render_cached_sources?
        "GPU: RCS texture sampling selected L3+LLC WB; destination UC":
        "GPU: RCS texture sampling selected UC; destination UC"));
    gpu_status_set(g_gpu_present_ready?"GPU:rainbow+RCS flip UC":"GPU:rainbow+RCS LLC");
    return true;
}
#endif
