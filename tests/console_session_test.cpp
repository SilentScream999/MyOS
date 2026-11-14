#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#define TERMINAL_H
#define scheduler_h
#define TTY_HOST_TEST
enum class TaskState { READY,RUNNING,BLOCKED,DEAD };
struct Task { unsigned pid;TaskState state;Task* next;uint64_t wake_at_tick=0;bool interrupt_requested=false,waiting_tty_input=false;
    void* console=nullptr;bool console_host=false;char launch_target[256]{}; };
static Task *g_task_list,*g_current_task;
static unsigned g_terminal_shell_pid=1,g_terminal_foreground_pid=0;
static unsigned g_term_view_offset,freed;
static constexpr uint64_t USERSPACE_END=UINT64_MAX;
static void term_putchar(char){}static void term_flip(){}static void term_sync_backbuffer(){}static void term_dirty_all(){}
static void term_write(const char*,size_t){}static void term_render_cursor(){}
static void (*on_schedule)();static void schedule(){assert(on_schedule);on_schedule();}
static void scheduler_idle_halt(){assert(false);}
static void kfree(void* p){++freed;free(p);}
static bool task_request_interrupt(unsigned pid){Task* t=g_task_list;if(t)do{if(t->pid==pid){t->interrupt_requested=true;t->state=TaskState::READY;return true;}t=t->next;}while(t!=g_task_list);return false;}
#include "../src/core/app_console.h"
static AppConsole* first;
static void provide_input(){console_input(first,'x');console_input(first,'\n');}
int main(){
    first=(AppConsole*)calloc(1,sizeof(AppConsole));auto* second=(AppConsole*)calloc(1,sizeof(AppConsole));first->refs=2;first->echo=true;second->refs=1;
    Task shell{1,TaskState::BLOCKED,nullptr},host{2,TaskState::RUNNING,nullptr},child{3,TaskState::BLOCKED,nullptr},peer{4,TaskState::BLOCKED,nullptr};
    shell.next=&host;host.next=&child;child.next=&peer;peer.next=&shell;g_task_list=&shell;
    host.console=child.console=first;host.console_host=true;peer.console=second;child.waiting_tty_input=peer.waiting_tty_input=shell.waiting_tty_input=true;
    g_current_task=&host;strcpy(host.launch_target,"/home/console.wiz");
    char bytes[128]{};assert(sys_console(0,(uint64_t)bytes,sizeof(bytes))==17);
    assert(!strcmp(bytes,"/home/console.wiz"));
    tty_wake_readers();assert(shell.state==TaskState::READY&&child.state==TaskState::BLOCKED&&peer.state==TaskState::BLOCKED);
    console_input(first,'a');console_input(first,'b');console_input(first,'\b');console_input(first,'c');console_input(first,'\n');
    assert(child.state==TaskState::READY&&peer.state==TaskState::BLOCKED);
    g_current_task=&child;assert(console_read(first,bytes,sizeof(bytes))==3&&!memcmp(bytes,"ac\n",3));
    child.state=TaskState::RUNNING;on_schedule=provide_input;assert(console_read(first,bytes,sizeof(bytes))==2&&!memcmp(bytes,"x\n",2));
    g_current_task=&host;assert(sys_console(1,(uint64_t)bytes,sizeof(bytes))>0);assert(second->output_head==second->output_tail);
    console_output(first,"one",3);console_output(second,"two",3);assert(sys_console(1,(uint64_t)bytes,sizeof(bytes))==3&&!memcmp(bytes,"one",3));
    first->raw=true;console_input(first,'r');g_current_task=&child;assert(console_read(first,bytes,sizeof(bytes))==1&&bytes[0]=='r');first->raw=false;
    g_current_task=&host;console_input(first,'e');console_input(first,'n');console_input(first,'d');sys_console(3,0,0);
    g_current_task=&child;assert(console_read(first,bytes,sizeof(bytes))==3&&!memcmp(bytes,"end",3));assert(console_read(first,bytes,sizeof(bytes))==0);
    assert(sys_console(1,(uint64_t)bytes,sizeof(bytes))==(uint64_t)-1);
    first->eof=false;g_current_task=&host;console_release(&host);assert(child.interrupt_requested&&freed==0);
    console_release(&child);assert(freed==1);console_release(&peer);assert(freed==2);
}
