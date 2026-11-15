#ifndef WYZAR_NATIVE_TEXT_H
#define WYZAR_NATIVE_TEXT_H
#include "native_ui.h"

// Shared insertion caret. Cache the covered column during a normal repaint so
// idle blinking can restore text pixels without redrawing the app or its field.
struct NativeTextCursor {
    unsigned epoch=0;
    bool visible=true;
private:
    uint32_t background[64],ink=0;
    uint32_t* pixels=nullptr;
    int width=0,height=0,left=0,top=0,rows=0;
    bool cached=false;
public:
    void reset(unsigned frame){epoch=frame;visible=true;}
    bool tick(unsigned frame,bool active=true){
        bool next=active&&((frame-epoch)/30%2==0);
        bool changed=next!=visible;visible=next;return changed;
    }
    void draw(NativeSurface& surface,int x,int y,int caret_height,uint32_t color=0xffe9bd70){
        pixels=surface.pixels;width=surface.w;height=surface.h;left=x;top=y<0?0:y;
        int bottom=y+caret_height;if(bottom>height)bottom=height;
        rows=bottom-top;ink=color;
        cached=pixels&&x>=0&&x<width&&rows>0&&rows<=64;
        if(cached)for(int r=0;r<rows;++r)background[r]=pixels[(top+r)*width+left];
        if(visible)surface.rect(x,y,1,caret_height,color);
    }
    // Run before the owner's normal repaint. A pending repaint invalidates the
    // old column (including on resize/focus changes); draw captures it again.
    // Return true only when a changed caret needs a normal repaint as fallback.
    bool animate(NativeSurface& surface,uint64_t window,unsigned frame,bool active,bool repaint_pending){
        bool changed=tick(frame,active);
        if(repaint_pending){cached=false;return false;}
        if(!changed)return false;
        if(!cached||surface.pixels!=pixels||surface.w!=width||surface.h!=height)return active;
        for(int r=0;r<rows;++r)pixels[(top+r)*width+left]=visible?ink:background[r];
        native_call(204,window,left,top,1,rows);
        if(!active)cached=false;
        return false;
    }
};

// ASCII single-line input shared by native app fields. The owner's replace
// flag selects all text on focus; arrows collapse that selection to either end.
struct NativeTextInput {
    NativeTextCursor cursor;
    unsigned position=0,first=0;
    void sync(const char* text,bool replace){unsigned n=native_length(text);if(replace||position>n)position=n;}
    bool insert(char* text,unsigned capacity,const char* value,unsigned count,bool& replace,unsigned frame){
        sync(text,replace);unsigned n=native_length(text),at=replace?0:position;
        unsigned kept=replace?0:n;
        if(kept>=capacity||count>=capacity-kept)return false;
        if(replace){n=0;text[0]=0;}
        for(unsigned i=n+1;i>at;--i)text[i-1+count]=text[i-1];
        for(unsigned i=0;i<count;++i)text[at+i]=value[i];
        position=at+count;replace=false;cursor.reset(frame);return true;
    }
    bool key(char* text,unsigned capacity,uint32_t key,bool& replace,unsigned frame){
        sync(text,replace);unsigned n=native_length(text);
        if(key==0x1044){if(replace)position=0;else if(position)--position;}
        else if(key==0x1043){if(replace)position=n;else if(position<n)++position;}
        else if(key==0x1100)position=0;
        else if(key==0x1101)position=n;
        else if(key==8||key==127||key==0x1102){
            if(replace){text[0]=0;position=0;}
            else {unsigned at=position;if(key!=0x1102){if(!at)return true;--at;--position;}
                if(at<n)for(unsigned i=at;i<n;++i)text[i]=text[i+1];}
        }else if(key>=32&&key<=126){char c=(char)key;return insert(text,capacity,&c,1,replace,frame);}
        else return false;
        replace=false;cursor.reset(frame);return true;
    }
    void draw(NativeSurface& surface,int x,int y,int right,const char* text,bool replace,int size=14,uint32_t ink=0xffeeebf8){
        sync(text,replace);const UiFont* font=ui_font_for_pixels(size);
        if(first>position)first=position;
        int width=0;for(unsigned i=first;i<position;++i)width+=ui_font_glyph(font,text[i])->advance;
        while(first<position&&width>=right-x-1)width-=ui_font_glyph(font,text[first++])->advance;
        surface.text(x,y,text+first,ink,right,size);
        if(x+width<right)cursor.draw(surface,x+width,y,font->height);
    }
};
#endif
