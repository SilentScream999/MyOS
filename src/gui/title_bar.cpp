#include "title_bar.h"
#include "window_decorations.h"
#include "window_alpha.h"

bool title_bar_equal(const TitleBarKey& a,const TitleBarKey& b) {
    if(a.width!=b.width||a.height!=b.height||a.font!=b.font||a.icon.pixels!=b.icon.pixels||
       a.icon.width!=b.icon.width||a.icon.height!=b.icon.height||a.resource_epoch!=b.resource_epoch||
       a.focused!=b.focused||a.closable!=b.closable||a.maximized!=b.maximized||a.hover!=b.hover)return false;
    for(unsigned i=0;i<sizeof(a.title);++i){if(a.title[i]!=b.title[i])return false;if(!a.title[i])break;}
    return true;
}
void title_bar_blit(const uint32_t* pixels,int width,int height,TitleBarTarget target,int x,int y,VisibleRect clip) {
    if(!pixels||!target.pixels||width<=0||height<=0||target.stride<target.width)return;
    auto r=visibility_intersect({x,y,x+width,y+height},visibility_intersect(clip,{0,0,target.width,target.height}));
    for(int py=r.y1;py<r.y2;++py){
        auto* dst=target.pixels+(uint64_t)py*target.stride+r.x1;
        auto* src=pixels+(uint64_t)(py-y)*width+r.x1-x;
        for(int px=r.x1;px<r.x2;++px)*dst++=*src++;
    }
}
void title_bar_render(const TitleBarKey& key,TitleBarTarget target,int x,int y,VisibleRect clip) {
    if(!target.pixels||!key.font||key.width<=0||key.height<=0||target.stride<target.width)return;
    auto r=visibility_intersect({x,y,x+key.width,y+key.height},visibility_intersect(clip,{0,0,target.width,target.height}));
    if(!visibility_valid(r))return;
    uint32_t bar=key.focused?0xff352a49:0xff25232e,line=key.focused?0xff77608f:0xff3d374a;
    for(int py=r.y1;py<r.y2;++py){
        auto* row=target.pixels+(uint64_t)py*target.stride;
        for(int px=r.x1;px<r.x2;++px)row[px]=py==y?line:bar;
    }
    int text_x=x+10;
    if(key.icon.pixels&&key.icon.width&&key.icon.height){
        int ix=x+6,iy=y+(key.height-(int)key.icon.height)/2;
        auto icon_clip=visibility_intersect(r,{x,y,x+key.width-key.height,y+key.height});
        auto ir=visibility_intersect(icon_clip,{ix,iy,ix+(int)key.icon.width,iy+(int)key.icon.height});
        for(int py=ir.y1;py<ir.y2;++py)for(int px=ir.x1;px<ir.x2;++px){
            auto& dst=target.pixels[(uint64_t)py*target.stride+px];
            dst=window_source_over(key.icon.pixels[(uint64_t)(py-iy)*key.icon.width+px-ix],dst);
        }
        text_x=x+12+key.icon.width;
    }
    int text_width=window_control_rect(x,y,key.width,key.height,WINDOW_MINIMIZE).x-text_x-10;
    if(text_width>0){
        const char* text=key.title;char shortened[68];
        if(ui_text_width(key.font,text)>text_width){
            int used=0,dots=ui_text_width(key.font,"...");unsigned n=0;
            while(text[n]&&n<sizeof(shortened)-4){
                unsigned advance=ui_font_glyph(key.font,(uint8_t)text[n])->advance;
                if(used+(int)advance+dots>text_width)break;
                used+=advance;shortened[n]=text[n];++n;
            }
            if(dots<=text_width){shortened[n++]='.';shortened[n++]='.';shortened[n++]='.';}
            shortened[n]=0;text=shortened;
        }
        int right=r.x2;if(text_x+text_width<right)right=text_x+text_width;
        ui_text_draw(target.pixels,target.width,target.height,target.stride,key.font,text_x,
            y+(key.height-key.font->height)/2,text,key.focused?0xffffffff:0xff9094a0,r.x1,r.y1,right,r.y2);
    }
    for(int control=0;control<3;++control){
        auto button=window_control_rect(x,y,key.width,key.height,control);
        auto br=visibility_intersect(r,{button.x,button.y,button.x+button.w,button.y+button.h});
        bool disabled=control==WINDOW_CLOSE&&!key.closable,hover=!disabled&&key.hover==control;
        uint32_t fill=hover?(control==WINDOW_CLOSE?0xff9a405d:0xff665078):bar;
        uint32_t ink=disabled?0xff625c6d:hover?0xffffffff:key.focused?0xffe9d7b0:0xffada2bd;
        for(int py=br.y1;py<br.y2;++py)for(int px=br.x1;px<br.x2;++px){
            bool corner=(px==button.x||px==button.x+button.w-1)&&(py==button.y||py==button.y+button.h-1);
            target.pixels[(uint64_t)py*target.stride+px]=window_control_ink(control,key.maximized,
                px-(button.x+button.w/2),py-(button.y+button.h/2))?ink:corner?bar:fill;
        }
    }
}
