#define _GNU_SOURCE
#include "cipher_lib.h"
#include <stdio.h> 
#include <stdlib.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/random.h>
#include <stdint.h>


#define BUFFER_SIZE 4096

#define WORKERS_COUNT 4  // максимум потоков в параллельном режиме

// Режимы работы
#define MODE_SEQUENTIAL 0
#define MODE_PARALLEL 1
#define MODE_AUTO 2

// Глобальные переменные
unsigned char* protected_key = NULL;
char* output_directory = NULL;
char** file_list = NULL;
int total_files = 0, next_file_index = 0, completed_files = 0;
volatile sig_atomic_t keep_running = 1;
static int img_global_fd = -1; 

struct statistics {
    double total_time;      // общее время выполнения
    double avg_time;        // среднее время на файл
    int files_processed;    // количество обработанных файлов
};

// Мьютексы
pthread_mutex_t file_list_mutex;
pthread_mutex_t counter_mutex;
pthread_mutex_t log_mutex;
pthread_mutex_t img_write_mutex = PTHREAD_MUTEX_INITIALIZER;

// Структура записи в образе (Задание 6) ===
#pragma pack(push, 1)
typedef struct {
    uint32_t file_size;
    uint32_t name_len;
    unsigned char salt[16];
} img_hdr_t;
#pragma pack(pop)

// === Вспомогательные функции для образов ===
static void gen_salt(unsigned char s[16]) {
    if(getrandom(s, 16, 0) != 16) {
        srand(time(NULL) ^ getpid());
        for(int i=0;i<16;i++) s[i] = rand() % 256;
    }
}

static int encrypt_rc4(unsigned char* data, size_t len, const char* key, const unsigned char* salt) {
    rc4_ctx_t* ctx = rc4_init((unsigned char*)key, strlen(key), salt, 16);
    if(!ctx) return -1;
    rc4_crypt(ctx, data, len);
    rc4_free(ctx);
    return 0;
}

// === Команда -add: добавление файлов в образ ===
static int img_add_one(const char* fpath, const char* entry, const char* key) {
    FILE* f = fopen(fpath, "rb"); if(!f) return -1;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char* data = malloc(sz); if(!data) { fclose(f); return -1; }
    if(fread(data, 1, sz, f) != (size_t)sz) { free(data); fclose(f); return -1; }
    fclose(f);
    
    unsigned char salt[16]; gen_salt(salt);
    encrypt_rc4(data, sz, key, salt);
    


    pthread_mutex_lock(&img_write_mutex);

    if (img_global_fd < 0) {
        pthread_mutex_unlock(&img_write_mutex);
        fprintf(stderr, "❌ Image file not opened\n");
        return -1;
    }

    off_t pos = lseek(img_global_fd, 0, SEEK_END);
    if (pos == (off_t)-1) {
        pthread_mutex_unlock(&img_write_mutex);
        return -1;
    }

    img_hdr_t h = {.file_size = (uint32_t)sz, .name_len = (uint32_t)strlen(entry)};
    memcpy(h.salt, salt, 16);

    if (write(img_global_fd, &h, sizeof(h)) != sizeof(h)) goto write_err;
    if (write(img_global_fd, entry, h.name_len) != (ssize_t)h.name_len) goto write_err;
    if (write(img_global_fd, data, sz) != (ssize_t)sz) goto write_err;

    pthread_mutex_unlock(&img_write_mutex);
    return 0;

    write_err:
    pthread_mutex_unlock(&img_write_mutex);
    perror("write to image");
    return -1;

    free(data); 
    printf("\r✓ %s", entry); 
    fflush(stdout);
    return 0;
}

typedef struct { 
    const char *key, *fpath, *entry; 
    int *res; 
} add_arg_t;
static void* add_worker(void* arg) {
    add_arg_t* a = (add_arg_t*)arg;
    *a->res = img_add_one(a->fpath, a->entry, a->key);
    return NULL;
}

