#ifndef WYZAR_TASKBAR_H
#define WYZAR_TASKBAR_H
#include "taskbar_layout.h"
static_assert(MAX_WINDOWS==TASKBAR_MAX_WINDOWS);

struct TaskbarSnapshot {
    Window* windows[MAX_WINDOWS];
    int count;
    TaskbarLayout layout;
};
static TaskbarSnapshot wm_taskbar_snapshot() {
    TtyIrqGuard guard;
    TaskbarSnapshot snapshot{};
    TaskbarEntry entries[MAX_WINDOWS]{};
    for(int i=0;i<g_window_count;++i){
        Window* win=g_window_list[i];
        if(!win||!win->visible)continue;
        const char* application=win->is_system?nullptr:win->executable;
        Task* task=g_task_list;
        if(task&&!win->is_system)do{
            if(task->pid==win->owner_pid){if(task->console_host)application=task->launch_target;break;}
            task=task->next;
        }while(task!=g_task_list);
        int at=snapshot.count++;
        snapshot.windows[at]=win;
        entries[at]={application,!win->minimized&&win->id==g_focused_win_id};
    }
    snapshot.layout=taskbar_layout(entries,snapshot.count,6+g_sb_w+(10*fb->height)/1080,
                                  (int)fb->width-wm_tray_w()-14,g_wbtn_w,(4*fb->height)/1080);
    return snapshot;
}
static int g_taskbar_group_id=0,g_taskbar_first=0,g_taskbar_anchor=0;
// Popup mode is 0=closed, 1=overflow groups, 2=windows in an application.
static int g_taskbar_popup=0;
struct TaskbarPopup {
    int ids[MAX_WINDOWS],count,x,y,w,h,rows;
};
static TaskbarPopup wm_taskbar_panel(const TaskbarSnapshot& snapshot) {
    TaskbarPopup panel{};
    const auto& layout=snapshot.layout;
    int group=-1;
    if(g_taskbar_popup==2)for(int i=0;i<snapshot.count;++i)
        if(snapshot.windows[i]->id==g_taskbar_group_id)group=layout.membership[i];
    if(g_taskbar_popup==1){
        for(int i=layout.shown;i<layout.count;++i)panel.ids[panel.count++]=snapshot.windows[layout.groups[i].representative]->id;
    }else if(group>=0){
        for(int i=0;i<snapshot.count;++i)if(layout.membership[i]==group)panel.ids[panel.count++]=snapshot.windows[i]->id;
    }
    panel.w=300;if(panel.w>(int)fb->width-12)panel.w=fb->width-12;
    panel.rows=((int)fb->height-g_taskbar_h-52)/32;
    if(panel.rows>panel.count)panel.rows=panel.count;
    if(panel.rows<0)panel.rows=0;
    panel.h=40+panel.rows*32;
    panel.x=g_taskbar_anchor;
    if(panel.x+panel.w>(int)fb->width-6)panel.x=fb->width-panel.w-6;
    if(panel.x<6)panel.x=6;
    panel.y=fb->height-g_taskbar_h-panel.h-4;
    return panel;
}
static void wm_taskbar_damage(bool full=true) {
    if(!fb)return;
    if(full)wm_dirty_add(0,fb->width-1,0,fb->height-1);
    else{
        auto snapshot=wm_taskbar_snapshot();auto panel=wm_taskbar_panel(snapshot);
        wm_dirty_add(panel.x,panel.x+panel.w-1,panel.y,panel.y+panel.h-1);
    }
    g_needs_refresh=true;
}
static void wm_taskbar_close() {
    if(!g_taskbar_popup)return;
    g_taskbar_popup=0;g_taskbar_first=0;wm_taskbar_damage();
}
static bool wm_taskbar_escape(char c,uint32_t mods) {
    if(c!=27||mods||!g_taskbar_popup)return false;
    wm_taskbar_close();return true;
}
static void wm_taskbar_open(int mode,int id,int x) {
    wm_menu_close();wm_clipboard_close();
    g_taskbar_popup=mode;g_taskbar_group_id=id;g_taskbar_anchor=x;g_taskbar_first=0;
    wm_taskbar_damage();
}
static void wm_taskbar_activate(int id) {
    for(int i=0;i<g_window_count;++i)if(g_window_list[i]->id==id){wm_raise_window(i);return;}
}
static void wm_taskbar_click_bar(int x) {
    TtyIrqGuard guard;
    auto snapshot=wm_taskbar_snapshot();const auto& layout=snapshot.layout;
    if(layout.overflow_width&&x>=layout.overflow_x&&x<layout.overflow_x+layout.overflow_width){
        if(g_taskbar_popup==1)wm_taskbar_close();else wm_taskbar_open(1,0,layout.overflow_x);
        return;
    }
    for(int group=0;group<layout.shown;++group){
        int left=layout.start+group*(layout.width+layout.gap);
        if(x<left||x>=left+layout.width)continue;
        Window* win=snapshot.windows[layout.groups[group].representative];
        if(layout.groups[group].count>1){
            if(g_taskbar_popup==2&&g_taskbar_group_id==win->id)wm_taskbar_close();
            else wm_taskbar_open(2,win->id,left);
        }else{
            wm_taskbar_close();
            for(int i=0;i<g_window_count;++i)if(g_window_list[i]==win){
                if(win->id==g_focused_win_id&&!win->minimized)wm_minimize_window(i);else wm_raise_window(i);
                break;
            }
        }
        return;
    }
    wm_taskbar_close();
}
static bool wm_taskbar_popup_click(int x,int y) {
    if(!g_taskbar_popup)return false;
    if(y>=(int)fb->height-(int)g_taskbar_h)return false;
    TtyIrqGuard guard;
    auto snapshot=wm_taskbar_snapshot();auto panel=wm_taskbar_panel(snapshot);
    if(x<panel.x||x>=panel.x+panel.w||y<panel.y||y>=panel.y+panel.h){wm_taskbar_close();return false;}
    int row=(y-panel.y-36)/32;
    if(y<panel.y+36||row>=panel.rows)return true;
    int selected=g_taskbar_first+row;
    if(selected>=panel.count)return true;
    int id=panel.ids[selected];
    if(g_taskbar_popup==1){
        for(int i=0;i<snapshot.count;++i)if(snapshot.windows[i]->id==id){
            if(snapshot.layout.groups[snapshot.layout.membership[i]].count>1){wm_taskbar_open(2,id,panel.x);return true;}
            break;
        }
    }
    wm_taskbar_close();wm_taskbar_activate(id);return true;
}
static void wm_taskbar_scroll(int delta) {
    auto snapshot=wm_taskbar_snapshot();auto panel=wm_taskbar_panel(snapshot);
    g_taskbar_first+=delta;
    int last=panel.count-panel.rows;if(last<0)last=0;
    if(g_taskbar_first>last)g_taskbar_first=last;
    if(g_taskbar_first<0)g_taskbar_first=0;
    wm_taskbar_damage(false);
}
static void wm_taskbar_render_popup(int min_y,int max_y) {
    if(!g_taskbar_popup)return;
    auto snapshot=wm_taskbar_snapshot();auto panel=wm_taskbar_panel(snapshot);
    if(!panel.count){wm_taskbar_close();return;}
    int last=panel.count-panel.rows;if(g_taskbar_first>last)g_taskbar_first=last;
    auto rect=[&](int x,int y,int w,int h,uint32_t color){
        for(int py=y;py<y+h;++py){
            if(py<min_y||py>max_y||py<0||py>=(int)fb->height)continue;
            auto* pixels=(uint32_t*)(g_master_backbuffer+(uint64_t)py*fb->pitch);
            int right=x+w;if(right>(int)fb->width)right=fb->width;
            int left=x<0?0:x;if(right>left)memset_32(pixels+left,color,right-left);
        }
    };
    rect(panel.x,panel.y,panel.w,panel.h,0xff151b2e);
    rect(panel.x,panel.y,panel.w,32,0xff303955);
    wm_draw_string(panel.x+10,panel.y+8,g_taskbar_popup==1?"More apps":"Open windows",0xffeeeeff,g_font_scale_100,panel.w-20);
    for(int row=0;row<panel.rows;++row){
        Window* win=wm_find_window(panel.ids[g_taskbar_first+row]);if(!win)continue;
        int y=panel.y+36+row*32;
        bool hover=g_mouse_x>=panel.x&&g_mouse_x<panel.x+panel.w&&g_mouse_y>=y&&g_mouse_y<y+32;
        if(hover)rect(panel.x+2,y,panel.w-4,32,0xff364366);
        const Image& icon=wm_window_icons(win).task;
        if(icon.pixels)wm_draw_icon(icon,panel.x+8,y+4,panel.x,min_y,panel.x+panel.w,max_y+1);
        char label[96];resources::copy(label,sizeof(label),win->title);
        if(g_taskbar_popup==2){char number[24];to_str(win->id,number);resources::append(label,sizeof(label)," #");resources::append(label,sizeof(label),number);}
        wm_draw_string(panel.x+40,y+8,label,win->id==g_focused_win_id?0xffffffff:0xffbfc9e8,g_font_scale_100,panel.w-52);
    }
    if(panel.count>panel.rows){
        int track=panel.rows*32;
        int thumb=track*panel.rows/panel.count;
        rect(panel.x+panel.w-4,panel.y+36+track*g_taskbar_first/panel.count,2,thumb,0xff8295c4);
    }
}
#endif
