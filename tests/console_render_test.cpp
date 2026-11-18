#include <cassert>
#include <vector>
#define NATIVE_HOST_TEST
#include "../user/console.cpp"
static uint64_t native_call(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t){return 0;}
int main(){
    const char* edited="\x1b[2J\x1b[Hhello\x1b[2DX";
    while(*edited)put(*edited++);
    assert(row==0&&col==4&&cells[0][3]=='X');
    transcript_start=transcript_size=0;
    layout_console(W,H);
    for(int i=0;i<120;++i)put('a');assert(row==1&&col==20);
    layout_console(400,300);assert(COLS==50&&row==2&&col==20);
    layout_console(1000,700);assert(COLS==125&&row==0&&col==120);
    for(int x=0;x<120;++x)assert(cells[0][x]=='a');
    put('\n');const char* text="Retained output\n";while(*text)put(*text++);
    std::vector<uint32_t> pixels;
    for(auto size: {std::pair{360,220},std::pair{1200,740},std::pair{744,440}}){
        layout_console(size.first,size.second);pixels.assign(W*H,0);screen={pixels.data(),W,H};
        assert(COLS<=MAX_COLS&&ROWS<=MAX_ROWS&&COLS>0&&ROWS>0);render(1);
        running=false;render(1);running=true;
    }
    transcript_start=transcript_size=0;layout_console(400,300);
    const char* green="\x1b[32mG\x1b[0mW";
    while(*green)put(*green++);
    assert(colors[0][0]==0xff00ff00&&colors[0][1]==0xffffffff);
    for(int i=0;i<ROWS+HISTORY_ROWS+10;++i){put('x');put('\n');}
    assert(history_size==HISTORY_ROWS&&row==ROWS-1);
    scroll_view(1);assert(view_offset==1);
    put('y');put('\n');assert(view_offset==2);
    scroll_view(1000);assert(view_offset==HISTORY_ROWS);
    pixels.assign(W*H,0);screen={pixels.data(),W,H};render(1);
    scroll_view(-1000);assert(view_offset==0);
    const char* clear="\x1b[2J";while(*clear)put(*clear++);
    assert(history_size==0&&view_offset==0&&row==0&&col==0);
}