// Рекурсивный сбор файлов из директории
static void collect_dir_files(const char* base_path, const char* current_rel, 
                              char*** files, int* cnt, int* cap) {
    char full_path[1024];
    
    if (current_rel[0] == '\0') {
        snprintf(full_path, sizeof(full_path), "%s", base_path);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", base_path, current_rel);
    }
    
    DIR* d = opendir(full_path);
    if (!d) return;

    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        
        char rel_path[1024];
        if (current_rel[0] == '\0') {
        snprintf(rel_path, sizeof(rel_path), "%s/%s", base_path, e->d_name);
        } else {
            snprintf(rel_path, sizeof(rel_path), "%s/%s/%s", base_path, current_rel, e->d_name);
        }

        char stat_path[2048];
        snprintf(stat_path, sizeof(stat_path), "%s/%s", full_path, e->d_name);
        
        struct stat st;
        if (stat(stat_path, &st) != 0) continue;
        
        if (S_ISREG(st.st_mode)) {
            if (*cnt >= *cap) {
                *cap *= 2;
                *files = realloc(*files, *cap * sizeof(char*));
            }
            (*files)[*cnt] = strdup(rel_path);
            (*cnt)++;
            printf("Добавлен файл: %s\n", rel_path);
        } else if (S_ISDIR(st.st_mode)) {
            char new_rel[1024];
            if (current_rel[0] == '\0') {
                snprintf(new_rel, sizeof(new_rel), "%s", e->d_name);
            } else {
                snprintf(new_rel, sizeof(new_rel), "%s/%s", current_rel, e->d_name);
            }
            collect_dir_files(base_path, new_rel, files, cnt, cap);
        }
    }
    closedir(d);
}

static int cmd_add(int argc, char** argv) {
    char *key=NULL,*img=NULL; int i=1;
    while(i<argc) {
        if(!strcmp(argv[i],"-key") && i+1<argc) key=argv[++i];
        else if(!strcmp(argv[i],"-image") && i+1<argc) img=argv[++i];
        i++;
    }
    if(!key||!img) { fprintf(stderr,"Usage: -add -key K -image I files...\n"); return 1; }
    
    int fd = open(img, O_RDWR|O_CREAT, 0644);
    if (fd < 0) {
        perror("Failed to create image file");
        return 1;
    }
    
    char** files = malloc(64*sizeof(char*)); int cnt=0, cap=64;
    for(i=1;i<argc;i++) {
        if(!strcmp(argv[i],"-key")||!strcmp(argv[i],"-image")) { i++; continue; }
        struct stat st; if(stat(argv[i],&st)) continue;
        
        if(S_ISREG(st.st_mode)) {
            if(cnt>=cap){cap*=2;files=realloc(files,cap*sizeof(char*));}
            files[cnt++] = strdup(argv[i]);
        } else if(S_ISDIR(st.st_mode)) {
        char base_with_dir[1024];
        snprintf(base_with_dir, sizeof(base_with_dir), "%s", argv[i]);
        collect_dir_files(base_with_dir, "", &files, &cnt, &cap);
        }
    }

    img_global_fd = open(img, O_RDWR | O_CREAT | O_APPEND, 0644);
    if (img_global_fd < 0) {
        perror("open image");
        for(i = 0; i < cnt; i++) free(files[i]);
        free(files);
        return 1;
    }
    
    pthread_t th[5]; int idx=0, active=0;
    add_arg_t args[cnt]; int results[cnt];
    
    while(idx<cnt) {
        while(active<5 && idx<cnt) {
            args[idx].key=key; 
            args[idx].fpath=files[idx];
            args[idx].entry=files[idx]; 
            args[idx].res=&results[idx];
            pthread_create(&th[active++], NULL, add_worker, &args[idx]);
            idx++;
        }
        for(int j=0;j<active;j++) pthread_join(th[j],NULL);
        active=0;
    }
    if (img_global_fd >= 0) {
    fsync(img_global_fd);  // гарантируем запись на диск
    close(img_global_fd);
    img_global_fd = -1;
    }
    printf("\n✅ Добавлено %d файлов в %s\n", cnt, img);
    for(i=0;i<cnt;i++) free(files[i]); 
    free(files);
    return 0;
}

