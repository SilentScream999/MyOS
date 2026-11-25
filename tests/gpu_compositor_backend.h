// QEMU integration fixture only. Compile with CXX='clang++ -DGPU_COMPOSITOR_TEST'.
// This exercises the real WM hook/fallback; it does not emulate Intel hardware.
#include "src/drivers/gpu_upload_cache.h"
static uint64_t g_gpu_test_calls=0,g_gpu_test_rainbow_calls=0;
static uint64_t g_gpu_test_bcs_pixels=0,g_gpu_test_rcs_pixels=0;
static bool g_gpu_test_fail=false;
static uint32_t g_gpu_test_sample[3]{};
static uint8_t* g_gpu_test_recovery;
// Invoke the shared repair traversal on the mock retained destination. The
// production direct-scanout gate requires physical Intel registers and hooks.
static VisibleRegion g_gpu_test_scene_repairs{};
static uint8_t g_gpu_test_scene_repair_request=0;
static bool g_gpu_test_scene_repair_accepted=false;
static int32_t g_gpu_test_tray_left=0;
static bool g_gpu_test_tray_enabled=false;
static bool g_gpu_test_tray_menu=false;
static unsigned g_gpu_test_tray_fps=0;
static uint8_t* g_gpu_test_taskbar_before=nullptr;
static void gpu_test_repair_scene(){
    if(!g_gpu_test_scene_repair_request)return;
    unsigned request=g_gpu_test_scene_repair_request;
    g_gpu_test_scene_repair_request=0;
    g_gpu_retained_master=g_master_backbuffer;g_gpu_direct_back=0;
    bool clock_valid=g_clock_valid;
    bool menu_open=g_menu_open;
    if((request==2||request==3)&&g_gpu_test_tray_menu){
        g_menu_open=true;wm_draw_start_menu(0,0,true);
    }
    if(request==2||request==3){g_desktop_fps=g_gpu_test_tray_fps;g_clock_valid=false;}
    if(request==4||request==5||request==6){
        Window* rain=nullptr;
        for(int i=0;i<g_window_count;++i)if(g_window_list[i]->type==WIN_TYPE_RAIN)rain=g_window_list[i];
        g_gpu_test_scene_repair_accepted=false;
        if(rain){
            int popup=g_taskbar_popup,group=g_taskbar_group_id,anchor=g_taskbar_anchor;
            int x=rain->x,y=rain->y,w=rain->w,h=rain->h;
            int cw=rain->canvas_width,ch=rain->canvas_height;
            bool maximized=rain->maximized;
            AudioVolume saved_volume=g_audio_mixer.value;
            AudioMixer saved_mixer=g_audio_mixer;
            AudioHotkeyQueue saved_keys=g_audio_hotkeys;
            DamageList saved_damage=g_damage;
            int saved_order[MAX_WINDOWS];for(int i=0;i<g_window_count;++i)saved_order[i]=g_window_order[i];
            g_menu_open=false;g_taskbar_popup=request>=5?3:2;g_taskbar_group_id=rain->id;g_taskbar_anchor=request>=5?wm_volume_tray_x():100;
            rain->x=0;rain->y=g_title_h;rain->w=fb->width;rain->h=fb->height-g_taskbar_h-g_title_h;
            rain->canvas_width=rain->w;rain->canvas_height=rain->h;rain->maximized=true;
            if(request==6){
                // Keep Boot Log visible but fully covered by opaque Rainbow.
                int at=-1;for(int i=0;i<g_window_count;++i)if(g_window_list[g_window_order[i]]==rain)at=i;
                if(at>=0){int index=g_window_order[at];for(int i=at;i+1<g_window_count;++i)g_window_order[i]=g_window_order[i+1];g_window_order[g_window_count-1]=index;}
            }
            auto panel=wm_taskbar_panel(wm_taskbar_snapshot());
            VisibleRect cut{panel.x,panel.y,panel.x+panel.w,panel.y+panel.h};
            bool ok=wm_prepare_retained_animation(rain)&&g_retained_animation_plan.count;
            if(request==6)ok=ok&&g_win_log&&g_win_log->visible&&!g_win_log->minimized&&
                wm_find_top_window(g_win_log->x+g_win_log->w/2,g_win_log->y+g_win_log->h/2)==g_window_order[g_window_count-1];
            for(unsigned i=0;ok&&i<g_retained_animation_plan.count;++i)
                ok=!visibility_valid(visibility_intersect(cut,g_retained_animation_plan.copies[i].destination));
            // Compare the entire rendered popup, including text/icon edges,
            // after corrupting its pixels and leaving a partial repair clip.
            // Repeat hover/non-hover on alternating destinations so cutout
            // acceptance alone cannot hide an incomplete retained panel.
            auto* expected=(uint8_t*)kmalloc((uint64_t)panel.w*panel.h*4);
            int mx=g_mouse_x,my=g_mouse_y;
            int left=g_ui_clip_left,right=g_ui_clip_right,top=g_ui_clip_top,bottom=g_ui_clip_bottom;
            auto deferred=g_deferred_alpha;g_deferred_alpha.clear();
            if(!expected)ok=false;
            for(unsigned pass=0;ok&&pass<4;++pass){
                if(request>=5){g_audio_mixer.value.percent=pass*25;g_audio_mixer.value.muted=pass&1;}
                if(request==6){
                    auto log_hash=[](){uint64_t hash=1469598103934665603ULL;for(unsigned i=0;i<sizeof(g_log_buf);++i)hash=(hash^((uint8_t*)g_log_buf)[i])*1099511628211ULL;return hash;};
                    uint64_t before_log=log_hash(),calls=g_gpu_test_calls;
                    {TtyIrqGuard guard;damage_clear(&g_damage);g_audio_hotkeys={};g_audio_hotkeys.queue(0x30);hda::consume_volume_keys();}
                    ok=ok&&before_log==log_hash()&&g_damage.n>0;
                    for(unsigned i=0;ok&&i<g_damage.n;++i){const auto& r=g_damage.r[i];ok=wm_repair_retained_foreground({(int)r.x1,(int)r.y1,(int)r.x2+1,(int)r.y2+1});}
                    ok=ok&&wm_repair_retained_foreground(cut)&&wm_repair_retained_foreground(
                        {wm_volume_tray_x(),(int)fb->height-(int)g_taskbar_h,wm_volume_tray_x()+(int)g_taskbar_h,(int)fb->height})&&g_gpu_test_calls==calls;
                }
                g_gpu_direct_back=pass%2;
                g_mouse_x=pass&1?panel.x+45:0;g_mouse_y=pass&1?panel.y+45:0;
                g_ui_clip_left=panel.x;g_ui_clip_right=panel.x+panel.w;
                g_ui_clip_top=panel.y;g_ui_clip_bottom=panel.y+panel.h;
                wm_taskbar_render_popup(panel.y,panel.y+panel.h-1);
                for(int py=0;py<panel.h;++py){
                    auto* row=g_master_backbuffer+(uint64_t)(panel.y+py)*fb->pitch+panel.x*4;
                    memcpy(expected+(uint64_t)py*panel.w*4,row,panel.w*4);
                    memset_32((uint32_t*)row,0xffdeadbe,panel.w);
                }
                g_ui_clip_right=panel.x+panel.w/2;g_ui_clip_top=panel.y+panel.h/2;
                ok=wm_draw_retained_animation(0)&&!g_retained_scene_invalidated;
                for(int py=0;ok&&py<panel.h;++py){
                    auto* wanted=(uint32_t*)(expected+(uint64_t)py*panel.w*4);
                    auto* actual=(uint32_t*)(g_master_backbuffer+(uint64_t)(panel.y+py)*fb->pitch+panel.x*4);
                    for(int px=0;ok&&px<panel.w;++px)ok=wanted[px]==actual[px];
                }
            }
            kfree(expected);g_deferred_alpha=deferred;
            g_mouse_x=mx;g_mouse_y=my;
            g_ui_clip_left=left;g_ui_clip_right=right;g_ui_clip_top=top;g_ui_clip_bottom=bottom;
            g_gpu_test_scene_repair_accepted=ok;
            rain->x=x;rain->y=y;rain->w=w;rain->h=h;rain->canvas_width=cw;rain->canvas_height=ch;rain->maximized=maximized;
            g_taskbar_popup=popup;g_taskbar_group_id=group;g_taskbar_anchor=anchor;
            g_audio_mixer.value=saved_volume;
            if(request==6){g_audio_mixer=saved_mixer;g_audio_hotkeys=saved_keys;g_damage=saved_damage;}
            for(int i=0;i<g_window_count;++i)g_window_order[i]=saved_order[i];
            if(request==6){
                // The coverage probe temporarily moved Rainbow above every
                // app; restore the actual scene before later pixel checks.
                wm_audio_changed();wm_dirty_add(0,fb->width-1,0,fb->height-1);g_needs_refresh=true;
            }
        }
    }else if(request==2){
        uint64_t bytes=(uint64_t)fb->pitch*g_taskbar_h;
        if(!g_gpu_test_taskbar_before)g_gpu_test_taskbar_before=(uint8_t*)kmalloc(bytes);
        if(!g_gpu_test_taskbar_before)hcf();
        memcpy(g_gpu_test_taskbar_before,g_master_backbuffer+(uint64_t)(fb->height-g_taskbar_h)*fb->pitch,bytes);
        g_gpu_test_scene_repair_accepted=true;
        for(unsigned i=0;i<g_gpu_test_scene_repairs.count;++i)
            if(!wm_repair_retained_foreground(g_gpu_test_scene_repairs.rects[i]))g_gpu_test_scene_repair_accepted=false;
    }else if(request==3){
        wm_compose_dirty(0,fb->width-1,fb->height-g_taskbar_h,fb->height-1,false);
    }else{
        wm_compose_retained_repairs(g_gpu_test_scene_repairs);
        g_gpu_test_scene_repair_accepted=true;
    }
    g_menu_open=menu_open;
    g_clock_valid=clock_valid;
    gpu_direct_end();
}
#ifdef GPU_RENDER_TEST
static uint64_t g_gpu_test_render_calls=0;
static uint64_t g_gpu_test_demo_render_calls=0;
static uint8_t g_gpu_test_render_fail=0;
static uint32_t g_gpu_test_scaled_sample[3]{};
static GpuUploadCache g_gpu_test_upload_cache{};
static uint8_t* g_gpu_test_upload=nullptr;
static uint64_t g_gpu_test_upload_capacity=0;
static uint64_t g_gpu_test_uploads=0,g_gpu_test_upload_reuses=0;
static bool gpu_test_render(const GpuScenePlan& plan) {
    ++g_gpu_test_render_calls;
    unsigned uploaded_source=UINT32_MAX;
    for(unsigned i=0;i<plan.count;++i){
        const auto& copy=plan.copies[i];const auto& c=plan.clients[copy.source];
        ++g_gpu_perf.rcs_draws;
        uint64_t bytes=(uint64_t)c.pitch*c.height;
        if(uploaded_source!=copy.source){
            if(!g_gpu_test_upload_cache.matches(c.pixels,bytes,c.revision,g_gpu_surface_epoch)){
                if(bytes>g_gpu_test_upload_capacity){
                    auto* pixels=(uint8_t*)kmalloc(bytes);if(!pixels)return false;
                    kfree(g_gpu_test_upload);g_gpu_test_upload=pixels;g_gpu_test_upload_capacity=bytes;
                }
                memcpy(g_gpu_test_upload,c.pixels,bytes);
                g_gpu_test_upload_cache.record(c.pixels,bytes,c.revision,g_gpu_surface_epoch);
                ++g_gpu_perf.uploads;g_gpu_perf.upload_bytes+=bytes;
                ++g_gpu_test_uploads;
            }else {++g_gpu_perf.upload_reuses;++g_gpu_test_upload_reuses;}
            uploaded_source=copy.source;
        }
        g_gpu_test_rcs_pixels+=(uint64_t)(copy.destination.x2-copy.destination.x1)*(copy.destination.y2-copy.destination.y1);
        if(c.width==480&&c.height==320)++g_gpu_test_demo_render_calls;
        GpuTextureDraw draw{{0x10000000,c.width,c.height,c.pitch},
            {0x2000000,(uint32_t)fb->width,(uint32_t)fb->height,(uint32_t)fb->pitch},
            {c.client.x1,c.client.y1,c.client.x2,c.client.y2},
            {copy.destination.x1,copy.destination.y1,copy.destination.x2,copy.destination.y2},c.premultiplied};
        uint32_t batch[1024];if(!gpu_build_texture_batch(batch,0x100000,draw))return false;
        bool blend=(batch[2432/4+1]&(1u<<31))!=0;
        if(blend){
            uint32_t state=batch[2432/4+1];
            if(((state>>26)&31)!=1||((state>>21)&31)!=0x13)return false;
        }
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
            auto* row=(const uint32_t*)(g_gpu_test_upload+(uint64_t)sy*c.pitch);
            auto* to=(uint32_t*)(g_master_backbuffer+(uint64_t)y*fb->pitch);
            for(int x=r.x1;x<r.x2;++x){
                unsigned sx=(u1+(x+.5f-left)*(u2-u1)/(right-left))*c.width;
                if(sx>=c.width)sx=c.width-1;
                to[x]=blend?window_source_over(row[sx],to[x]):row[sx];
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
    ++g_gpu_perf.present_calls;
    unsigned back=1-g_gpu_test_present_front;
    uint64_t bytes=(uint64_t)fb->pitch*fb->height;
    {
        GpuPerfTimer timer(g_gpu_perf.scanout_copy_ticks);
        memcpy(g_gpu_test_present_buffers[back],g_master_backbuffer,bytes);
    }
    if(g_gpu_test_present_fail)return false; // prepared frame did not latch
    OverlayTarget target{g_gpu_test_present_buffers[back],g_master_backbuffer,(int)fb->width,(int)fb->height,(int)fb->pitch};
    const OverlayFrame empty{{-999,-999,0,0},{},0};
    {
        GpuPerfTimer timer(g_gpu_perf.overlay_ticks);
        overlay_render_transition(target,empty,frame,sprite,true);gpu_mfence();
    }
    {
        // Mock display copy, not a simulated Intel vblank measurement.
        GpuPerfTimer timer(g_gpu_perf.flip_ticks);
        memcpy_vram_sse_headless(fb->address,g_gpu_test_present_buffers[back],bytes);gpu_mfence();
        g_gpu_test_present_front=back;
    }
    return true;
}
#endif
static bool gpu_test_compose(const GpuScenePlan& plan) {
    ++g_gpu_test_calls;
    ++g_gpu_perf.bcs_batches;
    for(unsigned i=0;i<plan.count;++i){
        const auto& copy=plan.copies[i];const auto& c=plan.clients[copy.source];auto r=copy.destination;
        g_gpu_test_bcs_pixels+=(uint64_t)(r.x2-r.x1)*(r.y2-r.y1);
        for(int y=r.y1;y<r.y2;++y){
            auto* to=(uint32_t*)(g_master_backbuffer+(uint64_t)y*fb->pitch)+r.x1;
            if(c.rainbow){
                for(int x=r.x1;x<r.x2;++x)
                    to[x-r.x1]=plan.rainbow_colors[((uint64_t)plan.scroll+copy.source_x+x-r.x1)%720];
            }else memcpy(to,(uint8_t*)c.pixels+(uint64_t)(copy.source_y+y-r.y1)*c.pitch+copy.source_x*4,(r.x2-r.x1)*4);
        }
        if(c.rainbow){
            ++g_gpu_test_rainbow_calls;
            // Ordered batches also draw pixels later covered by higher windows
            // and shadows. Keep the frame-boundary sample in exposed content.
            VisibleRegion exposed{};exposed.count=1;exposed.rects[0]=r;
            bool above=false;
            for(int s=0;s<g_window_count&&exposed.count;++s){
                auto* win=g_window_list[g_window_order[s]];
                if(win==g_win_rain){above=true;continue;}
                if(!above||!win||!win->visible||win->minimized)continue;
                WinRect bounds;wm_get_win_rect(win,&bounds);
                VisibleRect cover{bounds.x1,bounds.y1,bounds.x2,bounds.y2};
                if(win->shadow_enabled&&!win->maximized)cover=window_shadow_bounds(cover);
                if(!visibility_subtract(&exposed,cover)){exposed.count=0;break;}
            }
            if(exposed.count){
                auto sample=exposed.rects[0];
                g_gpu_test_sample[0]=(sample.x1+sample.x2)/2;g_gpu_test_sample[1]=(sample.y1+sample.y2)/2;
                g_gpu_test_sample[2]=plan.rainbow_colors[((uint64_t)plan.scroll+copy.source_x+g_gpu_test_sample[0]-r.x1)%720];
            }
        }
        if(g_gpu_test_fail){
            // Simulate late-DMA quarantine after a partially executed batch.
            g_master_backbuffer=g_gpu_test_recovery;
            g_gpu_compose_hook=nullptr; // Match gpu_scene_abort's submission shutdown.
            g_gpu_present_hook=nullptr;
            g_gpu_quarantine_sources=true;
            g_gpu_test_recovery=nullptr;
            return false;
        }
    }
    return true;
}
static void gpu_test_install() {
    g_gpu_test_tray_left=wm_tray_damage_left();
    g_gpu_test_tray_enabled=GPU_EXACT_REPAIRS;
    // Isolate the existing retained/alpha fixtures; shadow coverage below
    // explicitly enables decorations on these same windows.
    for(int i=0;i<g_window_count;++i)g_window_list[i]->shadow_enabled=false;
    g_gpu_test_recovery=(uint8_t*)kmalloc((uint64_t)fb->pitch*fb->height);
    if(!g_gpu_test_recovery)hcf();
    memset(g_gpu_test_recovery,0,(uint64_t)fb->pitch*fb->height);
    g_gpu_compose_hook=gpu_test_compose;
#ifdef GPU_RENDER_TEST
    auto* fixture=wm_create_window("GPU scaled fixture",1020,180,64,48,false,false,WIN_TYPE_USER,0);
    if(!fixture)hcf();
    fixture->shadow_enabled=false;
    for(unsigned y=0;y<48;++y)for(unsigned x=0;x<64;++x)
        fixture->buffer[y*64+x]=0xFF000000u|(x*3u<<16)|(y*5u<<8)|((x+y)*2u);
    fixture->w=128;fixture->h=96;
    auto* neighbor=wm_create_window("GPU native fixture",1148,180,128,96,false,false,WIN_TYPE_USER,0);
    if(!neighbor)hcf();
    neighbor->shadow_enabled=false;
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
