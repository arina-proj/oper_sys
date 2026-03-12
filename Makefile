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
clean:
	rm -f $(TARGET)       # удаляем библиотеку
	rm -f output.txt      # удаляем зашифрованный файл
	rm -f decrypted.txt   # удаляем расшифрованный файл
	@echo "🧹 Всё очищено!"
.PHONY: all install test clean