#include "gpu_opaque_coverage.h"
bool GpuOpaqueCoverage::update(const uint32_t* data,unsigned w,unsigned h,unsigned stride,uint64_t version){
    if(version&&data==pixels&&version==revision&&w==width&&h==height&&stride==pitch)return false;
    pixels=data;revision=version;width=w;height=h;pitch=stride;count=0;
    if(!data||!version||!w||!h||w>16384||h>16384||stride<(uint64_t)w*4||
       stride>262144||(stride&3)||(uint64_t)w*h>4*1024*1024)return false;
    for(unsigned band=0;band<capacity;++band){
        unsigned top=(uint64_t)h*band/capacity,bottom=(uint64_t)h*(band+1)/capacity;
        if(top==bottom)continue;
        unsigned start=0,best_left=0,best_right=0;
        for(unsigned x=0;x<=w;++x){
            bool opaque=x<w;
            if(opaque)for(unsigned y=top;y<bottom;++y){
                auto* row=(const uint32_t*)((const uint8_t*)data+(uint64_t)y*stride);
                if((row[x]>>24)!=255){opaque=false;break;}
            }
            if(!opaque){
                if(x-start>best_right-best_left){best_left=start;best_right=x;}
                start=x+1;
            }
        }
        if(best_right<=best_left)continue;
        VisibleRect r{(int)best_left,(int)top,(int)best_right,(int)bottom};
        if(count&&source[count-1].x1==r.x1&&source[count-1].x2==r.x2&&source[count-1].y2==r.y1)
            source[count-1].y2=r.y2;
        else source[count++]=r;
    }
    return true;
}
unsigned GpuOpaqueCoverage::mapped(VisibleRect client,int grip,VisibleRect* out) const{
    int64_t w=(int64_t)client.x2-client.x1,h=(int64_t)client.y2-client.y1;
    if(!out||!revision||!width||!height||w<=0||h<=0||w>16384||h>16384||grip<0)return 0;
    unsigned n=0;
    for(unsigned i=0;i<count;++i){
        auto r=source[i];
        // Shrink by a source texel on every edge, then round inward in screen
        // space. Nearest-sampler precision and CPU scaling boundaries can only
        // reach texels already proven alpha=255. Never retain the CPU grip.
        if(r.x2-r.x1<=2||r.y2-r.y1<=2)continue;
        VisibleRect mapped{
            (int)(client.x1+(((int64_t)r.x1+1)*w+width-1)/width),
            (int)(client.y1+(((int64_t)r.y1+1)*h+height-1)/height),
            (int)(client.x1+((int64_t)r.x2-1)*w/width),
            (int)(client.y1+((int64_t)r.y2-1)*h/height)};
        if(mapped.y2>client.y2-grip)mapped.y2=client.y2-grip;
        if(visibility_valid(mapped))out[n++]=mapped;
    }
    return n;
}
