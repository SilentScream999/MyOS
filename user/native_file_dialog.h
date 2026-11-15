#ifndef WYZAR_NATIVE_FILE_DIALOG_H
#define WYZAR_NATIVE_FILE_DIALOG_H
#include "native_ui.h"

// Explorer-style common file picker. The owning app processes its events while
// suspending document input; filesystem work happens outside the renderer.
struct NativeFileDialog {
    struct Entry { char name[256];uint64_t size,type; } entries[512];
    NativeSurface canvas{};
    uint64_t window=0;
    char directory[256]="/home",address[256],filename[256],result[256],message[192];
    int count=0,selected=-1,scroll=0,last_click=-1;
    unsigned last_frame=0;
    enum Focus { LIST, NAME, ADDRESS } focus=NAME;
    bool saving=false,replace_input=true,dirty=true,text_only=false,truncated=false;
    enum Result { WAIT, ACCEPT, CANCEL };
    static bool equal(const char* a,const char* b){while(*a&&*a==*b){++a;++b;}return *a==*b;}
    static int compare(const char* a,const char* b){while(*a&&*b){unsigned x=*a++,y=*b++;
        if(x>='A'&&x<='Z')x+=32;if(y>='A'&&y<='Z')y+=32;if(x!=y)return x<y?-1:1;}return *a?1:*b?-1:0;}
    static bool hit(int x,int y,int rx,int ry,int w,int h){return x>=rx&&x<rx+w&&y>=ry&&y<ry+h;}
    static bool absolute(const char* base,const char* value,char* output){
        char raw[512];unsigned used=0;
        if(value[0]!='/'){unsigned n=native_length(base);if(n+1>=sizeof(raw))return false;
            memcpy(raw,base,n);used=n;if(used&&raw[used-1]!='/')raw[used++]='/';}
        unsigned n=native_length(value);if(used+n>=sizeof(raw))return false;memcpy(raw+used,value,n+1);
        unsigned at=0;output[at++]='/';const char* p=raw;
        while(*p){while(*p=='/')++p;const char* part=p;while(*p&&*p!='/')++p;unsigned len=(unsigned)(p-part);
            if(!len||(len==1&&part[0]=='.'))continue;
            if(len==2&&part[0]=='.'&&part[1]=='.'){while(at>1&&output[at-1]!='/')--at;if(at>1)--at;continue;}
            unsigned extra=at>1?1:0;if(at+extra+len>=256)return false;
            if(extra)output[at++]='/';for(unsigned i=0;i<len;++i){if((unsigned char)part[i]<32)return false;output[at++]=part[i];}}
        output[at]=0;return true;
    }
    void tell(const char* text){native_copy(message,sizeof(message),text);dirty=true;}
    int visible()const{int n=(canvas.h-206)/30;return n>0?n:1;}
    void bound_scroll(){int maximum=count-visible();if(maximum<0)maximum=0;if(scroll<0)scroll=0;if(scroll>maximum)scroll=maximum;}
    bool navigate(const char* value){
        char target[256];file_info info{};
        if(!absolute(directory,value,target)){tell("The folder path is too long or invalid.");return false;}
        if(sys_stat(target,&info)!=0||info.type!=2){tell("Cannot open that folder. Check the path and storage.");return false;}
        native_copy(directory,sizeof(directory),target);native_copy(address,sizeof(address),target);
        count=0;selected=-1;scroll=0;last_click=-1;truncated=false;bool failed=false;uint64_t i=0;
        for(;i<8192;++i){char name[256];uint64_t size,type;
            int64_t rc=sys_readdir(directory,i,name,&size,&type);if(rc<=0){failed=rc<0;break;}
            if(name[0]=='.')continue;
            if(text_only&&type!=2){const char* ext=name;for(const char* p=name;*p;++p)if(*p=='.')ext=p;if(compare(ext,".txt"))continue;}
            if(count==512){truncated=true;break;}
            Entry value{};native_copy(value.name,256,name);value.size=size;value.type=type;
            int at=count++;while(at&&((type==2&&entries[at-1].type!=2)||
                ((type==2)==(entries[at-1].type==2)&&compare(name,entries[at-1].name)<0))){entries[at]=entries[at-1];--at;}entries[at]=value;}
        if(i==8192)truncated=true;
        tell(failed?"Folder read failed. Try another folder or refresh.":truncated?"Showing the first 512 items.":"Select a file or browse to another folder.");return true;
    }
    bool show(bool save,const char* initial){
        saving=save;focus=NAME;replace_input=true;text_only=false;filename[0]=0;
        char initial_dir[256];native_copy(initial_dir,256,initial&&initial[0]?initial:"/home/");
        unsigned end=native_length(initial_dir);while(end&&initial_dir[end-1]!='/')--end;
        if(end){native_copy(filename,256,initial_dir+end);initial_dir[end]=0;}else native_copy(initial_dir,256,"/home");
        window=native_call(200,(uint64_t)(save?"Save As":"Open"),210,150,720,460);
        if(!window||window==(uint64_t)-1){window=0;return false;}
        // A common dialog belongs to the caller but has the system's Explorer
        // identity in both its title bar and taskbar, for every application.
        if((int64_t)native_call(209,window,1)!=0){hide();return false;}
        canvas={(uint32_t*)native_call(203,window),720,460};
        if(!canvas.pixels||canvas.pixels==(uint32_t*)-1){hide();return false;}
        native_enable_resize(window,560,360,canvas);
        if(!navigate(initial_dir)&&!navigate("/home"))navigate("/");dirty=true;return true;
    }
    void hide(){if(window)native_call(202,window);window=0;canvas={};}
    void select(int index){if(index<0||index>=count)return;selected=index;
        if(entries[index].type!=2)native_copy(filename,256,entries[index].name);
        if(index<scroll)scroll=index;if(index>=scroll+visible())scroll=index-visible()+1;dirty=true;}
    Result submit(){
        if(focus==ADDRESS){if(navigate(address)){focus=LIST;replace_input=true;}return WAIT;}
        if(focus==LIST&&selected>=0&&entries[selected].type==2){navigate(entries[selected].name);return WAIT;}
        if(!filename[0]){tell("Choose a file name first.");return WAIT;}
        if(!absolute(directory,filename,result)){tell("The file path is too long or invalid.");return WAIT;}
        file_info info{};int64_t rc=sys_stat(result,&info);
        if(rc==0&&info.type==2){navigate(result);return WAIT;}
        if(!saving&&(rc!=0||info.type!=1)){tell("That file does not exist. Select a file from the list.");return WAIT;}
        return ACCEPT;
    }
    void draw_button(int x,int y,int w,const char* label){canvas.rounded(x,y,w,30,5,0xff403451);canvas.centered_text(x,y,w,30,label,0xffeeebf8,12);}
    void render(){
        int w=canvas.w,h=canvas.h;if(!canvas.pixels)return;
        canvas.rect(0,0,w,h,0xff1a1925);canvas.rect(0,0,w,58,0xff222131);
        draw_button(12,14,36,"Up");draw_button(56,14,64,"Refresh");
        canvas.rounded(132,14,w-148,30,4,focus==ADDRESS?0xff443454:0xff14121e);
        canvas.text(142,21,address,0xffeeebf8,w-28,12);
        canvas.rect(0,58,140,h-58,0xff222131);canvas.text(16,76,"PLACES",0xffa49bb6,132,12);
        const char* places[]={"Home","Applications","System","Devices"};
        for(int i=0;i<4;++i){canvas.folder(12,104+i*40,24);canvas.text(43,109+i*40,places[i],0xffeeebf8,134,12);}
        canvas.rect(148,66,w-164,26,0xff222131);canvas.text(184,71,"Name",0xffa49bb6,w-150,12);canvas.text(w-130,71,"Type",0xffa49bb6,w-20,12);
        for(int row=0;row<visible();++row){int index=scroll+row;if(index>=count)break;int y=96+row*30;
            if(index==selected)canvas.rounded(148,y,w-164,30,4,0xff443454);
            if(entries[index].type==2)canvas.folder(154,y+2,26);else canvas.document(154,y+1);
            canvas.text(188,y+7,entries[index].name,0xffeeebf8,w-142,12);
            canvas.text(w-130,y+7,entries[index].type==2?"Folder":"File",0xffa49bb6,w-20,12);}
        if(!count)canvas.text(178,112,"No files in this folder.",0xffa49bb6,w-20,12);
        if(count>visible()){int track=visible()*30,thumb=track*visible()/count;if(thumb<16)thumb=16;
            canvas.rect(w-8,96,3,track,0xff39334b);canvas.rect(w-8,96+(track-thumb)*scroll/(count-visible()),3,thumb,0xffa58bc5);}
        canvas.rect(140,h-106,w-140,106,0xff222131);canvas.text(154,h-94,"File name:",0xffeeebf8,236,12);
        canvas.rounded(240,h-100,w-256,30,4,focus==NAME?0xff443454:0xff14121e);
        canvas.text(250,h-93,filename,0xffeeebf8,w-26,12);
        draw_button(154,h-58,180,text_only?"Text files (*.txt)":"All files (*.*)");
        draw_button(w-200,h-58,86,saving?"Save":"Open");draw_button(w-102,h-58,86,"Cancel");
        canvas.text(152,h-20,message,0xffb9aecb,w-12,12);
        native_call(201,window);dirty=false;
    }
    Result event(const NativeEvent& e,unsigned frame){
        if(e.type==7)return CANCEL;
        if(e.type==8){if(native_accept_resize(window,canvas)){bound_scroll();dirty=true;}return WAIT;}
        if(e.type==5){uint32_t key=e.key;
            if(key==27)return CANCEL;
            if((e.mods&1)&&(key=='l'||key=='L')){focus=ADDRESS;replace_input=true;dirty=true;return WAIT;}
            if((e.mods&4)&&key==0x1041){navigate("..");return WAIT;}
            if(key=='\t'){focus=focus==NAME?LIST:focus==LIST?ADDRESS:NAME;replace_input=true;dirty=true;return WAIT;}
            if(key=='\n'||key=='\r')return submit();
            if(focus==LIST){if(key==0x1041)select(selected<0?0:selected-1);else if(key==0x1042)select(selected+1);else if(key==8)navigate("..");return WAIT;}
            char* buffer=focus==NAME?filename:address;unsigned n=native_length(buffer);
            if((e.mods&1)&&(key=='v'||key=='V')){
                char pasted[256];int64_t size=native_clipboard_get(pasted,sizeof(pasted));
                if(size<0){tell("Clipboard text is too long or unavailable.");return WAIT;}
                for(int64_t i=0;i<size;++i)if((unsigned char)pasted[i]<32){tell("Paste a single file or folder path.");return WAIT;}
                if(replace_input)n=0;if(n+(unsigned)size>=256){tell("The pasted path is too long.");return WAIT;}
                memcpy(buffer+n,pasted,(unsigned)size+1);replace_input=false;dirty=true;return WAIT;
            }
            if((e.mods&1)&&(key=='a'||key=='A')){replace_input=true;dirty=true;return WAIT;}
            if(key==8||key==127){if(replace_input)buffer[0]=0;else if(n)buffer[n-1]=0;replace_input=false;}
            else if(!(e.mods&1)&&key>=32&&key<=126){if(replace_input)n=0;if(n+1<256){buffer[n]=(char)key;buffer[n+1]=0;}replace_input=false;}
            dirty=true;return WAIT;
        }
        if(e.type==4){scroll-=e.delta*3;bound_scroll();dirty=true;return WAIT;}
        if(e.type!=1||e.key!=1)return WAIT;
        int x=e.x,y=e.y,w=canvas.w,h=canvas.h;
        if(hit(x,y,w-102,h-58,86,30))return CANCEL;
        if(hit(x,y,w-200,h-58,86,30))return submit();
        if(hit(x,y,154,h-58,180,30)){text_only=!text_only;navigate(directory);return WAIT;}
        if(hit(x,y,12,14,36,30)){navigate("..");return WAIT;}
        if(hit(x,y,56,14,64,30)){navigate(directory);return WAIT;}
        if(hit(x,y,132,14,w-148,30)){focus=ADDRESS;replace_input=true;dirty=true;return WAIT;}
        if(hit(x,y,240,h-100,w-256,30)){focus=NAME;replace_input=true;dirty=true;return WAIT;}
        const char* places[]={"/home","/home/Applications","/","/mnt"};
        for(int i=0;i<4;++i)if(hit(x,y,8,100+i*40,126,36)){navigate(places[i]);return WAIT;}
        if(hit(x,y,148,96,w-164,visible()*30)){int index=scroll+(y-96)/30;if(index<count){
            bool twice=last_click==index&&frame-last_frame<=30;select(index);focus=LIST;last_click=index;last_frame=frame;
            if(twice){last_click=-1;return submit();}}}
        return WAIT;
    }
};
#endif
