CC = gcc
CFLAGS = -O3 -march=haswell -mtune=haswell -fopenmp -Wall

indexer: indexer.c
	$(CC) $(CFLAGS) -o $@ $< -lm

clean:
	rm -f indexer

.PHONY: clean