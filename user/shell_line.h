#ifndef WYZAR_SHELL_LINE_H
#define WYZAR_SHELL_LINE_H
// Length and insertion position are separate: editing never discards the suffix.
static int shell_line_insert(char* text,int capacity,int* length,int* cursor,char c){
    if(c<32||c>126||*length>=capacity-1)return 0;
    for(int i=*length;i>=*cursor;--i)text[i+1]=text[i];
    text[(*cursor)++]=c;++*length;return 1;
}
static int shell_line_backspace(char* text,int* length,int* cursor){
    if(!*cursor)return 0;
    --*cursor;--*length;
    for(int i=*cursor;i<=*length;++i)text[i]=text[i+1];
    return 1;
}
#endif
