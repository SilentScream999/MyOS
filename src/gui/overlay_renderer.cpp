#include "overlay_renderer.h"
#include "../core/string.h"

// Thin vertical outlines are usually just two pixels wide. Copy them as whole
// pixels rather than issuing eight separate byte stores to the framebuffer.
static inline void overlay_copy(uint32_t* destination,const uint32_t* source,int bytes) {
    auto* out=(volatile uint32_t*)destination;
    while(bytes&&((uintptr_t)out&15)){*out++=*source++;bytes-=4;}
    int bulk=bytes&~15;
    if(bulk){memcpy_vram_sse_headless((void*)out,source,bulk);out+=bulk/4;source+=bulk/4;bytes-=bulk;}
    while(bytes){*out++=*source++;bytes-=4;}
}

void overlay_render_span(const OverlayTarget& target,int left,int right,int y,const OverlayFrame& frame,const uint32_t* sprite) {
    if(left<0)left=0;if(right>target.width)right=target.width;
    if(left>=right||y<0||y>=target.height)return;
    auto* source=(const uint32_t*)(target.source+(uint64_t)y*target.pitch);
    auto* destination=(uint32_t*)(target.destination+(uint64_t)y*target.pitch);
    overlay_present_row(destination,source,left,right,y,frame,
        [sprite](const OverlayCursor&,int x,int cy){return sprite[(cy+CURSOR_SPRITE_ORIGIN)*CURSOR_SPRITE_SIZE+x+CURSOR_SPRITE_ORIGIN];},
        [](uint32_t* d,const uint32_t* s,int bytes){overlay_copy(d,s,bytes);});
}
void overlay_render_transition(const OverlayTarget& target,const OverlayFrame& previous,const OverlayFrame& next,
                               const uint32_t* sprite,bool scene_changed) {
    overlay_transition(previous,next,target.height,[&](int left,int right,int y){
        overlay_render_span(target,left,right,y,next,sprite);
    },scene_changed);
}
