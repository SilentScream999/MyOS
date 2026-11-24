#ifndef WYZAR_GPU_DEFERRED_ALPHA_H
#define WYZAR_GPU_DEFERRED_ALPHA_H
#include "gpu_compositor.h"
#ifndef GPU_ALPHA_REPAIR_BATCH
#define GPU_ALPHA_REPAIR_BATCH 0
#endif
// Only a topmost immutable alpha client can defer its final repair draw. Its
// background is already composed, and later animation writes must be disjoint.
struct GpuDeferredAlpha {
    static constexpr unsigned capacity=VisibleRegion::capacity;
    GpuSceneClient client{};
    VisibleRect rects[capacity]{};
    unsigned count=0;
    bool active=false,failed=false;
    void clear(){count=0;active=false;failed=false;}
    bool same_client(const GpuSceneClient& c) const{
        auto same=[](VisibleRect a,VisibleRect b){return a.x1==b.x1&&a.y1==b.y1&&a.x2==b.x2&&a.y2==b.y2;};
        return c.pixels==client.pixels&&c.revision==client.revision&&c.width==client.width&&
            c.height==client.height&&c.pitch==client.pitch&&same(c.client,client.client)&&
            same(c.outer,client.outer)&&c.resize_grip==client.resize_grip&&
            c.visible==client.visible&&c.rainbow==client.rainbow&&c.premultiplied==client.premultiplied;
    }
    void begin(const GpuSceneClient& c,bool topmost){
        clear();client=c;
        int64_t w=(int64_t)c.client.x2-c.client.x1,h=(int64_t)c.client.y2-c.client.y1;
        active=topmost&&c.visible&&c.premultiplied&&!c.rainbow&&c.revision&&c.pixels&&
            !((uintptr_t)c.pixels&63)&&c.width&&c.height&&c.width<=16384&&c.height<=16384&&
            w>0&&h>(c.resize_grip?12:0)&&w<=16384&&h<=16384&&c.pitch>=(uint64_t)c.width*4&&
            c.pitch<=262144&&!(c.pitch&3);
    }
    bool append(const GpuScenePlan& repair,const GpuScenePlan& animation){
        if(!active||failed||!repair.count)return false;
        auto reject=[&](){if(count)failed=true;return false;};
        if(repair.client_count!=1||!same_client(repair.clients[0])||
           repair.count>capacity-count||animation.count>GpuScenePlan::capacity)return reject();
        VisibleRect body=client.client;if(client.resize_grip)body.y2-=12;
        for(unsigned i=0;i<repair.count;++i){
            const auto& copy=repair.copies[i];auto r=copy.destination;
            if(copy.source||!visibility_valid(r)||r.x1<body.x1||r.y1<body.y1||r.x2>body.x2||r.y2>body.y2)return reject();
            for(unsigned j=0;j<animation.count;++j)
                if(visibility_valid(visibility_intersect(r,animation.copies[j].destination)))return reject();
            for(unsigned j=0;j<count;++j)if(visibility_valid(visibility_intersect(r,rects[j])))return reject();
            for(unsigned j=0;j<i;++j)if(visibility_valid(visibility_intersect(r,repair.copies[j].destination)))return reject();
        }
        for(unsigned i=0;i<repair.count;++i)rects[count++]=repair.copies[i].destination;
        return true;
    }
    bool append_opaque(VisibleRect r,const VisibleRect* opaque,unsigned n,const GpuScenePlan& animation){
        if(!active||failed||count==capacity||!visibility_valid(r))return false;
        for(unsigned i=0;i<n;++i){
            auto cover=opaque[i];
            if(r.x1<cover.x1||r.y1<cover.y1||r.x2>cover.x2||r.y2>cover.y2)continue;
            // Reuse source, body, disjointness and capacity checks. Keep the
            // fixed-size packet off the compositor stack.
            static GpuScenePlan repair{};
            repair.client_count=repair.count=1;
            repair.clients[0]=client;repair.copies[0]={0,r,0,0};
            return append(repair,animation);
        }
        return false;
    }
    bool ready(const GpuSceneClient& c,const GpuScenePlan& animation) const{
        if(failed)return false;
        if(!count)return true;
        if(!same_client(c)||animation.count>GpuScenePlan::capacity-count)return false;
        for(unsigned i=0;i<count;++i)for(unsigned j=0;j<animation.count;++j)
            if(visibility_valid(visibility_intersect(rects[i],animation.copies[j].destination)))return false;
        return true;
    }
    bool combine(GpuScenePlan* alpha) const{
        if(!count)return !failed;
        if(failed||alpha->client_count!=1||!same_client(alpha->clients[0])||alpha->count>GpuScenePlan::capacity-count)return false;
        for(unsigned i=0;i<count;++i)for(unsigned j=0;j<alpha->count;++j)
            if(visibility_valid(visibility_intersect(rects[i],alpha->copies[j].destination)))return false;
        for(unsigned i=0;i<count;++i)alpha->copies[alpha->count++]={0,rects[i],0,0};
        alpha->accelerated=1;return true;
    }
};
#endif