// === Команда -list: список файлов в образе ===
struct file_entry { char name[512]; uint32_t size; };
int compare_entries(const void* a, const void* b) {
    return strcmp(((struct file_entry*)a)->name, ((struct file_entry*)b)->name);
}

static int cmd_list(const char* img) {
    int fd = open(img, O_RDONLY);
    if(fd<0) { perror("open"); return 1; }

    struct file_entry* entries = NULL;
    int count = 0, cap = 0;
    img_hdr_t h; 
    char name_buf[512];

    // Читаем все записи
    while(read(fd, &h, sizeof(h)) == sizeof(h)) {
        if(read(fd, name_buf, h.name_len) != h.name_len) break;
        name_buf[h.name_len] = '\0';
        
        if(count >= cap) {
            cap = cap ? cap*2 : 16;
            entries = realloc(entries, cap * sizeof(struct file_entry));
            if(!entries) {
                close(fd);
                return 1;
            }
        }
        strncpy(entries[count].name, name_buf, 511);
        entries[count].name[511] = '\0';
        entries[count].size = h.file_size;
        count++;
        
        lseek(fd, h.file_size, SEEK_CUR);
    }
    close(fd);

    // Сортируем
    if(count > 0) {
        qsort(entries, count, sizeof(struct file_entry), compare_entries);
    }

    printf("%-60s %12s\n", "FILE", "SIZE");
    printf("%-60s %12s\n", "----", "----");
    
    for(int i = 0; i < count; i++) {
        printf("%-60s %12u\n", entries[i].name, entries[i].size);
    }
    
    if(count == 0) {
        printf("(пусто)\n");
    }
    
    free(entries);
    return 0;
}

// === Команда -get: извлечение файла из образа ===
static int cmd_get(const char* img, const char* key, const char* entry, const char* out) {
    int fd = open(img, O_RDONLY); if(fd<0) { perror("open"); return 1; }
    img_hdr_t h; char name[512];
    while(read(fd,&h,sizeof(h))==sizeof(h)) {
        if(read(fd,name,h.name_len)!=h.name_len) break;
        name[h.name_len]=0;
        if(!strcmp(name, entry)) {
            unsigned char* data = malloc(h.file_size); if(!data) break;
            if(read(fd,data,h.file_size)!=h.file_size) { free(data); break; }
            encrypt_rc4(data, h.file_size, key, h.salt); // RC4 симметричен
            FILE* f = fopen(out,"wb"); if(f) { fwrite(data,1,h.file_size,f); fclose(f); }
            free(data); close(fd);
            printf("✅ Извлечён: %s → %s\n", entry, out); return 0;
        }
        lseek(fd, h.file_size, SEEK_CUR);
    }
    close(fd); fprintf(stderr,"❌ Файл не найден: %s\n", entry); return 1;
}

// Обработчик сигнала
void signal_handler(int sig) {
    keep_running = 0;
}

// Обработчик ошибки доступа к памяти
void segfault_handler(int sig) {
    fprintf(stderr, "\n❌ Ошибка безопасности: попытка записи в защищенную память!\n");
    exit(1);
}

