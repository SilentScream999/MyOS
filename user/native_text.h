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

static unsigned native_word_class(unsigned char c){
    if(c<=32)return 0;
    if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c>=128)return 1;
    return 2;
}

static unsigned native_word_edge(const char* text,unsigned length,unsigned at,bool right){
    if(right){
        if(at<length){
            unsigned kind=native_word_class(text[at++]);
            while(at<length&&native_word_class(text[at])==kind)++at;
            while(at<length&&!native_word_class(text[at]))++at;
        }
    }else{
        while(at&&!native_word_class(text[at-1]))--at;
        if(at){
            unsigned kind=native_word_class(text[--at]);
            while(at&&native_word_class(text[at-1])==kind)--at;
        }
    }
    return at;
}

// ASCII single-line input. The owner's replace flag selects all text on focus.
struct NativeTextInput {
    NativeTextCursor cursor;
    unsigned position=0,anchor=0,first=0;
    unsigned low()const{return position<anchor?position:anchor;}
    unsigned high()const{return position>anchor?position:anchor;}
    void sync(unsigned length,bool replace){
        if(replace){position=length;anchor=0;}
        if(position>length)position=length;
        if(anchor>length)anchor=length;
    }
    // Use the same advances and scroll origin as the painted text.
    void click(const char* text,bool& replace,int offset,int size,unsigned frame,bool focused,uint32_t mods){
        unsigned length=native_length(text);
        sync(length,replace);
        if(!focused)first=0;
        unsigned at=first<length?first:length;
        const UiFont* font=ui_font_for_pixels(size);
        for(;at<length;++at){
            int advance=ui_font_glyph(font,text[at])->advance;
            if(offset<(advance+1)/2)break;
            offset-=advance;
        }
        position=at;
        if(!focused||!(mods&2))anchor=position;
        replace=false;cursor.reset(frame);
    }
    bool insert(char* text,unsigned capacity,const char* value,unsigned count,bool& replace,unsigned frame){
        unsigned n=native_length(text);sync(n,replace);
        unsigned at=low(),end=high(),kept=n-(end-at);
        if(kept>=capacity||count>=capacity-kept)return false;
        if(at+count>end){
            for(unsigned i=n+1;i>end;--i)text[at+count+(i-1-end)]=text[i-1];
        }else{
            for(unsigned i=end;i<=n;++i)text[at+count+(i-end)]=text[i];
        }
        for(unsigned i=0;i<count;++i)text[at+i]=value[i];
        position=anchor=at+count;replace=false;cursor.reset(frame);return true;
    }
    bool key(char* text,unsigned capacity,uint32_t key,bool& replace,unsigned frame,uint32_t mods=0){
        if(mods&4)return false;
        bool ctrl=mods&1,shift=mods&2;
        unsigned n=native_length(text);sync(n,replace);
        if(ctrl&&(key=='a'||key=='A')){anchor=0;position=n;}
        else if(key==0x1044||key==0x1043){
            bool right=key==0x1043;
            if(!shift&&!ctrl&&position!=anchor)position=right?high():low();
            else if(ctrl)position=native_word_edge(text,n,position,right);
            else if(right&&position<n)++position;
            else if(!right&&position)--position;
            if(!shift)anchor=position;
        }
        else if(key==0x1100||key==0x1101){position=key==0x1100?0:n;if(!shift)anchor=position;}
        else if(ctrl){
            bool cut=key=='x'||key=='X';
            if(!(key=='c'||key=='C'||cut)||position==anchor)return false;
            if(!native_clipboard_set(text+low(),high()-low())||!cut)return false;
            return insert(text,capacity,"",0,replace,frame);
        }
        else if(key==8||key==127||key==0x1102){
            replace=false;
            if(position==anchor){
                if(key==0x1102&&position<n)++anchor;
                else if(key!=0x1102&&position)--anchor;
                else return false;
            }
            return insert(text,capacity,"",0,replace,frame);
        }else if(key>=32&&key<=126){char c=(char)key;return insert(text,capacity,&c,1,replace,frame);}
        else return false;
        replace=false;cursor.reset(frame);return true;
    }
    void draw(NativeSurface& surface,int x,int y,int right,const char* text,bool replace,int size=14,
              uint32_t ink=0xffeeebf8,uint32_t background=0xff14121e,uint64_t window=0){
        sync(native_length(text),replace);const UiFont* font=ui_font_for_pixels(size);
        surface.rect(x,y,right-x,font->height,background);
        if(first>position)first=position;
        int width=0;for(unsigned i=first;i<position;++i)width+=ui_font_glyph(font,text[i])->advance;
        while(first<position&&width>=right-x-1)width-=ui_font_glyph(font,text[first++])->advance;
        unsigned start=low(),end=high();
        if(start!=end&&end>first){
            int left=x,after=x;
            for(unsigned i=first;i<end&&after<right;++i){
                after+=ui_font_glyph(font,text[i])->advance;
                if(i<start)left=after;
            }
            if(after>right)after=right;
            surface.rect(left,y,after-left,font->height,0xff665384);
        }
        ui_text_draw(surface.pixels,surface.w,surface.h,surface.w,font,x,y,text+first,ink,x,y,right,y+font->height);
        if(x+width<right)cursor.draw(surface,x+width,y,font->height);
        if(window)native_call(204,window,x,y,right-x,font->height);
    }
};
#endif
