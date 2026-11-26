#ifndef WYZAR_POPUP_CACHE_H
#define WYZAR_POPUP_CACHE_H
// Shared opaque shell popup contract: paint a complete WB image only when its
// content changes, then copy exactly the caller's repair clip. The fallback
// painter must obey the same clip when scratch storage is unavailable.
template<class Paint> static void wm_render_cached_popup(VisibleRect panel,uint64_t key,unsigned kind,
        int min_y,int max_y,uint64_t& renders,uint64_t& reuses,Paint paint){
    auto clip=visibility_intersect(panel,{g_ui_clip_left,g_ui_clip_top,g_ui_clip_right,g_ui_clip_bottom});
    clip=visibility_intersect(clip,{0,min_y,(int)fb->width,max_y+1});
    clip=visibility_intersect(clip,{0,0,(int)fb->width,(int)fb->height});
    if(!visibility_valid(clip))return;
    key=(key^kind)*1099511628211ULL;
    auto& cache=g_menu_pixel_cache;
    if(!cache.pixels||cache.capacity<(uint64_t)fb->pitch*fb->height){
        paint(clip.y1,clip.y2-1);++renders;return;
    }
    if(cache.popup_key!=key){
        cache.key=0;cache.popup_key=0;
        auto* destination=g_master_backbuffer;
        int left=g_ui_clip_left,right=g_ui_clip_right,top=g_ui_clip_top,bottom=g_ui_clip_bottom;
        g_master_backbuffer=cache.pixels;
        g_ui_clip_left=panel.x1;g_ui_clip_right=panel.x2;
        g_ui_clip_top=panel.y1;g_ui_clip_bottom=panel.y2;
        paint(panel.y1,panel.y2-1);
        g_master_backbuffer=destination;
        g_ui_clip_left=left;g_ui_clip_right=right;g_ui_clip_top=top;g_ui_clip_bottom=bottom;
        cache.popup_key=key;++renders;
    }else ++reuses;
    for(int y=clip.y1;y<clip.y2;++y){
        uint64_t offset=(uint64_t)y*fb->pitch+clip.x1*4;
        memcpy_vram_sse_headless(g_master_backbuffer+offset,cache.pixels+offset,(clip.x2-clip.x1)*4);
    }
}
#endif
