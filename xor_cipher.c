#include <stdint.h>

static char cipher_key = 0;

void set_key(char key){
    cipher_key = key;
}

void caesar(void* src, void* dst, int len){

    char* source = (char*)src;
    char* target = (char*)dst;

    for(int i=0; i<len;i++){
        target[i] = source[i] ^ cipher_key;
    }
}