// Инициализация защищенной памяти
void init_protected_memory() {
    protected_key = mmap(NULL, 16, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (protected_key == MAP_FAILED) {
        perror("mmap failed");
        exit(1);
    }
}

// Установка ключа в защищенную память
void set_protected_key(int key) {
    mprotect(protected_key, 16, PROT_READ | PROT_WRITE);
    memcpy(protected_key, &key, sizeof(int));
    mprotect(protected_key, 16, PROT_READ);
}

// Получение ключа из защищенной памяти
int get_protected_key() {
    int key;
    memcpy(&key, protected_key, sizeof(int));
    return key;
}

// Затирание и освобождение памяти
void destroy_protected_memory() {
    if (protected_key != NULL) {
        mprotect(protected_key, 16, PROT_READ | PROT_WRITE);
        memset(protected_key, 0, 16);
        munmap(protected_key, 16);
        protected_key = NULL;
    }
}

// Логирование
void write_log(const char* filename, const char* status, long thread_id) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 5;
    
    if (pthread_mutex_timedlock(&log_mutex, &ts) != 0) {
        printf("\n⚠️ Таймаут логирования!\n");
        return;
    }
    
    FILE* log = fopen("log.txt", "a");
    if (log) {
        time_t now = time(NULL);
        char* time_str = ctime(&now);
        time_str[strlen(time_str) - 1] = '\0';
        fprintf(log, "[%s] Поток %ld: %s - %s\n", time_str, thread_id, filename, status);
        fclose(log);
    }
    
    pthread_mutex_unlock(&log_mutex);
}

// Получить следующий файл
char* get_next_file(long thread_id) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 5;
    
    if (pthread_mutex_timedlock(&file_list_mutex, &ts) != 0) {
        write_log("WARNING: Deadlock", "TIMEOUT", thread_id);
        return NULL;
    }
    
    char* filename = NULL;
    if (next_file_index < total_files && keep_running) {
        filename = file_list[next_file_index];
        next_file_index++;
        write_log(filename, "STARTED", thread_id);
    }
    
    pthread_mutex_unlock(&file_list_mutex);
    return filename;
}

// Увеличить счетчик
void increment_counter(long thread_id) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 5;
    
    if (pthread_mutex_timedlock(&counter_mutex, &ts) == 0) {
        completed_files++;
        printf("\r📊 Прогресс: %d/%d файлов", completed_files, total_files);
        fflush(stdout);
        pthread_mutex_unlock(&counter_mutex);
    }
}

// Шифрование одного файла
int encrypt_single_file(const char* input_path, const char* output_path, int key, long thread_id) {   
    // Открыть входной файл
    FILE* input = fopen(input_path, "rb");
    if (!input) {
        write_log(input_path, "ERROR: cannot open input", thread_id);
        return -1;
    }
    
    char output_dir[1024];
    strcpy(output_dir, output_path);
    char* last_slash = strrchr(output_dir, '/');
    if (last_slash) {
        *last_slash = '\0';
        mkdir(output_dir, 0755);
    }

    // Открыть выходной файл
    FILE* output = fopen(output_path, "wb");
    if (!output) {
        write_log(input_path, "ERROR: cannot open output", thread_id);
        fclose(input);
        return -1;
    }
    
    // Установить ключ
    set_key(key);
    
    // Буферы
    char buffer[BUFFER_SIZE];
    size_t bytes_read;
    int error = 0;
    
    // Читаем, шифруем, пишем
    while (keep_running && (bytes_read = fread(buffer, 1, BUFFER_SIZE, input)) > 0) {
        caesar(buffer, buffer, bytes_read); 
        
        if (fwrite(buffer, 1, bytes_read, output) != bytes_read) {
            error = -1;
            break;
        }
    }
    
    fclose(input);
    fclose(output);
    return error;
}

// Поток-воркер
void* worker_thread(void* arg) {
    long thread_id = (long)arg;
    
    while (keep_running) {
        // 1. Взять следующий файл
        char* filename = get_next_file(thread_id);
        if (!filename) break;
        
        // 2. Сформировать пути
        char input_path[512];
        char output_path[512];
        snprintf(input_path, sizeof(input_path), "%s", filename);
        // Берем только имя файла без пути
        const char* basename = strrchr(filename, '/');
        if (basename) {
            snprintf(output_path, sizeof(output_path), "%s/%s", output_directory, basename + 1);
        } else {
            snprintf(output_path, sizeof(output_path), "%s/%s", output_directory, filename);
        }
        
        // 4. Шифровать файл
        int result = encrypt_single_file(input_path, output_path, get_protected_key(), thread_id);
        
        if (result == 0) {
            write_log(filename, "OK", thread_id);
            increment_counter(thread_id);
        } else {
            write_log(filename, "FAILED", thread_id);
        }
    }
    
    return NULL;
}

