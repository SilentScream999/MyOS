#ifndef WYZAR_TRUETYPE_H
#define WYZAR_TRUETYPE_H
// Bounded, allocation-free TrueType outline decoder. No bytecode is executed.
// The caller owns scratch/output storage; file bytes need only survive baking.
#include "ui_font.h"
#include <stddef.h>

namespace ttf {
static constexpr size_t max_file = 16 * 1024 * 1024;
struct Bytes {
    const uint8_t* p = nullptr; size_t n = 0;
    bool has(size_t off, size_t len) const { return off <= n && len <= n - off; }
    uint16_t u16(size_t o) const { return (uint16_t(p[o]) << 8) | p[o+1]; }
    int16_t s16(size_t o) const { return (int16_t)u16(o); }
    uint32_t u32(size_t o) const { return (uint32_t(u16(o)) << 16) | u16(o+2); }
};
struct Face {
    Bytes head, hhea, hmtx, maxp, loca, glyf, cmap;
    unsigned units = 0, count = 0, metrics = 0, locsize = 0;
    bool init(const uint8_t* data, size_t size) {
        Bytes b{data,size};
        if (!data || size > max_file || !b.has(0,12) ||
            (b.u32(0) != 0x10000 && b.u32(0) != 0x74727565)) return false;
        unsigned tables = b.u16(4);
        if (!tables || tables > 256 || !b.has(12,tables*16)) return false;
        for (unsigned i=0; i<tables; ++i) {
            size_t o=12+i*16; uint32_t start=b.u32(o+8), len=b.u32(o+12);
            if (!b.has(start,len)) return false;
            Bytes* dst=nullptr;
            switch (b.u32(o)) {
                case 0x68656164: dst=&head; break; case 0x68686561: dst=&hhea; break;
                case 0x686d7478: dst=&hmtx; break; case 0x6d617870: dst=&maxp; break;
                case 0x6c6f6361: dst=&loca; break; case 0x676c7966: dst=&glyf; break;
                case 0x636d6170: dst=&cmap; break;
            }
            if (dst) { if (dst->p) return false; *dst={data+start,len}; }
        }
        if (!head.has(0,54) || !hhea.has(0,36) || !maxp.has(0,6) || !cmap.has(0,4)) return false;
        units=head.u16(18); count=maxp.u16(4); metrics=hhea.u16(34);
        if (units<16 || units>16384 || !count || !metrics || metrics>count || head.u16(50)>1) return false;
        locsize=head.u16(50)?4:2;
        if (!loca.has(0,(count+1)*locsize) || !hmtx.has(0,metrics*4+(count-metrics)*2) || !glyf.p) return false;
        uint32_t previous=0;
        for (unsigned i=0; i<=count; ++i) {
            uint32_t next=location(i);
            if (next<previous || next>glyf.n) return false;
            previous=next;
        }
        Bytes chosen{};
        unsigned records=cmap.u16(2);
        if (records>256 || !cmap.has(4,records*8)) return false;
        for (unsigned i=0; i<records; ++i) {
            size_t o=4+i*8, sub=cmap.u32(o+4);
            unsigned platform=cmap.u16(o), encoding=cmap.u16(o+2);
            if (!(platform==0 || (platform==3 && (encoding==1 || encoding==10)))) continue;
            if (!cmap.has(sub,4)) return false;
            unsigned format=cmap.u16(sub);
            if (format!=4 && format!=12) continue;
            if (format==12 && !cmap.has(sub,16)) return false;
            size_t len=format==4?cmap.u16(sub+2):cmap.u32(sub+4);
            if (!cmap.has(sub,len)) return false;
            Bytes candidate{cmap.p+sub,len};
            if (format==4) {
                if (!candidate.has(0,16)) return false;
                unsigned segments=candidate.u16(6)/2;
                if (!segments || candidate.u16(6)%2 || !candidate.has(16,segments*8)) return false;
            } else if (candidate.u32(12)>65536 || !candidate.has(16,(uint64_t)candidate.u32(12)*12)) return false;
            if (!chosen.p || format==12) chosen=candidate;
        }
        cmap=chosen; return cmap.p;
    }
    uint32_t location(unsigned id) const { return locsize==4?loca.u32(id*4):loca.u16(id*2)*2; }
    bool glyph(unsigned code, unsigned& id) const {
        id=0;
        if (cmap.u16(0)==12) {
            for (unsigned i=0; i<cmap.u32(12); ++i) {
                size_t o=16+i*12; uint32_t a=cmap.u32(o), b=cmap.u32(o+4);
                if (a>b) return false;
                if (code>=a && code<=b) { uint64_t v=(uint64_t)cmap.u32(o+8)+code-a; if(v>=count) return false; id=v; break; }
            }
        } else {
            unsigned n=cmap.u16(6)/2;
            for (unsigned i=0; i<n; ++i) {
                unsigned end=cmap.u16(14+i*2), start=cmap.u16(16+n*2+i*2);
                if (start>end) return false;
                if (code<start || code>end) continue;
                unsigned delta=cmap.u16(16+n*4+i*2), range=cmap.u16(16+n*6+i*2);
                if (!range) id=(code+delta)&65535;
                else {
                    size_t o=16+n*6+i*2+range+(code-start)*2;
                    if (!cmap.has(o,2)) return false;
                    id=cmap.u16(o); if(id) id=(id+delta)&65535;
                }
                break;
            }
        }
        return id<count;
    }
};
static constexpr unsigned max_points=4096, max_contours=256, max_edges=32768;
struct Point { int32_t x,y; bool on; };
struct Edge { int32_t x0,y0,x1,y1; };
struct Scratch {
    Point points[max_points]; uint16_t ends[max_contours]; uint8_t flags[max_points];
    Edge edges[max_edges]; int32_t crossings[max_edges]; int8_t directions[max_edges];
    unsigned np=0,nc=0,ne=0,components=0;
    bool outline(const Face& f, unsigned id, unsigned depth=0) {
        if (id>=f.count || depth>8 || ++components>128) return false;
        uint32_t a=f.location(id), b=f.location(id+1);
        if (a==b) return true;
        Bytes g{f.glyf.p+a,b-a}; if (!g.has(0,10)) return false;
        int contours=g.s16(0); size_t pos=10;
        if (contours>=0) {
            if ((unsigned)contours>max_contours-nc || !g.has(pos,contours*2+2)) return false;
            unsigned base=np, first=0;
            for (int i=0;i<contours;++i) {
                unsigned end=g.u16(pos); pos+=2;
                if (end<first || end>=max_points-base) return false;
                ends[nc++]=base+end; first=end+1;
            }
            unsigned total=first; unsigned instructions=g.u16(pos); pos+=2;
            if (!g.has(pos,instructions)) return false; pos+=instructions;
            for (unsigned i=0;i<total;) {
                if (!g.has(pos,1)) return false;
                uint8_t flag=g.p[pos++]; unsigned repeat=0;
                if (flag&8) { if(!g.has(pos,1))return false; repeat=g.p[pos++]; }
                if (repeat>=total-i) return false;
                do { flags[base+i++]=flag; } while (repeat--);
            }
            for (unsigned axis=0;axis<2;++axis) {
                int32_t value=0;
                for (unsigned i=0;i<total;++i) {
                    unsigned flag=flags[base+i], small=axis?4:2, same=axis?32:16; int delta=0;
                    if (flag&small) { if(!g.has(pos,1))return false; delta=g.p[pos++]; if(!(flag&same))delta=-delta; }
                    else if (!(flag&same)) { if(!g.has(pos,2))return false; delta=g.s16(pos);pos+=2; }
                    value+=delta; if(value < -131072 || value>131072)return false;
                    if(axis)points[base+i].y=value;else points[base+i].x=value;
                    points[base+i].on=flag&1;
                }
            }
            np+=total; return true;
        }
        if(contours!=-1)return false;
        unsigned flag, parent_base=np; bool instructions=false;
        do {
            if(!g.has(pos,4))return false;
            flag=g.u16(pos); unsigned child=g.u16(pos+2); pos+=4;
            instructions |= (flag&256)!=0;
            if((flag&0x1800)==0x1800)return false;
            bool xy=flag&2; unsigned arg1,arg2; int dx=0,dy=0;
            if(flag&1) { if(!g.has(pos,4))return false; arg1=g.u16(pos);arg2=g.u16(pos+2);dx=g.s16(pos);dy=g.s16(pos+2);pos+=4; }
            else { if(!g.has(pos,2))return false;arg1=g.p[pos];arg2=g.p[pos+1];dx=(int8_t)arg1;dy=(int8_t)arg2;pos+=2; }
            int xx=16384,yy=16384,xyv=0,yx=0;
            unsigned transform=flag&(8|64|128);
            if(transform && (transform&(transform-1)))return false;
            unsigned values=transform==128?4:transform==64?2:transform==8?1:0;
            if(!g.has(pos,values*2))return false;
            if(values==1)xx=yy=g.s16(pos);
            if(values==2){xx=g.s16(pos);yy=g.s16(pos+2);}
            if(values==4){xx=g.s16(pos);yx=g.s16(pos+2);xyv=g.s16(pos+4);yy=g.s16(pos+6);}
            pos+=values*2;
            unsigned base=np;
            if(!outline(f,child,depth+1))return false;
            for(unsigned i=base;i<np;++i){
                Point p=points[i];
                points[i].x=((int64_t)p.x*xx+(int64_t)p.y*xyv)/16384;
                points[i].y=((int64_t)p.x*yx+(int64_t)p.y*yy)/16384;
            }
            if(!xy){
                if(arg1>=base-parent_base || arg2>=np-base)return false;
                dx=points[parent_base+arg1].x-points[base+arg2].x;dy=points[parent_base+arg1].y-points[base+arg2].y;
            } else if(flag&0x800){ int x=dx;dx=((int64_t)x*xx+(int64_t)dy*xyv)/16384;dy=((int64_t)x*yx+(int64_t)dy*yy)/16384; }
            for(unsigned i=base;i<np;++i){
                points[i].x+=dx;points[i].y+=dy;
                if(points[i].x < -131072 || points[i].x>131072 || points[i].y < -131072 || points[i].y>131072)return false;
            }
        }while(flag&32);
        if(instructions){if(!g.has(pos,2)||!g.has(pos+2,g.u16(pos)))return false;}
        return true;
    }
    bool edge(Point a,Point b){
        if(a.y==b.y)return true;
        if(ne==max_edges)return false;
        edges[ne++]={a.x,a.y,b.x,b.y};return true;
    }
    static Point mid(Point a,Point b){return {(a.x+b.x)/2,(a.y+b.y)/2,true};}
    bool curve(Point a,Point c,Point b){
        Point prev=a;
        for(int t=1;t<=8;++t){ int u=8-t;Point p{(a.x*u*u+2*c.x*u*t+b.x*t*t)/64,(a.y*u*u+2*c.y*u*t+b.y*t*t)/64,true};if(!edge(prev,p))return false;prev=p; }
        return true;
    }
    bool flatten(unsigned pixels,unsigned units){
        ne=0;
        for(unsigned i=0;i<np;++i){
            points[i].x=(int64_t)points[i].x*pixels*256/units;points[i].y=-(int64_t)points[i].y*pixels*256/units;
            if(points[i].x < -32768 || points[i].x>32768 || points[i].y < -32768 || points[i].y>32768)return false;
        }
        unsigned first=0;
        for(unsigned c=0;c<nc;++c){
            unsigned end=ends[c], n=end-first+1;
            Point last=points[end], start=points[first];
            unsigned i=0;
            if(start.on)i=1;else start=last.on?last:mid(last,start);
            Point current=start;
            while(i<n){
                Point p=points[first+i];
                if(p.on){if(!edge(current,p))return false;current=p;++i;}
                else {Point next=i+1<n?points[first+i+1]:start;Point dest=next.on?next:mid(p,next);if(!curve(current,p,dest))return false;current=dest;i+=next.on?2:1;}
            }
            if(!edge(current,start))return false;first=end+1;
        }
        return true;
    }
    bool raster(uint8_t* out,int left,int top,unsigned width,unsigned height){
        unsigned work=0;
        for(unsigned y=0;y<height;++y)for(int sy=0;sy<4;++sy){
            int scan=(top+(int)y)*256+sy*64+32;unsigned n=0;
            for(unsigned e=0;e<ne;++e){Edge a=edges[e];int low=a.y0<a.y1?a.y0:a.y1,high=a.y0>a.y1?a.y0:a.y1;
                if(scan<low||scan>=high)continue;
                int x=a.x0+(int64_t)(scan-a.y0)*(a.x1-a.x0)/(a.y1-a.y0);
                unsigned j=n++;while(j&&crossings[j-1]>x){if(++work>4000000)return false;crossings[j]=crossings[j-1];directions[j]=directions[j-1];--j;}
                crossings[j]=x;directions[j]=a.y1>a.y0?1:-1;
            }
            unsigned j=0;int winding=0;
            for(unsigned x=0;x<width;++x)for(int sx=0;sx<4;++sx){
                int sample=(left+(int)x)*256+sx*64+32;
                while(j<n&&crossings[j]<=sample)winding+=directions[j++];
                if(winding)++out[y*width+x];
            }
        }
        for(unsigned i=0;i<width*height;++i)out[i]=(out[i]*255+8)/16;
        return true;
    }
};
// Fixed caps keep kernel memory and decoding work bounded for untrusted files.
struct Atlas {
    UiFont fonts[8]; UiGlyph glyphs[8][95]; uint8_t coverage[512*1024];
};
static bool bake(const uint8_t* bytes,size_t length,Atlas& out,Scratch& s){
    Face f{};if(!f.init(bytes,length))return false;
    const unsigned sizes[]={12,14,16,18,21,24,28,32};size_t used=0;
    int asc=f.hhea.s16(4),desc=f.hhea.s16(6),gap=f.hhea.s16(8);
    if(asc<=0||desc>0||gap<0)return false;
    for(unsigned z=0;z<8;++z){
        unsigned px=sizes[z],ascent=(asc*px+f.units-1)/f.units;
        unsigned height=((asc-desc+gap)*px+f.units-1)/f.units;
        if(height<px)height=px;
        if(height>96||ascent>height)return false;
        out.fonts[z]={(uint8_t)px,(uint8_t)height,(uint8_t)ascent,out.glyphs[z],out.coverage};
        for(unsigned c=32;c<=126;++c){
            unsigned id;if(!f.glyph(c,id))return false;
            if(!id && c!=' ' && !f.glyph('?',id))return false;
            s.np=s.nc=s.ne=s.components=0;
            if(!s.outline(f,id)||!s.flatten(px,f.units))return false;
            int left=0,top=0,right=0,bottom=0;
            if(s.np){
                int xmin=s.points[0].x,xmax=xmin,ymin=s.points[0].y,ymax=ymin;
                for(unsigned i=1;i<s.np;++i){Point p=s.points[i];if(p.x<xmin)xmin=p.x;if(p.x>xmax)xmax=p.x;if(p.y<ymin)ymin=p.y;if(p.y>ymax)ymax=p.y;}
                left=xmin>=0?xmin/256:-((-xmin+255)/256);top=ymin>=0?ymin/256:-((-ymin+255)/256);
                right=xmax>=0?(xmax+255)/256:-((-xmax)/256);bottom=ymax>=0?(ymax+255)/256:-((-ymax)/256);
            }
            unsigned w=right-left,h=bottom-top;
            unsigned metric=id<f.metrics?id:f.metrics-1;
            unsigned advance=(f.hmtx.u16(metric*4)*px+f.units/2)/f.units;
            if(!advance)advance=1;
            if(w>96||h>96||left < -127||left>127||top < -127||top>127||advance>128||used+w*h>sizeof(out.coverage))return false;
            out.glyphs[z][c-32]={(uint32_t)used,(uint8_t)w,(uint8_t)h,(int8_t)left,(int8_t)-top,(uint8_t)advance};
            for(unsigned i=0;i<w*h;++i)out.coverage[used+i]=0;
            if(!s.raster(out.coverage+used,left,top,w,h))return false;used+=w*h;
        }
    }
    return true;
}
}
#endif
