// QEMU integration fixture only. Compile with CXX='clang++ -DGPU_COMPOSITOR_TEST'.
// This exercises the real WM hook/fallback; it does not emulate Intel hardware.
static uint64_t g_gpu_test_calls=0,g_gpu_test_rainbow_calls=0;
static bool g_gpu_test_fail=false;
static uint32_t g_gpu_test_sample[3]{};
static uint8_t* g_gpu_test_recovery;
#ifdef GPU_PRESENT_TEST
static uint64_t g_gpu_test_present_calls=0;
static bool g_gpu_test_present_fail=false;
static uint8_t* g_gpu_test_present_buffers[2]{};
static unsigned g_gpu_test_present_front=1;
static bool gpu_test_present(const OverlayFrame& frame,const uint32_t* sprite) {
    ++g_gpu_test_present_calls;
    unsigned back=1-g_gpu_test_present_front;
    uint64_t bytes=(uint64_t)fb->pitch*fb->height;
    memcpy(g_gpu_test_present_buffers[back],g_master_backbuffer,bytes);
    if(g_gpu_test_present_fail)return false; // prepared frame did not latch
    OverlayTarget target{g_gpu_test_present_buffers[back],g_master_backbuffer,(int)fb->width,(int)fb->height,(int)fb->pitch};
    const OverlayFrame empty{{-999,-999,0,0},{},0};
    overlay_render_transition(target,empty,frame,sprite,true);gpu_mfence();
    memcpy_vram_sse_headless(fb->address,g_gpu_test_present_buffers[back],bytes);gpu_mfence();
    g_gpu_test_present_front=back;
    return true;
}
#endif
static bool gpu_test_compose(const GpuScenePlan& plan) {
    ++g_gpu_test_calls;
    for(unsigned i=0;i<plan.count;++i){
        const auto& copy=plan.copies[i];const auto& c=plan.clients[copy.source];auto r=copy.destination;
        for(int y=r.y1;y<r.y2;++y){
            auto* to=(uint32_t*)(g_master_backbuffer+(uint64_t)y*fb->pitch)+r.x1;
            if(c.rainbow){
                for(int x=r.x1;x<r.x2;++x)
                    to[x-r.x1]=plan.rainbow_colors[((uint64_t)plan.scroll+copy.source_x+x-r.x1)%720];
            }else memcpy(to,(uint8_t*)c.pixels+(uint64_t)(copy.source_y+y-r.y1)*c.pitch+copy.source_x*4,(r.x2-r.x1)*4);
        }
        if(c.rainbow){
            ++g_gpu_test_rainbow_calls;
            g_gpu_test_sample[0]=(r.x1+r.x2)/2;g_gpu_test_sample[1]=(r.y1+r.y2)/2;
            g_gpu_test_sample[2]=plan.rainbow_colors[((uint64_t)plan.scroll+copy.source_x+g_gpu_test_sample[0]-r.x1)%720];
        }
        if(g_gpu_test_fail){
            // Simulate late-DMA quarantine after a partially executed batch.
            g_master_backbuffer=g_gpu_test_recovery;
            g_gpu_present_hook=nullptr;
            g_gpu_quarantine_sources=true;
            g_gpu_test_recovery=nullptr;
            return false;
        }
    }
    return true;
}
static void gpu_test_install() {
    g_gpu_test_recovery=(uint8_t*)kmalloc((uint64_t)fb->pitch*fb->height);
    if(!g_gpu_test_recovery)hcf();
    memset(g_gpu_test_recovery,0,(uint64_t)fb->pitch*fb->height);
    g_gpu_compose_hook=gpu_test_compose;
#ifdef GPU_PRESENT_TEST
    for(auto& pointer:g_gpu_test_present_buffers){
        pointer=(uint8_t*)kmalloc((uint64_t)fb->pitch*fb->height);
        if(!pointer)hcf();
    }
    g_gpu_present_hook=gpu_test_present;
#endif
    gpu_status_set("GPU:test CPU backend");
}