// Замер времени выполнения (в секундах с микросекундами)
double get_time() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}

// Функция для последовательной обработки
struct statistics run_sequential(char** files, int count, char* out_dir) {
    struct statistics stats;
    stats.files_processed = 0;
    
    double start_time = get_time();
    
    for (int i = 0; i < count && keep_running; i++) {
        char output_path[512];
        snprintf(output_path, sizeof(output_path), "%s/%s", out_dir, files[i]);
        
        double file_start = get_time();
        int result = encrypt_single_file(files[i], output_path, get_protected_key(), 0);
        double file_end = get_time();
        
        if (result == 0) {
            stats.files_processed++;
            // ВАЖНО: обновляем глобальный счетчик для правильного вывода
            pthread_mutex_lock(&counter_mutex);
            completed_files++;
            pthread_mutex_unlock(&counter_mutex);
            
            printf("[%d/%d] %s - %.3f сек\n", i+1, count, files[i], file_end - file_start);
        } else {
            printf("[%d/%d] %s - ОШИБКА\n", i+1, count, files[i]);
        }
    }
    
    double end_time = get_time();
    stats.total_time = end_time - start_time;
    stats.avg_time = (stats.files_processed > 0) ? 
                     stats.total_time / stats.files_processed : 0;
    
    return stats;
}

struct statistics run_parallel(char** files, int count, char* out_dir) {
    // Сохраняем глобальные настройки
    total_files = count;
    file_list = files;
    output_directory = out_dir;
    
    next_file_index = 0;
    completed_files = 0;
    
    double start_time = get_time();
    
    // Создаем WORKERS_COUNT потоков
    pthread_t threads[WORKERS_COUNT];
    for (int i = 0; i < WORKERS_COUNT; i++) {
        pthread_create(&threads[i], NULL, worker_thread, (void*)(long)i);
    }
    
    // Ждем завершения
    for (int i = 0; i < WORKERS_COUNT; i++) {
        pthread_join(threads[i], NULL);
    }
    
    double end_time = get_time();
    
    struct statistics stats;
    stats.total_time = end_time - start_time;
    stats.files_processed = completed_files;
    stats.avg_time = (stats.files_processed > 0) ? 
                     stats.total_time / stats.files_processed : 0;
    
    return stats;
}

void print_statistics(struct statistics stats, int mode_type, const char* mode_name) {
    printf("\n📊 СТАТИСТИКА (%s режим):\n", mode_name);
    printf("   ✅ Обработано файлов: %d\n", stats.files_processed);
    printf("   ⏱️  Общее время: %.3f сек\n", stats.total_time);
    printf("   📈 Среднее время на файл: %.3f сек\n", stats.avg_time);
}


