#include <cassert>
#include <cstdio>
#include "src/gui/damage.h"
int main() {
    DamageList damage{};
    // Three touching exposed strips around an opaque foreground window must
    // not become a full bounding box containing the hidden center.
    damage_add(&damage,0,99,0,19);
    damage_add(&damage,0,19,20,79);
    damage_add(&damage,80,99,20,79);
    assert(damage.n==3);
    uint64_t area=0;for(unsigned i=0;i<damage.n;++i)area+=dmg_area(damage.r[i]);
    assert(area==4400);
    damage_add(&damage,0,99,80,99);assert(damage.n==4);
    DamageList adjacent{};damage_add(&adjacent,0,9,0,9);damage_add(&adjacent,10,19,0,9);
    assert(adjacent.n==1&&dmg_area(adjacent.r[0])==200);
    damage_add(&adjacent,5,8,2,4);assert(adjacent.n==1);
    // Random capacity pressure must preserve all requested pixels while
    // keeping the fixed list bounded, including after waste-based merging.
    damage_clear(&damage);
    bool requested[64][64]{};uint32_t seed=314159;
    for(unsigned n=0;n<250;++n){
        auto random=[&](){return seed=seed*1664525+1013904223;};
        unsigned x=random()%64,y=random()%64,w=1+random()%11,h=1+random()%9;
        unsigned x2=x+w-1<64?x+w-1:63,y2=y+h-1<64?y+h-1:63;
        damage_add(&damage,x,x2,y,y2);assert(damage.n<=DAMAGE_MAX);
        for(unsigned py=y;py<=y2;++py)for(unsigned px=x;px<=x2;++px)requested[py][px]=true;
        for(unsigned py=0;py<64;++py)for(unsigned px=0;px<64;++px)if(requested[py][px]){
            bool covered=false;for(unsigned i=0;i<damage.n;++i){auto r=damage.r[i];
                if(px>=r.x1&&px<=r.x2&&py>=r.y1&&py<=r.y2)covered=true;}
            assert(covered);
        }
    }
    puts("Damage regions preserve occlusion holes, merge exact rectangles and retain coverage at capacity.");
}
