#ifndef WYZAR_TEXT_CLIPBOARD_H
#define WYZAR_TEXT_CLIPBOARD_H
#include <stdint.h>
// In-memory desktop clipboard. Callers serialize access and validate user memory.
struct TextClipboard {
    static constexpr uint64_t capacity=65536;
    static constexpr unsigned history_limit=12;
    // Reserve space for a new copy even if the user pins the rest of history.
    static constexpr unsigned pin_limit=history_limit-1;

    struct Entry { char bytes[capacity+1]; uint64_t length,hash; bool pinned; };
private:
    Entry entries[history_limit]{};
    unsigned order[history_limit]{};
    unsigned count=0;
    bool has_current=false;
public:
    unsigned history_size()const{return count;}
    const Entry& history(unsigned index)const{return entries[order[index]];}
    const char* data()const{return has_current?history(0).bytes:"";}
    unsigned pinned_count()const{
        unsigned pinned=0;
        for(unsigned i=0;i<count;++i)if(history(i).pinned)++pinned;
        return pinned;
    }
    bool toggle_pin(unsigned index){
        if(index>=count)return false;
        Entry& entry=entries[order[index]];
        if(!entry.pinned&&pinned_count()==pin_limit)return false;
        entry.pinned=!entry.pinned;
        return true;
    }
    bool erase(unsigned index){
        if(index>=count)return false;
        Entry& entry=entries[order[index]];
        entry.length=0;entry.pinned=false;
        for(unsigned i=index+1;i<count;++i)order[i-1]=order[i];
        --count;
        if(index==0)has_current=false;
        return true;
    }
    void clear_unpinned(){
        for(unsigned i=count;i>0;--i)if(!history(i-1).pinned)erase(i-1);
    }
    bool select(unsigned index){
        if(index>=count)return false;
        unsigned slot=order[index];
        for(unsigned i=index;i>0;--i)order[i]=order[i-1];
        order[0]=slot;has_current=true;return true;
    }
    int64_t set(const char* input,uint64_t size){
        if(size>capacity||(!input&&size))return -1;
        uint64_t hash=14695981039346656037ull;
        for(uint64_t i=0;i<size;++i){
            if(!input[i])return -1;
            hash=(hash^(uint8_t)input[i])*1099511628211ull;
        }
        if(!size){has_current=false;return 0;}
        for(unsigned i=0;i<count;++i){
            const Entry& entry=history(i);
            if(entry.length!=size||entry.hash!=hash)continue;
            uint64_t j=0;
            while(j<size&&entry.bytes[j]==input[j])++j;
            if(j==size){select(i);return 0;}
        }
        unsigned at=count;
        if(count<history_limit){
            unsigned slot=0;
            while(entries[slot].length)++slot;
            order[count++]=slot;
        }else{
            do{--at;}while(history(at).pinned);
        }
        select(at);
        Entry& entry=entries[order[0]];
        for(uint64_t i=0;i<size;++i)entry.bytes[i]=input[i];
        entry.bytes[size]=0;entry.length=size;entry.hash=hash;return 0;
    }
    int64_t get(char* output,uint64_t size)const{
        uint64_t length=has_current?history(0).length:0;
        if(!output||size<=length)return -1;
        const char* value=data();
        for(uint64_t i=0;i<=length;++i)output[i]=value[i];return (int64_t)length;
    }
};
#endif