int main(int argc, char* argv[]) {

    if(argc>1 && !strcmp(argv[1],"-add")) return cmd_add(argc,argv);
    if(argc>1 && !strcmp(argv[1],"-list")) {
        char *img = NULL;
        // Ищем флаг -image
        for(int i=2; i<argc; i++) {
            if(!strcmp(argv[i], "-image") && i+1 < argc) {
                img = argv[++i];
            }
        }
        if(!img) { fprintf(stderr,"Usage: -list -image disk.img\n"); return 1; }
        return cmd_list(img);
    }
    if(argc>1 && !strcmp(argv[1],"-get")) {
        char *img=NULL,*key=NULL,*out=NULL,*entry=NULL;
        for(int i=2;i<argc;i++) {
            if(!strcmp(argv[i],"-image")&&i+1<argc) img=argv[++i];
            else if(!strcmp(argv[i],"-key")&&i+1<argc) key=argv[++i];
            else if(!strcmp(argv[i],"-out")&&i+1<argc) out=argv[++i];
            else if(!entry) entry=argv[i];
        }
        if(!img||!key||!out||!entry) { fprintf(stderr,"Usage: -get -image I -key K -out O entry\n"); return 1; }
        return cmd_get(img,key,entry,out);
    }

    // 1. Проверить аргументы (минимум 3 файла + папка + ключ)
    if (argc < 4) {
        fprintf(stderr, "Использование: %s файл1 файл2 ... файлN выходная_папка ключ\n", argv[0]);
        fprintf(stderr, "Пример: %s f1.txt f2.txt f3.txt outdir/ 42\n", argv[0]);
        return 1;
    }
    
    // 2. Парсим аргументы
    int key_value = atoi(argv[argc - 1]);    // последний - ключ
    output_directory = argv[argc - 2];           // предпоследний - папка
    total_files = argc - 3;                      // остальные - файлы
    file_list = &argv[1];                        // указатель на первый файл
    
    printf("📁 Файлов для обработки: %d\n", total_files);
    printf("🔑 Ключ шифрования: %d\n", key_value);
    printf("📂 Выходная папка: %s\n", output_directory);
    
    // 3. Создать выходную директорию
    mkdir(output_directory, 0755);
    
    // 4. Инициализировать мьютексы
    pthread_mutex_init(&file_list_mutex, NULL);
    pthread_mutex_init(&counter_mutex, NULL);
    pthread_mutex_init(&log_mutex, NULL);
    
    // 5. Инициализировать глобальные переменные
    next_file_index = 0;
    completed_files = 0;
    
    // 8. Установить обработчик сигнала
    signal(SIGINT, signal_handler);
    
     // Установить обработчик SIGSEGV
    struct sigaction sa;
    sa.sa_handler = segfault_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGSEGV, &sa, NULL);
    
    // Инициализация защищенной памяти
    init_protected_memory();
    set_protected_key(key_value);

       // 9. Автовыбор режима
    int actual_mode = MODE_PARALLEL;  // по умолчанию параллельный
    
    // Проверяем аргумент --mode= (если есть)
    if (argc > 1 && strncmp(argv[1], "--mode=", 7) == 0) {
        if (strcmp(argv[1] + 7, "sequential") == 0) actual_mode = MODE_SEQUENTIAL;
        else if (strcmp(argv[1] + 7, "parallel") == 0) actual_mode = MODE_PARALLEL;
        else if (strcmp(argv[1] + 7, "auto") == 0) {
            actual_mode = (total_files < 5) ? MODE_SEQUENTIAL : MODE_PARALLEL;
            printf("🔧 Автовыбор: %s режим\n", (actual_mode == MODE_SEQUENTIAL) ? "последовательный" : "параллельный");
        }
    } else {
        // Без аргумента --mode= - автовыбор
        actual_mode = (total_files < 5) ? MODE_SEQUENTIAL : MODE_PARALLEL;
        printf("🔧 Автовыбор: %s режим\n", (actual_mode == MODE_SEQUENTIAL) ? "последовательный" : "параллельный");
    }
    
        struct statistics stats;
    
    double start_time = get_time();
    
    if (actual_mode == MODE_SEQUENTIAL) {
    stats = run_sequential(file_list, total_files, output_directory);
    } else {
        stats = run_parallel(file_list, total_files, output_directory);
    }
    
    double end_time = get_time();
    
    // 10. Вывести итог
    printf("\n\n✅ Готово! Скопировано %d из %d файлов\n", completed_files, total_files);
    printf("📝 Лог записан в log.txt\n");
    printf("⏱️ Время выполнения: %.3f сек\n", end_time - start_time);
    
    // Выводим статистику
    print_statistics(stats, actual_mode, (actual_mode == MODE_SEQUENTIAL) ? "последовательный" : "параллельный");
    
    // 12. Очистить мьютексы
    pthread_mutex_destroy(&file_list_mutex);
    pthread_mutex_destroy(&counter_mutex);
    pthread_mutex_destroy(&log_mutex);

    destroy_protected_memory();
    return 0;
}