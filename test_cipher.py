import sys
import ctypes
import os

def main():

    if len(sys.argv) != 5:
        print("ОШИБКА: Неверное количество аргументов!")
        print("Использование: python3 test_cipher.py <библиотека> <ключ> <входной_файл> <выходной_файл>")
        sys.exit(1)

    lib_path = sys.argv[1]
    key = sys.argv[2]
    input_file = sys.argv[3]
    output_file = sys.argv[4]

    if not os.path.exists(lib_path):
        print(f"ОШИБКА: Библиотека '{lib_path}' не найдена!")
        print("Сначала выполни 'make' для компиляции библиотеки")
        sys.exit(1)

    if not os.path.exists(input_file):
        print(f"ОШИБКА: Входной файл '{input_file}' не найден!")
        print("Создай файл с тестовыми данными")
        sys.exit(1)

    if len(key) != 1:
        print(f"ОШИБКА: Ключ должен быть одним символом, а получено '{key}'")
        sys.exit(1)

    try:
        # ctypes.CDLL загружает динамическую библиотеку
        lib = ctypes.CDLL(lib_path)
        print(f"✅ Библиотека '{lib_path}' успешно загружена")
    except Exception as e:
        print(f"ОШИБКА: Не удалось загрузить библиотеку '{lib_path}'")
        print(f"Причина: {e}")
        sys.exit(1)

    #необходимо для загрузки с бибилиотеки в питон 
    lib.set_key.argtypes = [ctypes.c_char]
    lib.set_key.restype = None  # restype - возращаемое значение
    lib.cipher.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
    lib.cipher.restype = None  # функция ничего не возвращает

    key_byte = key.encode('utf-8')  # переводим строку в байт(char)
    lib.set_key(key_byte)
    print(f"🔑 Ключ установлен: '{key}' (код {ord(key)})")

    try:
        with open(input_file, 'rb') as f:  # 'rb' = read binary
            data = f.read()
        print(f"📖 Прочитано {len(data)} байт из '{input_file}'")
    except Exception as e:
        print(f"ОШИБКА: Не удалось прочитать файл '{input_file}'")
        print(f"Причина: {e}")
        sys.exit(1)

    if len(data) > 0:
        # Создаем буферы для исходных данных и результата
        src_buffer = ctypes.create_string_buffer(data)
        dst_buffer = ctypes.create_string_buffer(len(data))  
        src_ptr = ctypes.addressof(src_buffer)
        dst_ptr = ctypes.addressof(dst_buffer)
  
        lib.cipher(src_ptr, dst_ptr, len(data))
        
        result = dst_buffer.raw[:len(data)] # .raw - все байты (включая нулевые)

        try:
            with open(output_file, 'wb') as f:  # 'wb' = write binary
                f.write(result)
            print(f"💾 Результат сохранен в '{output_file}'")
        except Exception as e:
            print(f"ОШИБКА: Не удалось записать файл '{output_file}'")
            print(f"Причина: {e}")
            sys.exit(1)
    else:
        with open(output_file, 'wb') as f:
            pass
        print(f"⚠️ Входной файл пуст, создан пустой '{output_file}'")

    print("✅ Готово!")

if __name__ == "__main__":
    main()