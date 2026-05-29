CC = gcc #имя компилятора - такое по заданию 
CFLAGS = -Wall -Wextra -pedantic -DWORKERS_COUNT=4 #Включить все основные предупреждения
#-Wextra	Включить дополнительные предупреждения
#-pedantic	Строгое соответствие стандарту C (не разрешает расширения)
#-DWORKERS_COUNT=4	Определяет макрос WORKERS_COUNT = 4 (как #define WORKERS_COUNT 4)
LDFLAGS = -pthread -lrt

all: secure_copy # главная цель - target

secure_copy: secure_copy.c cipher_lib.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o secure_copy secure_copy.c cipher_lib.c

test_secure: secure_copy
	@echo "🔐 Тестирование обратимости XOR шифрования..."
	@echo "Test data" > test_input.txt
	
	# Шифруем
	mkdir -p encrypted
	./secure_copy test_input.txt encrypted/ 42
	
	# Расшифровываем (XOR с тем же ключом)
	mkdir -p decrypted
	./secure_copy encrypted/test_input.txt decrypted/ 42
	
	# Проверяем (правильный путь!)
	@if diff -q test_input.txt decrypted/encrypted/test_input.txt > /dev/null 2>&1; then \
		echo "✅ Успех: XOR шифрование обратимо!"; \
	else \
		echo "❌ Ошибка: расшифровка не совпадает"; \
		exit 1; \
	fi
	
	rm -rf test_input.txt encrypted decrypted

testfile:
	dd if=/dev/urandom of=test_1mb.bin bs=1M count=1
# 	dd — утилита для копирования с преобразованием
# 	if=/dev/urandom — источник (случайные числа)
# 	of=test_1mb.bin — куда сохранить
# 	bs=1M — размер блока (1 мегабайт)
# 	count=1 — количество блоков (1)
	@echo "✅ Создан файл test_1mb.bin (1 МБ)"

clean:
	rm -f secure_copy              
	rm -f output.txt decrypted.txt  
	rm -f test_input.txt test_encrypted.txt test_decrypted.txt  
	rm -f test_1mb.bin test_enc.bin  
	rm -f log.txt
	rm -rf outdir/
	rm -f *.img
	@echo "🧹 Всё очищено!"

.PHONY: all clean test_secure testfile 


