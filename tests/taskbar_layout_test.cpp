#include <cassert>
#include "../src/gui/taskbar_layout.h"
int main() {
    TaskbarEntry entries[]={{"/bin/init.elf",false},{"/bin/init.elf",true},
                            {"/bin/notepad.wiz",false},{nullptr,false},{nullptr,false}};
    auto layout=taskbar_layout(entries,5,100,800,140,4);
    assert(layout.count==4&&layout.shown==4&&!layout.overflow_width);
    assert(layout.groups[0].count==2&&layout.groups[0].active);
    assert(layout.membership[0]==layout.membership[1]);
    assert(layout.membership[3]!=layout.membership[4]);
    layout=taskbar_layout(entries,5,100,400,140,4);
    assert(layout.shown==1&&layout.overflow_width==36);
    assert(layout.overflow_x+layout.overflow_width<=400);
    layout=taskbar_layout(entries,5,100,120,140,4);
    assert(layout.shown==0&&layout.overflow_width==20&&layout.overflow_x==100);
    layout=taskbar_layout(entries,0,100,800,140,4);
    assert(layout.count==0&&layout.shown==0&&!layout.overflow_width);
    TaskbarEntry many[TASKBAR_MAX_WINDOWS]{};
    for(int i=0;i<TASKBAR_MAX_WINDOWS;++i)many[i]={"terminal",i==31};
    layout=taskbar_layout(many,TASKBAR_MAX_WINDOWS,100,800,140,4);
    assert(layout.count==1&&layout.groups[0].count==32&&layout.groups[0].active);
}
