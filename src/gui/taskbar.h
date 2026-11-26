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
// Popup mode is 0=closed, 1=overflow groups, 2=windows in an application, 3=volume.
static int g_taskbar_popup=0;
static bool g_volume_drag=false;
static uint64_t g_taskbar_popup_revision=1;
static uint64_t g_taskbar_popup_renders=0,g_taskbar_popup_reuses=0;
struct TaskbarPopup {
    int ids[MAX_WINDOWS],count,x,y,w,h,rows;
};
static TaskbarPopup wm_taskbar_panel(const TaskbarSnapshot& snapshot) {
    TaskbarPopup panel{};
    if(g_taskbar_popup==3){
        auto volume=volume_popup_layout(fb->width,fb->height,g_taskbar_h,g_taskbar_anchor);
        panel.x=volume.x;panel.y=volume.y;panel.w=volume.w;panel.h=volume.h;
        panel.count=1;return panel; // Nonempty opaque panel participates in retained cutouts.
    }
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
    ++g_taskbar_popup_revision;
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
    g_taskbar_popup=0;g_taskbar_first=0;g_volume_drag=false;wm_taskbar_damage();
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
    if(x>=wm_volume_tray_x()&&x<wm_volume_tray_x()+(int)g_taskbar_h){
        if(g_taskbar_popup==3)wm_taskbar_close();else wm_taskbar_open(3,0,wm_volume_tray_x());
        return;
    }
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
    if(g_taskbar_popup==3){
        VolumePopupLayout volume{panel.x,panel.y,panel.w,panel.h};
        if(y>=volume.slider_y()-20&&y<=volume.slider_y()+20){
            if(x<panel.x+52)g_audio_mixer.mute(2);
            else {g_volume_drag=true;g_audio_mixer.percent(volume.percent(x));g_audio_mixer.mute(0);}
        }
        return true;
    }
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
    if(g_taskbar_popup==3){
        TtyIrqGuard guard;
        int next=(int)g_audio_mixer.value.percent-delta*5;
        g_audio_mixer.percent(next<0?0:next>100?100:next);return;
    }
    auto snapshot=wm_taskbar_snapshot();auto panel=wm_taskbar_panel(snapshot);
    g_taskbar_first+=delta;
    int last=panel.count-panel.rows;if(last<0)last=0;
    if(g_taskbar_first>last)g_taskbar_first=last;
    if(g_taskbar_first<0)g_taskbar_first=0;
    wm_taskbar_damage(false);
}
static int wm_taskbar_popup_hover(int x,int y) {
    if(!g_taskbar_popup||g_taskbar_popup==3)return -1;
    auto panel=wm_taskbar_panel(wm_taskbar_snapshot());
    if(x<panel.x||x>=panel.x+panel.w||y<panel.y+36||y>=panel.y+36+panel.rows*32)return -1;
    return (y-panel.y-36)/32;
}
static void wm_taskbar_paint_popup(const TaskbarPopup& panel,int min_y,int max_y) {
    auto rect=[&](int x,int y,int w,int h,uint32_t color){
        for(int py=y;py<y+h;++py){
            if(py<min_y||py>max_y||py<g_ui_clip_top||py>=g_ui_clip_bottom||py<0||py>=(int)fb->height)continue;
            auto* pixels=(uint32_t*)(g_master_backbuffer+(uint64_t)py*fb->pitch);
            int right=x+w;if(right>(int)fb->width)right=fb->width;
            int left=x<0?0:x;
            if(left<g_ui_clip_left)left=g_ui_clip_left;
            if(right>g_ui_clip_right)right=g_ui_clip_right;
            if(right>left)memset_32(pixels+left,color,right-left);
        }
    };
    rect(panel.x,panel.y,panel.w,panel.h,0xff151b2e);
    if(g_taskbar_popup==3){
        AudioVolume state=g_audio_mixer.value;
        VolumePopupLayout volume{panel.x,panel.y,panel.w,panel.h};
        rect(panel.x,panel.y,panel.w,32,0xff303955);
        wm_draw_string(panel.x+14,panel.y+8,"Volume",0xffeeeeff,g_font_scale_100,panel.w-28);
        char label[24];to_str(state.percent,label);resources::append(label,sizeof(label),state.muted?"%  Muted":"%");
        wm_draw_string(panel.x+16,panel.y+44,label,0xffbfc9e8,g_font_scale_100,panel.w-32);
        int cy=volume.slider_y(),left=volume.slider_left(),right=volume.slider_right(),thumb=volume.thumb(state.percent);
        wm_draw_volume_icon(panel.x+19,cy-11,22,state.percent,state.muted,min_y,max_y);
        rect(left,cy-2,right-left,4,0xff465475);
        if(thumb>left)rect(left,cy-2,thumb-left,4,state.muted?0xff8290ac:0xff9d8aff);
        rect(thumb-5,cy-10,10,20,state.muted?0xff9da8c0:0xffd2caff);
        return;
    }
    rect(panel.x,panel.y,panel.w,32,0xff303955);
    wm_draw_string(panel.x+10,panel.y+8,g_taskbar_popup==1?"More apps":"Open windows",0xffeeeeff,g_font_scale_100,panel.w-20);
    for(int row=0;row<panel.rows;++row){
        Window* win=wm_find_window(panel.ids[g_taskbar_first+row]);if(!win)continue;
        int y=panel.y+36+row*32;
        bool hover=g_mouse_x>=panel.x&&g_mouse_x<panel.x+panel.w&&g_mouse_y>=y&&g_mouse_y<y+32;
        if(hover)rect(panel.x+2,y,panel.w-4,32,0xff364366);
        const Image& icon=wm_window_icons(win).task;
        if(icon.pixels)wm_draw_icon(icon,panel.x+8,y+4,
            panel.x>g_ui_clip_left?panel.x:g_ui_clip_left,
            min_y>g_ui_clip_top?min_y:g_ui_clip_top,
            panel.x+panel.w<g_ui_clip_right?panel.x+panel.w:g_ui_clip_right,
            max_y+1<g_ui_clip_bottom?max_y+1:g_ui_clip_bottom);
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
// All popup modes use the same cache, clipping and damage contract. New content
// invalidates it through wm_taskbar_damage; animation frames only copy pixels.
static void wm_taskbar_render_popup(int min_y,int max_y) {
    if(!g_taskbar_popup)return;
    auto snapshot=wm_taskbar_snapshot();auto panel=wm_taskbar_panel(snapshot);
    if(!panel.count){wm_taskbar_close();return;}
    int last=panel.count-panel.rows;
    if(g_taskbar_first>last)g_taskbar_first=last;
    uint64_t key=1469598103934665603ULL;
    auto hash=[&](uint64_t value){key=(key^value)*1099511628211ULL;};
    hash(g_taskbar_popup_revision);hash(g_taskbar_popup);hash(g_taskbar_first);
    hash(panel.x);hash(panel.y);hash(panel.w);hash(panel.h);hash(panel.count);
    hash(fb->pitch);hash(fb->height);hash(g_font_scale_100);hash(g_decoration_epoch);
    hash(g_focused_win_id);hash(wm_taskbar_popup_hover(g_mouse_x,g_mouse_y));
    // Include live state as well as explicit damage: titles/focus can change
    // during a client update without a pointer or popup input event.
    hash(g_audio_mixer.value.percent);hash(g_audio_mixer.value.muted);
    for(int i=0;i<panel.rows;++i){
        auto* win=wm_find_window(panel.ids[g_taskbar_first+i]);
        hash(win?win->id:0);
        if(win)for(const char* p=win->title;*p;++p)hash((unsigned char)*p);
    }
    wm_render_cached_popup({panel.x,panel.y,panel.x+panel.w,panel.y+panel.h},key,1,min_y,max_y,
        g_taskbar_popup_renders,g_taskbar_popup_reuses,
        [&](int top,int bottom){wm_taskbar_paint_popup(panel,top,bottom);});
}
static bool wm_volume_pointer(bool held){
    if(!g_volume_drag)return false;
    if(!held||g_taskbar_popup!=3){g_volume_drag=false;return true;}
    TtyIrqGuard guard;
    auto panel=wm_taskbar_panel(wm_taskbar_snapshot());
    VolumePopupLayout volume{panel.x,panel.y,panel.w,panel.h};
    g_audio_mixer.percent(volume.percent(g_mouse_x));g_audio_mixer.mute(0);return true;
}
static void wm_audio_changed(){
    TtyIrqGuard guard;
    static int previous_icon=-1;
    const auto& volume=g_audio_mixer.value;
    int icon=volume.muted||!volume.percent?0:volume.percent>50?2:1;
    if(icon!=previous_icon&&fb){
        int x=wm_volume_tray_x();
        wm_dirty_add(x,x+g_taskbar_h-1,fb->height-g_taskbar_h,fb->height-1);
        g_needs_refresh=true;
    }
    previous_icon=icon;
    if(g_taskbar_popup==3)wm_taskbar_damage(false);
}
#endif
