#ifndef WYZAR_OVERLAY_RENDERER_H
#define WYZAR_OVERLAY_RENDERER_H
#include "window_overlay.h"
struct OverlayTarget {
    uint8_t* destination;
    const uint8_t* source;
    int width,height,pitch;
};
// Caller holds the presentation lock, supplies the next cursor's cached sprite,
// and fences VRAM writes once after the complete presentation.
void overlay_render_span(const OverlayTarget&,int left,int right,int y,const OverlayFrame&,const uint32_t* sprite);
void overlay_render_transition(const OverlayTarget&,const OverlayFrame& previous,const OverlayFrame& next,
                               const uint32_t* sprite,bool scene_changed);
#endif
