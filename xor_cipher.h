#ifndef CAESAR_LIB_H  // чтобы файл не подключили дважды
#define CAESAR_LIB_H 

#ifdef __cplusplus  // Если компилятор C++ - вопспринимай как С
extern "C" {       
#endif

void set_key(char key);
void cipher(void* src, void* dst, int len);

#ifdef __cplusplus
} 
#endif

#endif 