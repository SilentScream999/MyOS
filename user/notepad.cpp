// Native text editor. Files stay in memory until an atomic VFS replacement succeeds.
#include "user/native_ui.h"
#include "user/native_file_dialog.h"
#include "user/native_text.h"
constexpr unsigned CAP=65536;
static char text[CAP+1], staging[CAP+1], undo_text[CAP+1], clip[CAP+1];
static unsigned length=0,caret=0,anchor=0,undo_length=0,undo_caret=0;
static bool modified=false,dirty=true,undo_ready=false,exiting=false;
static char path[256], field[256], message[192]="Ready";
static NativeSurface surface{};
static uint64_t window_id;
static int top_line=0,left_column=0;
enum Modal { NORMAL, OPEN_PATH, SAVE_PATH, UNSAVED, OVERWRITE, ABOUT };
enum Action { NO_ACTION, NEW_DOCUMENT, OPEN_DOCUMENT, CLOSE_DOCUMENT };
static Modal modal=NORMAL;
static Action pending=NO_ACTION;
static NativeFileDialog picker;
static int menu=-1,menu_item=0;
static bool show_status=true;
static unsigned frame=0;
static NativeTextCursor text_cursor;
constexpr int EDIT_TOP=44;
static void note(const char* s){native_copy(message,sizeof(message),s);dirty=true;}
static unsigned low(){return caret<anchor?caret:anchor;}
static unsigned high(){return caret>anchor?caret:anchor;}
static unsigned line_start(unsigned at){while(at&&text[at-1]!='\n')--at;return at;}
static unsigned line_end(unsigned at){while(at<length&&text[at]!='\n')++at;return at;}
static int rows(){int n=(surface.h-EDIT_TOP-(show_status?28:0))/20;return n>0?n:1;}
static int columns(){int n=(surface.w-44)/9;return n>0?n:1;}
static void reveal(){
    int row=0;for(unsigned i=0;i<caret;++i)if(text[i]=='\n')++row;
    int column=(int)(caret-line_start(caret));
    if(row<top_line)top_line=row;if(row>=top_line+rows())top_line=row-rows()+1;
    if(column<left_column)left_column=column;if(column>=left_column+columns())left_column=column-columns()+1;
    dirty=true;
}
static void snapshot(){memcpy(undo_text,text,length+1);undo_length=length;undo_caret=caret;undo_ready=true;}
static void insert(const char* value,unsigned count){
    unsigned first=low(),last=high();
    if(length-(last-first)+count>CAP){note("Document limit is 64 KiB.");return;}
    snapshot();
    if(count>last-first){for(unsigned i=length+1;i>last;--i)text[i-1-(last-first)+count]=text[i-1];}
    else {for(unsigned i=last;i<=length;++i)text[first+count+i-last]=text[i];}
    for(unsigned i=0;i<count;++i)text[first+i]=value[i];
    length=length-(last-first)+count;caret=anchor=first+count;modified=true;reveal();
}
static bool load(const char* candidate){
    if(candidate[0]!='/'){note("Enter an absolute file path, such as /home/notes.txt.");return false;}
    file_info info{};
    if(sys_stat(candidate,&info)!=0||info.type!=1){note("Cannot open that file.");return false;}
    if(info.size>CAP){note("File exceeds the 64 KiB document limit.");return false;}
    uint64_t fd=sys_open(candidate,0);if(fd==(uint64_t)-1){note("Cannot read that file.");return false;}
    unsigned done=0;bool ok=true;
    while(done<info.size){uint64_t n=sys_read(fd,staging+done,info.size-done);if(!n||n>info.size-done){ok=false;break;}done+=(unsigned)n;}
    sys_close(fd);
    if(!ok){note("Read failed. Current document was kept.");return false;}
    for(unsigned i=0;i<done;++i){unsigned char c=staging[i];
        if((c<32&&c!='\n'&&c!='\r'&&c!='\t')||c>126){note("Only ASCII text is supported; this file was left unchanged.");return false;}}
    staging[done]=0;memcpy(text,staging,done+1);length=done;caret=anchor=0;
    native_copy(path,sizeof(path),candidate);top_line=left_column=0;modified=undo_ready=false;note("File opened.");return true;
}
static void start_picker(bool saving,const char* initial){
    menu=-1;modal=saving?SAVE_PATH:OPEN_PATH;
    if(!picker.show(saving,initial)){modal=NORMAL;pending=NO_ACTION;note("Could not create the file browser window.");}dirty=true;
}
static void perform_pending(){
    Action action=pending;pending=NO_ACTION;modal=NORMAL;
    if(action==CLOSE_DOCUMENT)exiting=true;
    else if(action==NEW_DOCUMENT){length=caret=anchor=0;text[0]=path[0]=0;modified=undo_ready=false;top_line=left_column=0;note("New document.");}
    else if(action==OPEN_DOCUMENT)start_picker(false,path[0]?path:"/home/");
    dirty=true;
}
static void request(Action action){picker.hide();menu=-1;pending=action;if(modified){modal=UNSAVED;dirty=true;}else perform_pending();}
static bool save_to(const char* candidate){
    if(candidate[0]!='/'){note("Enter an absolute file path, such as /home/notes.txt.");return false;}
    if(sys_replace(candidate,text,length)!=0){note("Save failed. Your edits are still in memory.");if(picker.window)picker.tell(message);return false;}
    native_copy(path,sizeof(path),candidate);modified=false;picker.hide();modal=NORMAL;note("Saved.");
    if(pending!=NO_ACTION)perform_pending();return true;
}
static void save(bool as=false){
    if(!as&&path[0]){save_to(path);return;}
    native_copy(field,sizeof(field),path[0]?path:"/home/Untitled.txt");start_picker(true,field);
}
static void submit_path(){
    if(modal==OPEN_PATH){if(load(field)){picker.hide();modal=NORMAL;pending=NO_ACTION;}else if(picker.window)picker.tell(message);}
    else if(modal==SAVE_PATH){file_info existing{};
        if(sys_stat(field,&existing)==0){if(existing.type!=1){note("That path is a folder.");if(picker.window)picker.tell(message);return;}picker.hide();modal=OVERWRITE;dirty=true;}
        else save_to(field);}
}
static bool hit(int x,int y,int rx,int ry,int w,int h){return x>=rx&&x<rx+w&&y>=ry&&y<ry+h;}
static void button(int x,int y,int w,const char* label){surface.rounded(x,y,w,30,5,0xff403451);surface.centered_text(x,y,w,30,label,0xffeeebf8,12);}
struct MenuItem {const char* label;const char* shortcut;uint32_t key;};
static const MenuItem file_menu[]={{"New","Ctrl+N",'n'},{"Open...","Ctrl+O",'o'},{"Save","Ctrl+S",'s'},
    {"Save As...","Ctrl+Shift+S",'S'},{"Exit","",0}};
