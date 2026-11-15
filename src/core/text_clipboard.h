#ifndef WYZAR_TEXT_CLIPBOARD_H
#define WYZAR_TEXT_CLIPBOARD_H
#include <stdint.h>
// In-memory desktop clipboard. Callers serialize access and validate user memory.
struct TextClipboard {
    static constexpr uint64_t capacity=65536;
    char bytes[capacity+1]{};
    uint64_t length=0;
    int64_t set(const char* input,uint64_t size){
        if(size>capacity||(!input&&size))return -1;
        for(uint64_t i=0;i<size;++i)if(!input[i])return -1;
        for(uint64_t i=0;i<size;++i)bytes[i]=input[i];bytes[size]=0;length=size;return 0;
    }
    int64_t get(char* output,uint64_t size)const{
        if(!output||size<=length)return -1;
        for(uint64_t i=0;i<=length;++i)output[i]=bytes[i];return (int64_t)length;
    }
};
#endif
