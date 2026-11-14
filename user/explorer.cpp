// Native, freestanding file manager. Host tests exercise the same filesystem
// and input code through NATIVE_HOST_TEST; no host GUI toolkit is involved.
#include "user/native_ui.h"
#include "src/core/app.h"
static char current_dir[256]="/home";
static char operation_log[2048];
static void print(const char* s) {
    unsigned at=native_length(operation_log);
    while(*s && at+1<sizeof(operation_log)) operation_log[at++]=*s++;
    operation_log[at]=0;
}
static void print_dec(uint64_t n) { char b[24]; native_number(n,b); print(b); }
#include "user/wiz_cmd.h"

constexpr int WIDTH=860, HEIGHT=560, SIDE=164, LIST_TOP=138, ROW=34, VISIBLE=11;
constexpr uint32_t BG=0xff1a1925, PANEL=0xff222131, LINE=0xff39334b,
    INK=0xffeeebf8, MUTED=0xffa49bb6, BLUE=0xffc9b5ff;
struct ExplorerEntry { char name[256]; uint64_t size,type; };
static ExplorerEntry entries[512];
static int entry_count=0, selected=-1, scroll_row=0;
static bool show_hidden=false, listing_truncated=false, listing_error=false, dirty=true;
static char history[32][256]; static int history_count=0, history_at=-1;
static char clipboard[256]; static bool clipboard_cut=false;
static char status_line[192]="Double-click a folder or app to open it.";
static char edit_text[256], dialog_target[256], dialog_text[4097];
enum Dialog { NONE, NEW_FOLDER, RENAME, DELETE_ITEM, PREVIEW, PACKAGE_RESULT, NEW_FILE };
static Dialog dialog=NONE;
static bool address_edit=false, edit_replace=false;
static char address[256]="/home";
static uint64_t window_id=0, frame=0, last_click_frame=0;
static int last_click=-1;
static int hovered_button=-1;
static NativeSurface surface{};

