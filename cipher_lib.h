#ifndef CIPHER_LIB_H //если CIPHER_LIB_H НЕ определён
#define CIPHER_LIB_H // то определим его

#include <stddef.h>

#ifdef __cplusplus  // если компилятор C++
extern "C" {        // "используй C-совместимые имена"
#endif

typedef struct rc4_ctx rc4_ctx_t;

rc4_ctx_t* rc4_init(const unsigned char* key, int key_len, const unsigned char* salt, int salt_len);

void rc4_crypt(rc4_ctx_t* ctx, unsigned char* data, int len);

void rc4_free(rc4_ctx_t* ctx);

void set_key(char key);
void caesar(void* src, void* dst, int len);

#ifdef __cplusplus
}
#endif

#endif 