static const MenuItem edit_menu[]={{"Undo","Ctrl+Z",'z'},{"Cut","Ctrl+X",'x'},{"Copy","Ctrl+C",'c'},
    {"Paste","Ctrl+V",'v'},{"Delete","Del",0x1102},{"Select All","Ctrl+A",'a'}};
static const MenuItem view_menu[]={{"Status Bar","",0}};
static const MenuItem help_menu[]={{"About Notepad","",0}};
static int menu_count(){return menu==0?5:menu==1?6:1;}
static const MenuItem* menu_entries(){return menu==0?file_menu:menu==1?edit_menu:menu==2?view_menu:help_menu;}
static bool menu_enabled(int item){
    if(menu!=1)return true;if(item==0)return undo_ready;
    if(item==1||item==2)return caret!=anchor;if(item==3)return native_clipboard_get(clip,sizeof(clip))>0;
    if(item==4)return caret!=anchor||caret<length;return length>0;
}
static void key_event(uint32_t key,uint32_t mods);
static void menu_execute(){
    if(menu<0||!menu_enabled(menu_item))return;
    int current=menu;MenuItem command=menu_entries()[menu_item];menu=-1;dirty=true;
    if(current==0&&command.key==0)request(CLOSE_DOCUMENT);
    else if(current==2){show_status=!show_status;reveal();}
    else if(current==3){modal=ABOUT;dirty=true;}
    else if(command.key==0x1102)key_event(command.key,0);
    else key_event(command.key,command.key=='S'?3:1);
}
static void render_menu(){
    const char* labels[]={"File","Edit","View","Help"};
    for(int i=0;i<4;++i){if(menu==i)surface.rect(4+i*56,2,56,28,0xff443454);surface.centered_text(4+i*56,2,56,28,labels[i],0xffeeebf8,14);}
    if(menu<0)return;
    int x=4+menu*56,h=menu_count()*30+12;
    surface.rounded(x+3,35,270,h,5,0xff0d0b14);surface.rounded(x,32,270,h,5,0xff332a42);
    for(int i=0;i<menu_count();++i){bool enabled=menu_enabled(i);
        if(i==menu_item&&enabled)surface.rounded(x+4,38+i*30,262,30,4,0xff514063);
        uint32_t ink=enabled?0xffeeebf8:0xff82788f;
        const auto& item=menu_entries()[i];
        surface.text(x+24,45+i*30,item.label,ink,x+164,12);
        int shortcut=ui_text_width(ui_font_for_pixels(12),item.shortcut);
        surface.text(x+256-shortcut,45+i*30,item.shortcut,ink,x+262,12);
        if(menu==2&&show_status)surface.text(x+8,45+i*30,"*",0xffc9b5ff,x+23,12);
    }
}
static void fit_tail(const char* value,char* output,int width){
    const UiFont* font=ui_font_for_pixels(12);
    if(ui_text_width(font,value)<=width){native_copy(output,260,value);return;}
    int dots=ui_text_width(font,"...");while(*value&&ui_text_width(font,value)+dots>width)++value;
    output[0]=output[1]=output[2]='.';native_copy(output+3,257,value);
}
static void render(){
    surface.rect(0,0,surface.w,surface.h,0xff1a1925);surface.rect(0,0,surface.w,34,0xff222131);
    const char* name=path[0]?path:"Untitled";for(const char* p=name;*p;++p)if(*p=='/')name=p+1;
    char caption[260];fit_tail(name,caption,surface.w-280);
    int caption_width=ui_text_width(ui_font_for_pixels(12),caption);
    surface.text(surface.w-16-caption_width,9,caption,0xffb9aecb,surface.w-16,12);
    if(modified)surface.text(surface.w-30-caption_width,9,"*",0xffe9bd70,surface.w-16,12);
    unsigned start=0;int row=0;while(start<length&&row<top_line)if(text[start++]=='\n')++row;
    for(int r=0;r<rows();++r){unsigned end=line_end(start);
        for(unsigned i=start;i<=end;++i){int col=(int)(i-start)-left_column;if(col<0||col>=columns())continue;
            int x=20+col*9,y=EDIT_TOP+r*20;
            if(i>=low()&&i<high())surface.rect(x,y,9,20,0xff574274);
            if(i<end){char glyph[2]={text[i]=='\t'||text[i]=='\r'?' ':text[i],0};surface.text(x,y+2,glyph,0xffeeebf8,x+9,12);}
            if(i==caret&&modal==NORMAL&&menu<0)text_cursor.draw(surface,x,y,19);
        }
        if(end==length)break;start=end+1;
    }
    if(show_status){surface.rect(0,surface.h-28,surface.w,28,0xff222131);
        surface.text(12,surface.h-22,message,0xffb9aecb,surface.w-12,12);
        int start=ui_text_width(ui_font_for_pixels(12),message)+36,available=surface.w-start-16;
        if(path[0]&&available>160){char location[260];fit_tail(path,location,available);
            int width=ui_text_width(ui_font_for_pixels(12),location);
            surface.text(surface.w-16-width,surface.h-22,location,0xff968ba8,surface.w-16,12);}}
    render_menu();
    if(modal==UNSAVED||modal==OVERWRITE||modal==ABOUT){int x=(surface.w-440)/2,y=(surface.h-174)/2;
        surface.rounded(x,y,440,174,8,0xff393047);
        const char* title=modal==UNSAVED?"Save changes?":modal==ABOUT?"About Notepad":"Replace this existing file?";
        surface.text(x+18,y+16,title,0xffeeebf8,x+422,18);
        surface.text(x+18,y+58,modal==UNSAVED?"Your document has unsaved changes.":modal==ABOUT?"Notepad for WyzarOS":field,0xffc9b5ff,x+422,12);
        surface.text(x+18,y+92,modal==ABOUT?"ASCII text editor | 64 KiB documents":message,0xffb9aecb,x+422,12);
        if(modal!=ABOUT)button(x+18,y+126,86,"Cancel");
        if(modal==UNSAVED){button(x+222,y+126,96,"Discard");button(x+328,y+126,94,"Save");}
        else button(x+328,y+126,94,modal==ABOUT?"OK":"Replace");
    }
    native_call(201,window_id);dirty=false;
}
static void cancel(){
    if(modal==OVERWRITE){start_picker(true,field);return;}
    picker.hide();modal=NORMAL;pending=NO_ACTION;dirty=true;
}
static void picker_event(const NativeEvent& event){
    auto result=picker.event(event,frame);
    if(result==NativeFileDialog::CANCEL)cancel();
    else if(result==NativeFileDialog::ACCEPT){native_copy(field,sizeof(field),picker.result);submit_path();}
}
static void key_event(uint32_t key,uint32_t mods){
    text_cursor.reset(frame);dirty=true;
    bool ctrl=mods&1,shift=mods&2;
    if(modal==NORMAL&&(mods&4)){
        if(key=='f'||key=='F')menu=0;else if(key=='e'||key=='E')menu=1;else if(key=='v'||key=='V')menu=2;else if(key=='h'||key=='H')menu=3;
        else return;menu_item=0;dirty=true;return;
    }
    if(modal==NORMAL&&menu>=0){
        if(key==27)menu=-1;
        else if(key==0x1042)menu_item=(menu_item+1)%menu_count();
        else if(key==0x1041)menu_item=(menu_item+menu_count()-1)%menu_count();
        else if(key==0x1043||key==0x1044){menu=(menu+(key==0x1043?1:3))%4;menu_item=0;}
        else if(key=='\n'||key=='\r')menu_execute();
        else if(ctrl){menu=-1;key_event(key,mods);}dirty=true;return;
    }
    if(modal==OPEN_PATH||modal==SAVE_PATH){picker_event({5,0,0,key,mods,0});return;}
    if(modal!=NORMAL){if(key==27){cancel();return;}
        if(modal==ABOUT){if(key=='\n'||key=='\r')cancel();return;}
        if(key=='\n'||key=='\r'){if(modal==UNSAVED)save();else if(modal==OVERWRITE)save_to(field);else submit_path();return;}
        return;}
    if(ctrl&&key==0x1100){caret=0;if(!shift)anchor=caret;reveal();return;}
    if(ctrl&&key==0x1101){caret=length;if(!shift)anchor=caret;reveal();return;}
    if(ctrl){
        if(key=='s'||key=='S')save(shift);
        else if(key=='o'||key=='O')request(OPEN_DOCUMENT);
        else if(key=='n'||key=='N')request(NEW_DOCUMENT);
        else if(key=='a'||key=='A'){anchor=0;caret=length;reveal();}
        else if(key=='c'||key=='C'||key=='x'||key=='X'){unsigned n=high()-low();if(n){
            if(!native_clipboard_set(text+low(),n)){note("Could not copy text.");return;}if(key=='x'||key=='X')insert("",0);}}
        else if(key=='v'||key=='V'){int64_t n=native_clipboard_get(clip,sizeof(clip));if(n<0){note("Could not read clipboard text.");return;}
            for(int64_t i=0;i<n;++i){unsigned char c=clip[i];if((c<32&&c!='\n'&&c!='\r'&&c!='\t')||c>126){note("Clipboard contains unsupported text.");return;}}
            if(n)insert(clip,(unsigned)n);}
        else if((key=='z'||key=='Z')&&undo_ready){memcpy(text,undo_text,undo_length+1);length=undo_length;caret=anchor=undo_caret;modified=true;undo_ready=false;reveal();}
        return;
    }
    unsigned target=caret;bool move=true;
    if(key==0x1044){if(!shift&&caret!=anchor)target=low();else if(target)--target;}
    else if(key==0x1043){if(!shift&&caret!=anchor)target=high();else if(target<length)++target;}
    else if(key==0x1041){unsigned start=line_start(caret),col=caret-start;if(start){unsigned prev=line_start(start-1),end=start-1;target=prev+col;if(target>end)target=end;}}
    else if(key==0x1042){unsigned col=caret-line_start(caret),end=line_end(caret);if(end<length){unsigned next=end+1,last=line_end(next);target=next+col;if(target>last)target=last;}}
    else if(key==0x1100)target=line_start(caret);
    else if(key==0x1101)target=line_end(caret);
    else move=false;
    if(move){caret=target;if(!shift)anchor=caret;reveal();return;}
    if(key==8||key==127){if(caret==anchor&&caret)anchor=caret-1;if(caret!=anchor)insert("",0);}
    else if(key==0x1102){if(caret==anchor&&caret<length)anchor=caret+1;if(caret!=anchor)insert("",0);}
    else if(key=='\r'||key=='\n')insert("\n",1);
    else if(key=='\t'||(key>=32&&key<=126)){char c=(char)key;insert(&c,1);}
}
static void handle_event(const NativeEvent& e){
    if(e.type==7){request(CLOSE_DOCUMENT);return;}
    if(e.type==8){if(native_accept_resize(window_id,surface))reveal();return;}
    if(e.type==5){key_event(e.key,e.mods);return;}
    if(e.type==3&&modal==NORMAL&&menu>=0){
        if(e.y>=2&&e.y<30&&e.x>=4&&e.x<228){int next=(e.x-4)/56;if(menu!=next){menu=next;menu_item=0;dirty=true;}}
        else if(hit(e.x,e.y,4+menu*56,38,270,menu_count()*30)){int next=(e.y-38)/30;if(next!=menu_item){menu_item=next;dirty=true;}}return;}
    if(e.type==4&&modal==NORMAL){top_line-=e.delta*3;if(top_line<0)top_line=0;
        int lines=0;for(unsigned i=0;i<length;++i)if(text[i]=='\n')++lines;if(top_line>lines)top_line=lines;dirty=true;return;}
    if(e.type!=1||e.key!=1)return;
    if(modal==OPEN_PATH||modal==SAVE_PATH)return;
    if(modal!=NORMAL){int x=(surface.w-440)/2,y=(surface.h-174)/2;
        if(hit(e.x,e.y,x+18,y+126,86,30))cancel();
        else if(modal==UNSAVED&&hit(e.x,e.y,x+222,y+126,96,30))perform_pending();
        else if(hit(e.x,e.y,x+328,y+126,94,30)){if(modal==UNSAVED)save();else if(modal==OVERWRITE)save_to(field);else cancel();}return;}
    if(e.y>=2&&e.y<30&&e.x>=4&&e.x<228){int next=(e.x-4)/56;menu=menu==next?-1:next;menu_item=0;dirty=true;return;}
    if(menu>=0){int x=4+menu*56;if(hit(e.x,e.y,x,38,270,menu_count()*30)){menu_item=(e.y-38)/30;menu_execute();}
        else {menu=-1;dirty=true;}return;}
    if(e.y>=EDIT_TOP&&e.y<EDIT_TOP+rows()*20){int row=top_line+(e.y-EDIT_TOP)/20,col=left_column+(e.x>20?(e.x-20)/9:0);unsigned at=0;
        while(at<length&&row)if(text[at++]=='\n')--row;
        unsigned end=line_end(at);caret=at+(unsigned)col;if(caret>end)caret=end;if(!(e.mods&2))anchor=caret;text_cursor.reset(frame);reveal();}
}
#ifndef NATIVE_HOST_TEST
extern "C" void _start(){
    window_id=native_call(200,(uint64_t)"Notepad",170,120,760,520);
    if(!window_id){native_call(60,1);for(;;){}}
    surface={(uint32_t*)native_call(203,window_id),760,520};
    if(!surface.pixels){native_call(60,1);for(;;){}}
    native_enable_resize(window_id,480,300,surface);
    char document[256]{};if((int64_t)native_call(215,(uint64_t)document,sizeof(document))>0)load(document);
    while(!exiting){NativeEvent e{};unsigned n=0;while(n++<64&&native_call(205,window_id,(uint64_t)&e))handle_event(e);
        if(picker.window){n=0;while(n++<64&&picker.window&&native_call(205,picker.window,(uint64_t)&e))picker_event(e);}
        if(text_cursor.animate(surface,window_id,frame,modal==NORMAL&&menu<0,dirty))dirty=true;
        if(picker.window)picker.tick(frame);
        if(dirty)render();if(picker.window&&picker.dirty)picker.render();++frame;native_call(35,16);}
    native_call(60,0);for(;;){}
}
#endif
