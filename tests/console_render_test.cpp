#include <cassert>
#include <vector>
#define NATIVE_HOST_TEST
#include "../user/console.cpp"
static uint64_t native_call(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t){return 0;}
int main(){
    for(int i=0;i<120;++i)put('a');assert(row==1&&col==32);
    layout_console(400,300);assert(COLS==47&&row==2&&col==26);
    layout_console(1000,700);assert(COLS==122&&row==0&&col==120);
    for(int x=0;x<120;++x)assert(cells[0][x]=='a');
    put('\n');const char* text="Retained output\n";while(*text)put(*text++);
    std::vector<uint32_t> pixels;
    for(auto size: {std::pair{360,220},std::pair{1200,740},std::pair{744,440}}){
        layout_console(size.first,size.second);pixels.assign(W*H,0);screen={pixels.data(),W,H};
        assert(COLS<=MAX_COLS&&ROWS<=MAX_ROWS&&COLS>0&&ROWS>0);render(1);
        running=false;render(1);running=true;
    }
}
