#ifndef WYZAR_USER_ADDRESS_SPACE_H
#define WYZAR_USER_ADDRESS_SPACE_H
#include <stdint.h>

// Software PTE bit: the process owns this physical page. Window surfaces are
// borrowed mappings; their pages remain owned by the compositor's retire queue.
constexpr uint64_t USER_PAGE_OWNED = 1ull << 9;

// Only call for an inactive, private user address space. Its lower-half page
// tables are private, while its upper half shares kernel tables. Keep physical
// access/release injectable so host tests exercise the actual ownership walk.
template<class Map, class Release>
static void release_user_table(uint64_t physical, unsigned level, Map& map, Release& release) {
    constexpr uint64_t address_mask=0x000ffffffffff000ull;
    auto* table=map(physical);
    unsigned entries=level==4?256:512;
    for(unsigned i=0;i<entries;++i){
        uint64_t entry=table[i];
        if(!(entry&1))continue;
        table[i]=0;
        if(level==1){if(entry&USER_PAGE_OWNED)release(entry&address_mask);}
        else if(!(entry&(1ull<<7)))release_user_table(entry&address_mask,level-1,map,release);
        // The user loader never allocates huge pages. Borrowed huge mappings
        // are not process allocations and must not be traversed or freed.
    }
    release(physical);
}
template<class Map, class Release>
static void release_user_address_space(uint64_t root, Map map, Release release) {
    release_user_table(root,4,map,release);
}
#endif
