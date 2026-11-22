// QEMU integration fixture only. Compile with CXX='clang++ -DGPU_COMPOSITOR_TEST'.
// This exercises the real WM hook/fallback; it does not emulate Intel hardware.
static uint64_t g_gpu_test_calls=0,g_gpu_test_rainbow_calls=0;
static bool g_gpu_test_fail=false;
static uint32_t g_gpu_test_sample[3]{};
static uint8_t* g_gpu_test_recovery;
#ifdef GPU_RENDER_TEST
static uint64_t g_gpu_test_render_calls=0;
static uint64_t g_gpu_test_demo_render_calls=0;
static uint8_t g_gpu_test_render_fail=0;
static uint32_t g_gpu_test_scaled_sample[3]{};
static bool gpu_test_render(const GpuScenePlan& plan) {
    ++g_gpu_test_render_calls;
    for(unsigned i=0;i<plan.count;++i){
        const auto& copy=plan.copies[i];const auto& c=plan.clients[copy.source];
        if(c.width==480&&c.height==320)++g_gpu_test_demo_render_calls;
        GpuTextureDraw draw{{0x10000000,c.width,c.height,c.pitch},
            {0x2000000,(uint32_t)fb->width,(uint32_t)fb->height,(uint32_t)fb->pitch},
            {c.client.x1,c.client.y1,c.client.x2,c.client.y2},
            {copy.destination.x1,copy.destination.y1,copy.destination.x2,copy.destination.y2},false};
        uint32_t batch[1024];if(!gpu_build_texture_batch(batch,0x100000,draw))return false;
        // Decode the encoded geometry and scissor rather than model the plan's
        // clip directly, so packing or inclusive-edge mistakes affect pixels.
        const auto* vertices=batch+GPU_RENDER_VERTEX_OFFSET/4;
        const auto* scissor=batch+GPU_RENDER_SCISSOR_STATE/4;
        if(!(batch[GPU_RENDER_RASTER_DWORD]&(1u<<1)))return false;
        int left=(int16_t)vertices[6],top=(int16_t)(vertices[6]>>16);
        int right=(int16_t)vertices[0],bottom=(int16_t)(vertices[0]>>16);
        if(left>=right||top>=bottom)return false;
        float u1,u2,v1,v2;
        memcpy(&u1,vertices+7,4);memcpy(&u2,vertices+1,4);
        memcpy(&v1,vertices+8,4);memcpy(&v2,vertices+2,4);
        VisibleRect r{(int)(scissor[0]&65535),(int)(scissor[0]>>16),
                      (int)(scissor[1]&65535)+1,(int)(scissor[1]>>16)+1};
        r=visibility_intersect(r,{left,top,right,bottom});
        for(int y=r.y1;y<r.y2;++y){
            unsigned sy=(v1+(y+.5f-top)*(v2-v1)/(bottom-top))*c.height;
            if(sy>=c.height)sy=c.height-1;
            auto* row=(const uint32_t*)((const uint8_t*)c.pixels+(uint64_t)sy*c.pitch);
            auto* to=(uint32_t*)(g_master_backbuffer+(uint64_t)y*fb->pitch);
            for(int x=r.x1;x<r.x2;++x){
                unsigned sx=(u1+(x+.5f-left)*(u2-u1)/(right-left))*c.width;
                if(sx>=c.width)sx=c.width-1;
                to[x]=row[sx];
            }
        }
        unsigned x=(r.x1+r.x2)/2,y=(r.y1+r.y2)/2;
        unsigned sx=(uint64_t)(2*(x-c.client.x1)+1)*c.width/(2*(c.client.x2-c.client.x1));
        unsigned sy=(uint64_t)(2*(y-c.client.y1)+1)*c.height/(2*(c.client.y2-c.client.y1));
        g_gpu_test_scaled_sample[0]=x;g_gpu_test_scaled_sample[1]=y;
        g_gpu_test_scaled_sample[2]=*(const uint32_t*)((const uint8_t*)c.pixels+(uint64_t)sy*c.pitch+sx*4);
        if(g_gpu_test_render_fail==2){
            g_gpu_recovery_buffer=g_gpu_test_recovery;
            g_render_ready=true;
            return render_stop("Injected RCS timeout","PREDRAW");
        }
        if(g_gpu_test_render_fail)return false;
    }
    return true;
}
#endif
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
#ifdef GPU_RENDER_TEST
    auto* fixture=wm_create_window("GPU scaled fixture",1020,180,64,48,false,false,WIN_TYPE_USER,0);
    if(!fixture)hcf();
    for(unsigned y=0;y<48;++y)for(unsigned x=0;x<64;++x)
        fixture->buffer[y*64+x]=0xFF000000u|(x*3u<<16)|(y*5u<<8)|((x+y)*2u);
    fixture->w=128;fixture->h=96;
    auto* neighbor=wm_create_window("GPU native fixture",1148,180,128,96,false,false,WIN_TYPE_USER,0);
    if(!neighbor)hcf();
    for(unsigned i=0;i<128*96;++i)neighbor->buffer[i]=0xFF113355;
    g_gpu_render_hook=gpu_test_render;
#endif
#ifdef GPU_PRESENT_TEST
    for(auto& pointer:g_gpu_test_present_buffers){
        pointer=(uint8_t*)kmalloc((uint64_t)fb->pitch*fb->height);
        if(!pointer)hcf();
    }
    g_gpu_present_hook=gpu_test_present;
#endif
    gpu_status_set("GPU:test CPU backend");
}
