CC = gcc #имя компилятора - такое по заданию 
CFLAGS = -Wall -Wextra -pedantic -fPIC #флаги компилятора(настройки) - прописаны в задании
TARGET = libcaesar.so # имя выходного файла
TEST_PROG = test_cipher.py # имя тестового файла
all: $(TARGET) # главная цель - target
$(TARGET): xor_cipher.c # -shared - динамическая
	$(CC) $(CFLAGS) -shared -o $(TARGET) xor_cipher.c 
install: # cp - копировать /.. - куда, ldconfig - обновление кеша библиотек
	cp $(TARGET) /usr/local/lib
	ldconfig
	@echo "✅ Библиотека установлена в /usr/local/lib/"
test: $(TARGET) input.txt
	python3 $(TEST_PROG) ./$(TARGET) A input.txt output.txt
	@echo "\n🔍 Исходный файл:"
	@cat input.txt
	@echo "\n🔐 Зашифрованный файл:"
	@cat output.txt
	
	@echo "\n🔄 Расшифровка:"
	python3 $(TEST_PROG) ./$(TARGET) A output.txt decrypted.txt
	@cat decrypted.txt
	@echo "\n✅ Тест завершен!"
SECURE_COPY = secure_copy

secure_copy: secure_copy.c
	gcc -pthread -Wall -o $(SECURE_COPY) secure_copy.c -ldl

test_secure: secure_copy
	@echo "🔐 Тестирование secure_copy..."
	@echo "Test data for secure_copy" > test_input.txt
	./secure_copy test_input.txt test_encrypted.txt 42
	@echo "✅ Зашифровано"
	./secure_copy test_encrypted.txt test_decrypted.txt 42
	@echo "✅ Расшифровано"
	@echo "Проверка:"
	diff test_input.txt test_decrypted.txt && echo "✅ Файлы совпадают!" || echo "❌ Ошибка!"

testfile:
	dd if=/dev/urandom of=test_1mb.bin bs=1M count=1
	@echo "✅ Создан файл test_1mb.bin (1 МБ)"

test_interrupt: secure_copy testfile
	@echo "Запусти программу и нажми Ctrl+C через пару секунд:"
	./secure_copy test_1mb.bin test_enc.bin 42
clean:
	rm -f $(TARGET)                # библиотека
	rm -f output.txt decrypted.txt  # файлы из test
	rm -f secure_copy               # программа
	rm -f test_input.txt test_encrypted.txt test_decrypted.txt  # тестовые файлы
	rm -f test_1mb.bin test_enc.bin  # файлы 1 МБ
	@echo "🧹 Всё очищено!"
.PHONY: all install test clean secure_copy test_secure testfile test_interrupt


