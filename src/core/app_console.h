#ifndef WYZAR_APP_CONSOLE_H
#define WYZAR_APP_CONSOLE_H
#include "tty.h"

// Independent byte channels for desktop console apps. The userspace terminal
// owns presentation; the kernel owns routing, blocking and channel lifetime.
struct AppConsole {
    unsigned refs, input_head,input_tail,output_head,output_tail,line_len;
    bool raw,echo,eof;
    pid_t foreground;
    char input[4096],output[32768],line[4096];
};
static void console_wake(AppConsole* c) {
    Task* t=g_task_list;if(t)do{
        if(t->console==c&&t->waiting_tty_input&&t->state==TaskState::BLOCKED){t->waiting_tty_input=false;t->state=TaskState::READY;t->wake_at_tick=0;}
        t=t->next;
    }while(t!=g_task_list);
}
static void console_output(AppConsole* c,const char* bytes,uint64_t size) {
    TtyIrqGuard guard;
    if(size>=sizeof(c->output)){bytes+=size-(sizeof(c->output)-1);size=sizeof(c->output)-1;}
    for(uint64_t i=0;i<size;++i){unsigned next=(c->output_head+1)%sizeof(c->output);
        if(next==c->output_tail)c->output_tail=(c->output_tail+1)%sizeof(c->output);
        c->output[c->output_head]=bytes[i];c->output_head=next;}
}
static void console_release(Task* task) {
    auto* c=(AppConsole*)task->console;if(!c)return;
    TtyIrqGuard guard;
    if(task->console_host){c->eof=true;Task* other=g_task_list;
        if(other)do{if(other!=task&&other->console==c)task_request_interrupt(other->pid);other=other->next;}while(other!=g_task_list);
        console_wake(c);
    }
    task->console=nullptr;if(--c->refs==0)kfree(c);
}
static uint64_t console_read(AppConsole* c,char* out,uint64_t capacity) {
    TtyIrqGuard guard;uint64_t count=0;
    while(count<capacity){
        if(g_current_task->interrupt_requested||(c->eof&&c->input_head==c->input_tail))break;
        if(c->input_head!=c->input_tail){char ch=c->input[c->input_tail];c->input_tail=(c->input_tail+1)%sizeof(c->input);
            out[count++]=ch;if(ch=='\n'||c->raw)break;
        }else{if(count)break;g_current_task->waiting_tty_input=true;g_current_task->state=TaskState::BLOCKED;g_current_task->wake_at_tick=0;
            while(g_current_task->state==TaskState::BLOCKED){schedule();if(g_current_task->state==TaskState::BLOCKED)scheduler_idle_halt();}
            g_current_task->waiting_tty_input=false;
        }
    }return count;
}
static void console_input(AppConsole* c,char ch) {
    TtyIrqGuard guard;if(c->eof)return;
    if(c->raw){unsigned next=(c->input_head+1)%sizeof(c->input);if(next!=c->input_tail){c->input[c->input_head]=ch;c->input_head=next;console_wake(c);}return;}
    if(ch=='\r')ch='\n';
    if(ch=='\b'||ch==127){if(c->line_len){--c->line_len;if(c->echo)console_output(c,"\b \b",3);}return;}
    if(c->line_len+1>=sizeof(c->line))return;
    c->line[c->line_len++]=ch;if(c->echo)console_output(c,&ch,1);
    if(ch=='\n'){
        // Publish a complete line, or none if there is insufficient room.
        unsigned available=(c->input_tail+sizeof(c->input)-c->input_head-1)%sizeof(c->input);
        if(available>=c->line_len)for(unsigned i=0;i<c->line_len;++i){c->input[c->input_head]=c->line[i];c->input_head=(c->input_head+1)%sizeof(c->input);}
        c->line_len=0;console_wake(c);
    }
}
// Host-only interface: get target / drain output / submit keys / EOF / cancel.
static uint64_t sys_console(uint64_t action,uint64_t buffer,uint64_t size) {
    if(!g_current_task||!g_current_task->console_host||!g_current_task->console)return (uint64_t)-1;
    auto* c=(AppConsole*)g_current_task->console;TtyIrqGuard guard;
    if(action<=2&&(!buffer||buffer>=USERSPACE_END||size>USERSPACE_END-buffer||size>4096))return (uint64_t)-1;
    if(action==0){size_t n=strlen(g_current_task->launch_target);if(size<=n)return (uint64_t)-1;
        for(size_t i=0;i<=n;++i)((char*)buffer)[i]=g_current_task->launch_target[i];return n;}
    if(action==1){uint64_t n=0;while(n<size&&c->output_tail!=c->output_head){((char*)buffer)[n++]=c->output[c->output_tail];c->output_tail=(c->output_tail+1)%sizeof(c->output);}return n;}
    if(action==2){for(uint64_t i=0;i<size;++i)console_input(c,((char*)buffer)[i]);return size;}
    if(action==3){unsigned available=(c->input_tail+sizeof(c->input)-c->input_head-1)%sizeof(c->input);
        if(available>=c->line_len)for(unsigned i=0;i<c->line_len;++i){c->input[c->input_head]=c->line[i];c->input_head=(c->input_head+1)%sizeof(c->input);}
        c->line_len=0;c->eof=true;console_wake(c);return 0;}
    if(action==4)return task_request_interrupt(c->foreground)?0:(uint64_t)-1;
    return (uint64_t)-1;
}
#endif