static void status(const char* text) { native_copy(status_line,sizeof(status_line),text); dirty=true; }
static int compare(const char* a,const char* b) {
    while(*a&&*b) { unsigned x=(unsigned char)*a++,y=(unsigned char)*b++;
        if(x>='A'&&x<='Z')x+=32;if(y>='A'&&y<='Z')y+=32;if(x!=y)return x<y?-1:1; }
    return *a?1:*b?-1:0;
}
static bool join_path(const char* dir,const char* name,char* out) {
    return wz_join(out,256,dir,wiz_str_equal(dir,"/")?"":"/",name);
}
static bool selected_path(char* path) {
    return selected>=0&&selected<entry_count&&join_path(current_dir,entries[selected].name,path);
}
static void load_directory() {
    entry_count=0;selected=-1;scroll_row=0;last_click=-1;listing_truncated=false;listing_error=false;
    char name[256]; uint64_t size,type;
    // Bound work even on broken drivers or directories full of hidden files.
    uint64_t i=0;
    for(;i<8192;++i) {
        int64_t result=sys_readdir(current_dir,i,name,&size,&type);
        if(result<=0) { listing_error=result<0;break; }
        if(wiz_str_equal(name,".")||wiz_str_equal(name,"..")||(!show_hidden&&name[0]=='.'))continue;
        if(entry_count==512) { listing_truncated=true;break; }
        ExplorerEntry value{};native_copy(value.name,sizeof(value.name),name);value.size=size;value.type=type;
        int at=entry_count++;
        while(at>0 && ((value.type==2&&entries[at-1].type!=2) ||
              ((value.type==2)==(entries[at-1].type==2)&&compare(value.name,entries[at-1].name)<0))) {
            entries[at]=entries[at-1];--at;
        }
        entries[at]=value;
    }
    if(i==8192)listing_truncated=true;
    native_copy(address,sizeof(address),current_dir);
    if(listing_error)status("Folder read failed. Reconnect storage, then Refresh.");
    else if(listing_truncated)status("Showing the first 512 items; this folder has more entries.");
    else status("Double-click to open. Select an item for file actions.");
    dirty=true;
}
static bool navigate(const char* path,bool remember=true) {
    char absolute[256]; file_info info{};
    if(!wz_absolute(path,absolute,sizeof(absolute))) { status("That path is too long.");return false; }
    if(sys_stat(absolute,&info)!=0||info.type!=2) { status("Cannot open that folder. Check the path and storage.");return false; }
    if(remember) {
        history_count=history_at+1;
        if(history_count==32) { for(int i=1;i<32;++i)native_copy(history[i-1],256,history[i]);--history_count; }
        native_copy(history[history_count++],256,absolute);history_at=history_count-1;
    }
    native_copy(current_dir,sizeof(current_dir),absolute);
    native_call(80,(uint64_t)current_dir); // App launches inherit the folder being browsed.
    address_edit=false;load_directory();return true;
}
static void history_step(int direction) {
    int next=history_at+direction;
    if(next>=0&&next<history_count&&navigate(history[next],false))history_at=next;
}
static void select_row(int index) {
    if(index<0||index>=entry_count)return;
    selected=index;if(selected<scroll_row)scroll_row=selected;
    if(selected>=scroll_row+VISIBLE)scroll_row=selected-VISIBLE+1;
    last_click=-1;dirty=true;
    if(wiz_name_is_package(entries[selected].name))status("Double-click runs this app. Install adds it to Start; Pin keeps it in this location.");
}
static void open_selected() {
    char path[256];if(!selected_path(path))return;
    if(entries[selected].type==2) { navigate(path);return; }
    if(app_name_supported(path)) {
        int64_t pid=native_call(213,(uint64_t)path,1);
        if(pid<=0){status("Could not launch this app. Its package may be invalid or unsupported.");return;}
        status("App opened independently. Console apps use their own terminal.");return;
    }
    uint64_t fd=sys_open(path,0);if(fd==(uint64_t)-1){status("Cannot read that file.");return;}
    uint64_t n=sys_read(fd,dialog_text,sizeof(dialog_text)-1);sys_close(fd);
    if(n>=sizeof(dialog_text)){status("File read failed.");return;}
    bool binary=false;for(uint64_t i=0;i<n;++i)if((uint8_t)dialog_text[i]<32&&dialog_text[i]!='\n'&&dialog_text[i]!='\r'&&dialog_text[i]!='\t')binary=true;
    if(binary)native_copy(dialog_text,sizeof(dialog_text),"This file has binary content. A viewer for this file type is not installed.");
    else dialog_text[n]=0;
    native_copy(dialog_target,sizeof(dialog_target),entries[selected].name);dialog=PREVIEW;dirty=true;
}
static void begin_dialog(Dialog kind) {
    if((kind==RENAME||kind==DELETE_ITEM)&&!selected_path(dialog_target))return;
    edit_text[0]=0;
    if(kind==RENAME)native_copy(edit_text,sizeof(edit_text),entries[selected].name);
    if(kind==NEW_FOLDER)native_copy(edit_text,sizeof(edit_text),"New folder");
    if(kind==NEW_FILE)native_copy(edit_text,sizeof(edit_text),"New file.txt");
    edit_replace=true;address_edit=false;dialog=kind;dirty=true;
}
static bool valid_filename(const char* name) {
    if(!*name||wiz_str_equal(name,".")||wiz_str_equal(name,".."))return false;
    for(const unsigned char* p=(const unsigned char*)name;*p;++p)if(*p=='/'||*p<32)return false;
    return true;
}
static void commit_dialog() {
    if(dialog==PREVIEW||dialog==PACKAGE_RESULT){dialog=NONE;dirty=true;return;}
    char path[256];int64_t result=-1;
    if(dialog==DELETE_ITEM) {
        file_info info{};
        if(sys_stat(dialog_target,&info)==0)result=info.type==2?sys_rmdir(dialog_target):sys_unlink(dialog_target);
    } else {
        if(!valid_filename(edit_text)||!join_path(current_dir,edit_text,path)){status("Use a nonempty name without '/' that fits the path.");return;}
        if(dialog==NEW_FOLDER)result=sys_mkdir(path,0755);
        else if(dialog==RENAME)result=sys_rename(dialog_target,path);
        else if(dialog==NEW_FILE) {
            file_info existing{};
            if(sys_stat(path,&existing)!=0) { uint64_t fd=sys_open(path,0xc1);if(fd!=(uint64_t)-1){sys_close(fd);result=0;} }
        }
    }
    if(result!=0){status("Operation failed: name exists, folder is not empty, or storage is unavailable.");return;}
    dialog=NONE;load_directory();status("Done.");
}
static void copy_selection(bool cut) {
    if(!selected_path(clipboard))return;
    clipboard_cut=cut;status(cut?"Ready to move. Open a destination folder, then Paste.":"Ready to copy. Open a destination folder, then Paste.");
}
static void paste_selection() {
    if(!clipboard[0]){status("Copy or cut an item first.");return;}
    const char* name=clipboard;for(const char* p=clipboard;*p;++p)if(*p=='/')name=p+1;
    char destination[256];file_info info{},existing{};
    if(!join_path(current_dir,name,destination)){status("Destination path is too long.");return;}
    if(sys_stat(destination,&existing)==0){status("An item with that name already exists. Rename it first.");return;}
    if(sys_stat(clipboard,&info)!=0){status("The source is no longer available.");return;}
    if(clipboard_cut) {
        if(sys_rename(clipboard,destination)!=0){status("Move failed. Source and destination must be on the same filesystem.");return;}
        clipboard[0]=0;load_directory();status("Item moved.");return;
    }
    if(info.type!=1){status("Folder copying is not supported yet. Files can be copied; folders can be moved.");return;}
    // Stage the copy with an exclusive name. The destination is never visible
    // partially written and rename never overwrites another user's file.
    char temporary[256];uint64_t out=(uint64_t)-1;
    for(unsigned attempt=0;attempt<64;++attempt) {
        char number[24],leaf[64];native_number(native_call(39)*64+attempt,number);
        wz_join(leaf,sizeof(leaf),".explorer-copy-",number,".tmp");
        if(!join_path(current_dir,leaf,temporary)){status("Destination path is too long.");return;}
        out=sys_open(temporary,0xc1);if(out!=(uint64_t)-1)break;
    }
    if(out==(uint64_t)-1){status("Cannot create a temporary file in this folder.");return;}
    uint64_t in=sys_open(clipboard,0),done=0;bool ok=in!=(uint64_t)-1;static char block[4096];
    while(ok&&done<info.size) {
        uint64_t want=info.size-done;if(want>sizeof(block))want=sizeof(block);
        uint64_t n=sys_read(in,block,want);
        if(n==0||n>want||sys_write(out,block,n)!=n){ok=false;break;}done+=n;
    }
    if(ok&&native_call(74,out)!=0)ok=false;
    if(in!=(uint64_t)-1)sys_close(in);sys_close(out);
    if(ok&&sys_rename(temporary,destination)==0){load_directory();status("File copied.");}
    else {sys_unlink(temporary);status("Copy failed. The original file was kept.");}
}
static void package_action(bool install) {
    char path[256];if(!selected_path(path)||!wiz_name_is_package(path)){status("Select a .wiz package first.");return;}
    operation_log[0]=0;if(install)wz_install(path,0);else wz_pin(path);
    native_copy(dialog_text,sizeof(dialog_text),operation_log);dialog=PACKAGE_RESULT;dirty=true;
}
static void file_size(uint64_t n,char* out) {
    const char* unit=" B";if(n>=1024*1024){n=(n+524288)/(1024*1024);unit=" MB";}else if(n>=1024){n=(n+512)/1024;unit=" KB";}
    native_number(n,out);wz_append(out,32,unit);
}
struct Button { int x,y,w,h;const char* label; };
static const Button buttons[]={
    {12,12,32,30,"<"},{48,12,32,30,">"},{84,12,32,30,"^"},{120,12,80,30,"Refresh"},
    {176,62,84,30,"New folder"},{266,62,72,30,"New file"},{344,62,60,30,"Rename"},
    {410,62,52,30,"Copy"},{468,62,46,30,"Cut"},{520,62,52,30,"Paste"},
    {578,62,58,30,"Delete"},{642,62,60,30,"Install"},{708,62,48,30,"Pin"},{762,62,80,30,"Hidden"}
};
static bool hit(int x,int y,int rx,int ry,int w,int h){return x>=rx&&x<rx+w&&y>=ry&&y<ry+h;}
static bool button_enabled(int id) {
    if(id==0)return history_at>0;if(id==1)return history_at+1<history_count;
    if(id==2)return !wiz_str_equal(current_dir,"/");if(id==9)return clipboard[0];
    if(id==6||id==7||id==8||id==10)return selected>=0;
    if(id==11||id==12)return selected>=0&&entries[selected].type==1&&wiz_name_is_package(entries[selected].name);
    return true;
}
static void button_symbol(int id,int x,int y,uint32_t color) {
    auto line=[&](int a,int b,int c,int d){surface.line(x+a,y+b,x+c,y+d,color);};
    if(id<3){if(id==2){line(3,7,7,3);line(7,3,11,7);line(7,3,7,12);}
        else {int a=id==0?9:5,b=id==0?5:9;line(a,3,b,7);line(b,7,a,11);}}
    else if(id==3){line(4,3,10,3);line(10,3,12,6);line(12,6,12,9);line(12,9,9,12);
        line(9,12,4,12);line(4,12,2,9);line(2,9,2,6);line(2,6,4,3);line(12,2,12,6);line(9,6,12,6);}
    else if(id==4){line(1,4,5,4);line(5,4,7,6);line(7,6,13,6);line(1,4,1,12);line(1,12,13,12);line(13,6,13,12);
        line(7,8,7,11);line(5,10,9,10);}
    else if(id==5){line(3,2,9,2);line(9,2,12,5);line(12,5,12,13);line(12,13,3,13);line(3,13,3,2);}
    else if(id==6){line(3,11,10,4);line(4,12,11,5);line(3,11,2,13);line(2,13,4,12);}
    else if(id==7){line(2,2,9,2);line(2,2,2,10);line(5,5,12,5);line(12,5,12,13);line(12,13,5,13);line(5,13,5,5);}
    else if(id==8){line(3,2,11,12);line(11,2,3,12);surface.rounded(x+1,y+9,5,5,2,color);surface.rounded(x+9,y+9,5,5,2,color);}
    else if(id==9){line(3,4,12,4);line(3,4,3,13);line(3,13,12,13);line(12,13,12,4);line(5,2,10,2);line(5,2,5,5);line(5,5,10,5);line(10,5,10,2);}
    else if(id==10){line(2,4,13,4);line(5,2,10,2);line(4,5,5,13);line(5,13,10,13);line(10,13,11,5);line(7,6,7,11);line(9,6,9,11);}
    else if(id==11){line(7,2,7,9);line(4,6,7,9);line(7,9,10,6);line(2,10,2,13);line(2,13,12,13);line(12,13,12,10);}
    else if(id==12){line(6,2,12,5);line(6,2,5,7);line(12,5,8,8);line(5,7,8,8);line(5,7,2,12);}
    else if(id==13){line(1,7,5,3);line(5,3,9,3);line(9,3,13,7);line(13,7,9,11);line(9,11,5,11);line(5,11,1,7);surface.rounded(x+5,y+5,4,4,2,color);}
}
static void draw_button(const Button& b,bool active=false,int id=-1) {
    bool enabled=id<0||button_enabled(id),hover=enabled&&id>=0&&hovered_button==id;
    uint32_t edge=active?0xff9971cb:hover?0xff87729f:0xff41394f;
    uint32_t fill=active?0xff44315f:hover?0xff3b304e:0xff2c273b;
    uint32_t ink=enabled?(active?0xffe3c9ff:INK):0xff756d85;
    if(id==10&&hover){edge=0xffa8667c;fill=0xff4a2c3d;ink=0xffffc1d2;}
    surface.rounded(b.x,b.y,b.w,b.h,6,edge);
    surface.rounded(b.x+1,b.y+1,b.w-2,b.h-2,5,fill);
    if(id>=0&&id<3){button_symbol(id,b.x+8,b.y+7,ink);return;}
    int label_width=ui_text_width(ui_font_for_pixels(12),b.label);
    bool symbol=id>=0&&label_width+25<=b.w;
    int start=b.x+(b.w-label_width-(symbol?18:0))/2;
    if(symbol)button_symbol(id,start,b.y+7,ink);
    surface.text(start+(symbol?18:0),b.y+(b.h-ui_font_for_pixels(12)->height)/2,b.label,ink,b.x+b.w-4,12);
}
static void multiline(const char* text,int x,int y,int width,int rows) {
    const UiFont* font=ui_font_for_pixels(14);char line[192];int used=0,pixels=0,row=0;
    for(const unsigned char* p=(const unsigned char*)text;;++p) {
        unsigned char c=*p;if(c=='\r')continue;
        if(!c||c=='\n'||pixels+ui_font_glyph(font,c)->advance>width||used==190) {
            line[used]=0;surface.text(x,y+row*20,line,INK,x+width);used=0;pixels=0;if(++row>=rows||!c)break;
            if(c=='\n')continue;
        }
        if(c=='\t')c=' ';line[used++]=c<32||c>126?'?':c;pixels+=ui_font_glyph(font,c)->advance;
    }
}
static void render() {
    surface.rect(0,0,WIDTH,HEIGHT,BG);surface.rect(0,0,WIDTH,52,PANEL);
    surface.rect(0,52,SIDE,HEIGHT-52,PANEL);surface.rect(SIDE,52,1,HEIGHT-52,LINE);
    for(int i=0;i<14;++i)draw_button(buttons[i],i==13&&show_hidden,i);
    surface.rounded(204,12,638,30,6,address_edit?0xff58416f:0xff15131e);
    surface.text(216,18,address,INK,830);
    surface.text(16,72,"PLACES",MUTED,150,12);
    const char* labels[]={"Home","Applications","App samples","Installed apps","System","Devices"};
    const char* paths[]={"/home","/home/Applications","/share/samples","/apps","/","/mnt"};
    for(int i=0;i<6;++i){int y=105+i*43;if(wiz_str_equal(current_dir,paths[i]))surface.rounded(8,y-4,148,36,5,0xff3b2d50);
        surface.folder(17,y,20);surface.text(46,y+4,labels[i],INK,155);}
    surface.text(17,410,"WyzarOS",MUTED,155,16);surface.text(17,436,"File Explorer",MUTED,155,12);
    surface.rect(SIDE,105,WIDTH-SIDE,32,PANEL);surface.text(216,112,"Name",MUTED,600,12);
    surface.text(627,112,"Type",MUTED,750,12);surface.text(770,112,"Size",MUTED,840,12);
    for(int row=0;row<VISIBLE;++row) {
        int index=scroll_row+row;if(index>=entry_count)break;int y=LIST_TOP+row*ROW;
        auto& e=entries[index];if(index==selected){surface.rounded(SIDE+7,y,WIDTH-SIDE-20,ROW,5,0xff3c2f51);surface.rect(SIDE+8,y+6,2,ROW-12,0xffc8a4ed);}
        else if(row%2)surface.rect(SIDE+7,y,WIDTH-SIDE-20,ROW,0xff201d2d);
        if(e.type==2)surface.folder(180,y+2,28);
        else if(wiz_name_is_package(e.name))surface.wizard(179,y+1,30);
        else surface.document(180,y+2,app_name_supported(e.name)?0xff9a82db:0xff8598b5);
        surface.text(216,y+8,e.name,INK,610);
        const char* type=e.type==2?"Folder":wiz_name_is_package(e.name)?"Wyzar app":app_name_supported(e.name)?"Program":"File";
        surface.text(627,y+8,type,MUTED,750,12);
        if(e.type!=2){char size[32];file_size(e.size,size);surface.text(770,y+8,size,MUTED,842,12);}
    }
    if(entry_count==0)surface.centered_text(SIDE,LIST_TOP,WIDTH-SIDE,HEIGHT-36-LIST_TOP,
        listing_error?"This folder could not be read.":"This folder is empty.",MUTED,16);
    if(entry_count>VISIBLE){int track=VISIBLE*ROW,thumb=track*VISIBLE/entry_count;if(thumb<20)thumb=20;
        surface.rect(850,LIST_TOP,4,track,LINE);surface.rect(850,LIST_TOP+(track-thumb)*scroll_row/(entry_count-VISIBLE),4,thumb,0xff6486b2);}
    surface.rect(0,HEIGHT-36,WIDTH,36,PANEL);surface.text(14,HEIGHT-25,status_line,MUTED,WIDTH-12,12);
    if(dialog!=NONE) {
        bool reading=dialog==PREVIEW||dialog==PACKAGE_RESULT;int top=reading?98:176,height=reading?380:202;
        surface.rect(193,top+5,480,height,0xff0c1019);surface.rect(188,top,480,height,0xff283449);
        const char* title=dialog==NEW_FOLDER?"Create folder":dialog==NEW_FILE?"Create file":dialog==RENAME?"Rename item":
            dialog==DELETE_ITEM?"Delete this item?":dialog==PREVIEW?dialog_target:"Package manager";
        surface.text(210,top+16,title,INK,646,18);
        if(reading)multiline(dialog_text,210,top+55,434,13);
        else if(dialog==DELETE_ITEM){multiline(dialog_target,210,top+54,434,2);surface.text(210,top+104,"Files and empty folders only. This cannot be undone.",MUTED,646,12);}
        else {surface.rect(208,top+57,438,34,0xff111925);surface.text(218,top+65,edit_text,BLUE,638);}
        int bottom=top+height-44;
        draw_button({reading?560:454,bottom,90,30,reading?"Close":"Cancel"});
        if(!reading)draw_button({554,bottom,92,30,dialog==DELETE_ITEM?"Delete":"Save"},true);
        if(!reading)surface.text(210,top+height-74,status_line,MUTED,645,12);
    }
    native_call(201,window_id);dirty=false;
}
static void button_action(int id) {
    address_edit=false;
    switch(id){case 0:history_step(-1);break;case 1:history_step(1);break;case 2:navigate("..");break;
    case 3:load_directory();break;case 4:begin_dialog(NEW_FOLDER);break;case 5:begin_dialog(NEW_FILE);break;
    case 6:begin_dialog(RENAME);break;case 7:copy_selection(false);break;case 8:copy_selection(true);break;
    case 9:paste_selection();break;case 10:begin_dialog(DELETE_ITEM);break;case 11:package_action(true);break;
    case 12:package_action(false);break;case 13:show_hidden=!show_hidden;load_directory();break;}dirty=true;
}
static void edit_key(char* buffer,unsigned capacity,uint32_t key) {
    unsigned n=native_length(buffer);
    if(key==8||key==127){if(edit_replace)buffer[0]=0;else if(n)buffer[n-1]=0;edit_replace=false;}
    else if(key>=32&&key<=126){if(edit_replace){n=0;buffer[0]=0;}if(n+1<capacity){buffer[n]=(char)key;buffer[n+1]=0;}edit_replace=false;}
    dirty=true;
}
static void handle_event(const NativeEvent& event) {
    if(event.type==3){int next=-1;if(dialog==NONE)for(int i=0;i<14;++i)
            if(hit(event.x,event.y,buttons[i].x,buttons[i].y,buttons[i].w,buttons[i].h)){next=i;break;}
        if(next!=hovered_button){hovered_button=next;dirty=true;}return;}
    if(event.type==5) {
        uint32_t key=event.key;
        if(dialog!=NONE){if(key==27){dialog=NONE;dirty=true;}else if(key=='\n'||key=='\r')commit_dialog();
            else if(dialog==NEW_FOLDER||dialog==NEW_FILE||dialog==RENAME)edit_key(edit_text,sizeof(edit_text),key);return;}
        if(address_edit){if(key==27){address_edit=false;native_copy(address,sizeof(address),current_dir);dirty=true;}
            else if(key=='\n'||key=='\r')navigate(address);else edit_key(address,sizeof(address),key);return;}
        if((event.mods&1)&&(key=='c'||key=='C'))copy_selection(false);
        else if((event.mods&1)&&(key=='x'||key=='X'))copy_selection(true);
        else if((event.mods&1)&&(key=='v'||key=='V'))paste_selection();
        else if((event.mods&1)&&(key=='l'||key=='L')){address_edit=true;edit_replace=true;dirty=true;}
        else if((event.mods&4)&&key==0x1044)history_step(-1);
        else if((event.mods&4)&&key==0x1043)history_step(1);
        else if(key==0x1041)select_row(selected<0?0:selected-1);
        else if(key==0x1042)select_row(selected<0?0:selected+1);
        else if(key=='\n'||key=='\r')open_selected();
        else if(key==8||key==127)navigate("..");
    } else if(event.type==4&&dialog==NONE){scroll_row-=event.delta*3;if(scroll_row<0)scroll_row=0;
        int max=entry_count-VISIBLE;if(max<0)max=0;if(scroll_row>max)scroll_row=max;dirty=true;}
    else if(event.type==1&&event.key==1){int x=event.x,y=event.y;
        if(dialog!=NONE){bool reading=dialog==PREVIEW||dialog==PACKAGE_RESULT;int top=reading?98:176,h=reading?380:202;
            if(hit(x,y,reading?560:454,top+h-44,90,30)){dialog=NONE;dirty=true;}
            else if(!reading&&hit(x,y,554,top+h-44,92,30))commit_dialog();return;}
        if(hit(x,y,204,12,638,30)){address_edit=true;edit_replace=true;dirty=true;return;}
        for(int i=0;i<14;++i)if(hit(x,y,buttons[i].x,buttons[i].y,buttons[i].w,buttons[i].h)){if(button_enabled(i))button_action(i);return;}
        const char* paths[]={"/home","/home/Applications","/share/samples","/apps","/","/mnt"};
        for(int i=0;i<6;++i)if(hit(x,y,8,101+i*43,148,36)){navigate(paths[i]);return;}
        address_edit=false;
        if(hit(x,y,SIDE,LIST_TOP,WIDTH-SIDE,VISIBLE*ROW)){int index=scroll_row+(y-LIST_TOP)/ROW;
            if(index<entry_count){bool twice=last_click==index&&frame-last_click_frame<=30;selected=index;
                last_click=index;last_click_frame=frame;dirty=true;
                if(wiz_name_is_package(entries[selected].name))status("Double-click runs this app. Install adds it to Start; Pin keeps it in this location.");
                if(twice){last_click=-1;open_selected();}}}
    }
}
#ifndef NATIVE_HOST_TEST
extern "C" void _start() {
    window_id=native_call(200,(uint64_t)"File Explorer",130,100,WIDTH,HEIGHT);
    if(!window_id){native_call(60,1);for(;;){}}
    surface={(uint32_t*)native_call(203,window_id),WIDTH,HEIGHT};
    if(!surface.pixels){native_call(60,1);for(;;){}}
    if(!navigate("/home"))navigate("/");
    for(;;){NativeEvent event{};unsigned processed=0;
        while(processed++<64&&native_call(205,window_id,(uint64_t)&event)){
            if(event.type==7){native_call(60,0);for(;;){}}handle_event(event);}
        if(dirty)render();++frame;native_call(35,16);
    }
}
#endif
