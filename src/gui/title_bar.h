#ifndef WYZAR_TITLE_BAR_H
#define WYZAR_TITLE_BAR_H
#include "ui_font.h"
#include "image.h"
#include "visibility.h"

struct TitleBarKey {
    int width,height;
    const UiFont* font;
    Image icon;
    uint64_t resource_epoch;
    char title[64];
    bool focused,closable,maximized;
    int hover; // WINDOW_* control or WINDOW_NO_CONTROL
};
struct TitleBarTarget { uint32_t* pixels;int width,height,stride; };
struct TitleBarCache { TitleBarKey key;uint32_t* pixels;uint64_t capacity;bool valid; };
bool title_bar_equal(const TitleBarKey& a,const TitleBarKey& b);
void title_bar_render(const TitleBarKey& key,TitleBarTarget target,int x,int y,VisibleRect clip);
void title_bar_blit(const uint32_t* pixels,int width,int height,TitleBarTarget target,int x,int y,VisibleRect clip);
#endif
