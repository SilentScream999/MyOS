#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include "../src/arch/user_address_space.h"
int main(){
    std::map<uint64_t,std::array<uint64_t,512>> tables;
    std::set<uint64_t> freed;
    // Two user branches, including one empty table left by a closed window.
    tables[0x1000][0]=0x2007;tables[0x2000][0]=0x3007;tables[0x3000][0]=0x4007;
    tables[0x4000][0]=0x5007|USER_PAGE_OWNED;
    tables[0x4000][1]=0x6007|USER_PAGE_OWNED;
    tables[0x4000][2]=0x7007; // compositor-owned window surface
    tables[0x4000][3]=0x8000|USER_PAGE_OWNED; // non-present: not an allocation
    tables[0x1000][192]=0x9007;tables[0x9000][0]=0xa007;tables[0xa000][0]=0xb007;tables[0xb000]={};
    tables[0x2000][1]=0x40000087; // borrowed huge mapping
    tables[0x1000][256]=0xc007;tables[0xc000][0]=0xd007; // shared kernel half
    release_user_address_space(0x1000,
        [&](uint64_t page){assert(tables.count(page));return tables.at(page).data();},
        [&](uint64_t page){assert(freed.insert(page).second);});
    assert((freed==std::set<uint64_t>{0x1000,0x2000,0x3000,0x4000,0x5000,0x6000,0x9000,0xa000,0xb000}));
    assert(tables.at(0x1000)[256]==0xc007&&tables.at(0xc000)[0]==0xd007);
    for(unsigned i=0;i<256;++i)assert(!tables.at(0x1000)[i]);
    puts("User address-space ownership, private tables, borrowed surfaces and kernel preservation passed.");
}
