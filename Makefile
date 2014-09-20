all:
	gcc -ggdb -std=gnu99 src/test.c -I ./src/ -lpthread -o test.out
