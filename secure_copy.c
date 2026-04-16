#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <dlfcn.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>

#define BUFFER_SIZE 4096

#define WORKERS_COUNT 4  // максимум потоков в параллельном режиме

// Режимы работы
#define MODE_SEQUENTIAL 0
#define MODE_PARALLEL 1
#define MODE_AUTO 2



// Глобальные переменные
volatile sig_atomic_t keep_running = 1;

struct statistics {
    double total_time;      // общее время выполнения
    double avg_time;        // среднее время на файл
    int files_processed;    // количество обработанных файлов
};

// Мьютексы
pthread_mutex_t file_list_mutex;
pthread_mutex_t counter_mutex;
pthread_mutex_t log_mutex;

// Общие данные
char** file_list;
int total_files;
int next_file_index;
int completed_files;
char* output_directory;
int encryption_key;

// Функции из библиотеки
void (*set_key)(char);
void (*caesar)(void*, void*, int);

// Обработчик сигнала
void signal_handler(int sig) {
    keep_running = 0;
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
    
    // Читаем, шифруем, пишем
    while (keep_running && (bytes_read = fread(buffer, 1, BUFFER_SIZE, input)) > 0) {
        caesar(buffer, buffer, bytes_read);
        fwrite(buffer, 1, bytes_read, output);
    }
    
    fclose(input);
    fclose(output);
    
    return 0;
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
        snprintf(output_path, sizeof(output_path), "%s/%s", output_directory, filename);
        
        // 3. Замерить время
        clock_t start = clock();
        
        // 4. Шифровать файл
        int result = encrypt_single_file(input_path, output_path, encryption_key, thread_id);
        
        // 5. Записать в лог
        clock_t end = clock();
        double elapsed = (double)(end - start) / CLOCKS_PER_SEC;
        
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
struct statistics run_sequential(char** files, int count, char* out_dir, int key) {
    struct statistics stats;
    stats.files_processed = 0;
    
    double start_time = get_time();
    
    for (int i = 0; i < count && keep_running; i++) {
        char output_path[512];
        snprintf(output_path, sizeof(output_path), "%s/%s", out_dir, files[i]);
        
        double file_start = get_time();
        int result = encrypt_single_file(files[i], output_path, key, 0);
        double file_end = get_time();
        
        if (result == 0) {
            stats.files_processed++;
            completed_files++;
            printf("[%d/%d] %s - %.3f сек\n", 
                   i+1, count, files[i], file_end - file_start);
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

struct statistics run_parallel(char** files, int count, char* out_dir, int key) {
    // Сохраняем глобальные настройки
    total_files = count;
    file_list = files;
    output_directory = out_dir;
    encryption_key = key;
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
    // 1. Проверить аргументы (минимум 3 файла + папка + ключ)
    if (argc < 4) {
        fprintf(stderr, "Использование: %s файл1 файл2 ... файлN выходная_папка ключ\n", argv[0]);
        fprintf(stderr, "Пример: %s f1.txt f2.txt f3.txt outdir/ 42\n", argv[0]);
        return 1;
    }
    
    // 2. Парсим аргументы
    encryption_key = atoi(argv[argc - 1]);      // последний - ключ
    output_directory = argv[argc - 2];           // предпоследний - папка
    total_files = argc - 3;                      // остальные - файлы
    file_list = &argv[1];                        // указатель на первый файл
    
    printf("📁 Файлов для обработки: %d\n", total_files);
    printf("🔑 Ключ шифрования: %d\n", encryption_key);
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
    
    // 6. Загрузить библиотеку
    void* lib_handle = dlopen("./libcaesar.so", RTLD_LAZY);
    if (!lib_handle) {
        fprintf(stderr, "Ошибка загрузки библиотеки: %s\n", dlerror());
        return 1;
    }
    
    // 7. Получить функции
    set_key = dlsym(lib_handle, "set_key");
    caesar = dlsym(lib_handle, "caesar");
    
    if (!set_key || !caesar) {
        fprintf(stderr, "Ошибка получения функций: %s\n", dlerror());
        dlclose(lib_handle);
        return 1;
    }
    
    // 8. Установить обработчик сигнала
    signal(SIGINT, signal_handler);
    
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
        stats = run_sequential(file_list, total_files, output_directory, encryption_key);
    } else {
        stats = run_parallel(file_list, total_files, output_directory, encryption_key);
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
    
    // 13. Выгрузить библиотеку
    dlclose(lib_handle);
    
    return 0;

    
}