#ifndef WYZAR_TASKBAR_LAYOUT_H
#define WYZAR_TASKBAR_LAYOUT_H

constexpr int TASKBAR_MAX_WINDOWS=32;
struct TaskbarEntry {
    const char* application;
    bool focused;
};
struct TaskbarGroup {
    int representative,count;
    bool active;
};
struct TaskbarLayout {
    TaskbarGroup groups[TASKBAR_MAX_WINDOWS];
    int membership[TASKBAR_MAX_WINDOWS];
    int count,shown,start,width,gap,overflow_x,overflow_width;
};
static bool taskbar_equal(const char* a,const char* b) {
    if(!a||!b||!*a||!*b)return false;
    while(*a&&*a==*b){++a;++b;}
    return *a==*b;
}
static TaskbarLayout taskbar_layout(const TaskbarEntry* entries,int count,int start,int right,int width,int gap) {
    TaskbarLayout result{};
    result.start=start;result.width=width;result.gap=gap;
    for(int i=0;i<count&&i<TASKBAR_MAX_WINDOWS;++i){
        int group=0;
        while(group<result.count&&!taskbar_equal(entries[i].application,entries[result.groups[group].representative].application))++group;
        if(group==result.count){result.groups[group].representative=i;++result.count;}
        ++result.groups[group].count;
        result.groups[group].active|=entries[i].focused;
        result.membership[i]=group;
    }
    int available=right-start;
    if(available<0)available=0;
    int capacity=(available+gap)/(width+gap);
    result.shown=result.count;
    if(result.count>capacity){
        result.overflow_width=36;
        if(result.overflow_width>available)result.overflow_width=available;
        int slots=(available-result.overflow_width)/(width+gap);
        result.shown=slots>0?slots:0;
        result.overflow_x=start+result.shown*(width+gap);
    }
    return result;
}
#endif
