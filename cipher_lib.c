#include <stdlib.h>
#include <string.h>
#include "cipher_lib.h"
#include <sys/mman.h>

static char xor_key = 0;

void set_key(char key) { 
    xor_key = key; 
}

struct rc4_ctx {
    unsigned char S[256];
    int i, j;
};

void caesar(void* src, void* dst, int len) {
    unsigned char *s = (unsigned char*)src, *d = (unsigned char*)dst;
    for(int i = 0; i < len; i++) 
        d[i] = s[i] ^ xor_key;
}

rc4_ctx_t* rc4_init(const unsigned char* key, int key_len, const unsigned char* salt, int salt_len) {
    struct rc4_ctx* ctx = mmap(NULL, sizeof(struct rc4_ctx), 
                               PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ctx == MAP_FAILED) return NULL;
    
    unsigned char full_key[256];
    int full_len = key_len + salt_len;
    if(full_len > 256) full_len = 256;
    
    memcpy(full_key, key, key_len < 256 ? key_len : 256);
    if(salt_len > 0 && key_len < 256) {
        int salt_copy = (full_len - key_len < salt_len) ? (full_len - key_len) : salt_len;
        memcpy(full_key + key_len, salt, salt_copy);
    }
    
    for(int n = 0; n < 256; n++) 
        ctx->S[n] = n;
    
    int j = 0;
    for(int n = 0; n < 256; n++) {
        j = (j + ctx->S[n] + full_key[n % full_len]) & 0xFF;
        unsigned char tmp = ctx->S[n];
        ctx->S[n] = ctx->S[j];
        ctx->S[j] = tmp;
    }
    ctx->i = ctx->j = 0;
    mprotect(ctx, sizeof(struct rc4_ctx), PROT_NONE);
    return ctx;
}

void rc4_crypt(rc4_ctx_t* ctx, unsigned char* data, int len) {
    mprotect(ctx, sizeof(struct rc4_ctx), PROT_READ | PROT_WRITE);
    for(int n = 0; n < len; n++) {
        ctx->i = (ctx->i + 1) & 0xFF;
        ctx->j = (ctx->j + ctx->S[ctx->i]) & 0xFF;
        
        // Swap S[i] and S[j]
        unsigned char tmp = ctx->S[ctx->i];
        ctx->S[ctx->i] = ctx->S[ctx->j];
        ctx->S[ctx->j] = tmp;
        
        // Generate keystream byte
        unsigned char ks = ctx->S[(ctx->S[ctx->i] + ctx->S[ctx->j]) & 0xFF];
        data[n] ^= ks;
    }
    mprotect(ctx, sizeof(struct rc4_ctx), PROT_NONE);
}

void rc4_free(rc4_ctx_t* ctx_ptr) {
    struct rc4_ctx* ctx = (struct rc4_ctx*)ctx_ptr;
    if (ctx) {
        // Разрешаем запись для затирания
        mprotect(ctx, sizeof(struct rc4_ctx), PROT_READ | PROT_WRITE);
        memset(ctx, 0, sizeof(struct rc4_ctx));
        munmap(ctx, sizeof(struct rc4_ctx));
    }
}