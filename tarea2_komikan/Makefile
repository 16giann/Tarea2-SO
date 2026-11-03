# Torneo de Komikan Concurrente - Tarea 2 SO
# Uso: make | make test | make tsan | make asan | make clean
CC     = gcc
# Banderas EXACTAS exigidas por la rubrica
CFLAGS = -Wall -Wextra -std=c17 -pthread

all: torneo

torneo: torneo.c
	$(CC) $(CFLAGS) -o $@ $<

# Ejecuta la suite de 12 pruebas (test_cases.txt)
test: torneo
	./torneo 12 4 test_cases.txt

# Detector de data races / deadlocks potenciales
tsan: torneo.c
	$(CC) -Wall -Wextra -std=c17 -O1 -g -pthread -fsanitize=thread -o torneo_tsan torneo.c
	./torneo_tsan 12 4 test_cases.txt

# Memoria + comportamiento indefinido (en Linux tambien detecta leaks)
asan: torneo.c
	$(CC) -Wall -Wextra -std=c17 -O1 -g -pthread -fsanitize=address,undefined -fno-omit-frame-pointer -o torneo_asan torneo.c
	./torneo_asan 12 4 test_cases.txt

clean:
	rm -f torneo torneo_tsan torneo_asan *.log
	rm -rf *.dSYM

.PHONY: all test tsan asan clean
