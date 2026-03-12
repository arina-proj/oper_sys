#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <dlfcn.h>
#include <string.h>
#define BUFFER_SIZE 4096

volatile sig_atomic_t keep_running = 1;

struct shared_buffer {
    char data[BUFFER_SIZE];
    int size;
    int producer_done;
    pthread_mutex_t mutex;
    pthread_cond_t cond_producer;
    pthread_cond_t cond_consumer;
};

struct producer_args {
    struct shared_buffer* buf;
    FILE* input_file;
    long file_size;
    long* processed; 
    void(*set_key)(char);
    void(*cipher)(void*, void*, int);
    char key;
};

struct consumer_args {
    struct shared_buffer* buf;
    FILE* output_file;
};

void signal_handler(int sig) {
    keep_running = 0;
}

void update_progress(long current, long total) {
    if (total==0) return;
    int percent = current*100/total;
    printf("\r[");
    for (int i=0; i<50;i++){
        if(i<percent/2){
            printf("=");
        }
        else{
            printf(" ");
        }
    }
    printf("] %d%%", percent);
    fflush(stdout);
}

void* producer_thread(void* arg) {
    struct producer_args* args = (struct producer_args*)arg;
    struct shared_buffer* buf = args->buf;
    FILE* input = args->input_file;
    long total = args->file_size;
    long* processed = args->processed;
    void (*set_key)(char) = args->set_key;
    void (*cipher)(void*,void*,int) = args->cipher;
    char key = args->key;
    set_key(key);

    char temp_buf[BUFFER_SIZE];
    size_t bytes_read;
    
    while (!feof(input) && keep_running) {
        pthread_mutex_lock(&buf->mutex);
        while (buf->size > 0 && keep_running) {
            pthread_cond_wait(&buf->cond_producer, &buf->mutex);
        }
        if (!keep_running) {
            pthread_mutex_unlock(&buf->mutex);
            break;
        }
        bytes_read = fread(temp_buf, 1, BUFFER_SIZE, input);
        if (bytes_read == 0) {
            buf->producer_done = 1;
            pthread_mutex_unlock(&buf->mutex);
            break;
        }
        cipher(temp_buf, temp_buf, bytes_read);
        memcpy(buf->data, temp_buf, bytes_read);
        buf->size = bytes_read;
        *processed += bytes_read;
        update_progress(*processed, total);
        pthread_cond_signal(&buf->cond_consumer);
        pthread_mutex_unlock(&buf->mutex);
    }
    pthread_mutex_lock(&buf->mutex);
    buf->producer_done = 1;
    pthread_cond_signal(&buf->cond_consumer);
    pthread_mutex_unlock(&buf->mutex);
    return NULL;

}

void* consumer_thread(void* arg) {
    struct consumer_args* args = (struct consumer_args*)arg;
    struct shared_buffer* buf = args->buf;
    FILE* output = args->output_file;
    char temp_buf[BUFFER_SIZE];
    int bytes_to_write;
    while (keep_running){
        pthread_mutex_lock(&buf->mutex);
        while (buf->size == 0 && !buf->producer_done && keep_running) {
            pthread_cond_wait(&buf->cond_consumer, &buf->mutex);
        }
    
        if (!keep_running || (buf->producer_done && buf->size == 0)) {
            pthread_mutex_unlock(&buf->mutex);
            break;
        }
        
        bytes_to_write = buf->size;
        memcpy(temp_buf, buf->data, bytes_to_write);
        
        buf->size = 0;
        
        pthread_cond_signal(&buf->cond_producer);
        
        pthread_mutex_unlock(&buf->mutex);
        
        fwrite(temp_buf, 1, bytes_to_write, output);
    }

    return NULL;
}

int main(int argc, char* argv[]) {

    if (argc != 4) {
        fprintf(stderr, "Использование: %s <входной_файл> <выходной_файл> <ключ>\n", argv[0]);
        return 1;
    }
    

    char* input_filename = argv[1];
    char* output_filename = argv[2];
    int key = atoi(argv[3]);  // превращаем строку в число
    

    FILE* input_file = fopen(input_filename, "rb");
    if (!input_file) {
        perror("Ошибка открытия входного файла");
        return 1;
    }
    

    FILE* output_file = fopen(output_filename, "wb");
    if (!output_file) {
        perror("Ошибка открытия выходного файла");
        fclose(input_file);
        return 1;
    }
    

    fseek(input_file, 0, SEEK_END);
    long file_size = ftell(input_file);
    fseek(input_file, 0, SEEK_SET);
    

    void* lib_handle = dlopen("./libcaesar.so", RTLD_LAZY);
    if (!lib_handle) {
        fprintf(stderr, "Ошибка загрузки библиотеки: %s\n", dlerror());
        fclose(input_file);
        fclose(output_file);
        return 1;
    }
    

    void (*set_key)(char) = dlsym(lib_handle, "set_key");
    void (*caesar)(void*, void*, int) = dlsym(lib_handle, "caesar");
    
    if (!set_key || !caesar) {
        fprintf(stderr, "Ошибка получения функций: %s\n", dlerror());
        dlclose(lib_handle);
        fclose(input_file);
        fclose(output_file);
        return 1;
    }
    

    struct shared_buffer buffer;
    buffer.size = 0;
    buffer.producer_done = 0;
    pthread_mutex_init(&buffer.mutex, NULL);
    pthread_cond_init(&buffer.cond_producer, NULL);
    pthread_cond_init(&buffer.cond_consumer, NULL);
    
    long processed_bytes = 0;
    
    struct producer_args p_args;
    p_args.buf = &buffer;
    p_args.input_file = input_file;
    p_args.file_size = file_size;
    p_args.processed = &processed_bytes;
    p_args.set_key = set_key;
    p_args.cipher = caesar;
    p_args.key = key;
    
    struct consumer_args c_args;
    c_args.buf = &buffer;
    c_args.output_file = output_file;
    

    signal(SIGINT, signal_handler);
    

    pthread_t producer, consumer;
    pthread_create(&producer, NULL, producer_thread, &p_args);
    pthread_create(&consumer, NULL, consumer_thread, &c_args);
    

    pthread_join(producer, NULL);
    pthread_join(consumer, NULL);
    

    fclose(input_file);
    fclose(output_file);
    

    dlclose(lib_handle);
    

    printf("\nКопирование завершено!\n");
    return 0;
}