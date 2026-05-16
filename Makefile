CC = gcc
CFLAGS = -O3 -march=haswell -mtune=haswell -flto -fomit-frame-pointer -fno-plt \
         -fno-semantic-interposition -fno-trapping-math \
         -DNDEBUG -Wall -Wextra
LDFLAGS = -static -flto -s -lm

SRCS = src/main.c src/config.c src/knn.c src/http_server.c src/http_resp.c src/vectorizer.c src/scm_rights.c src/perf.c
TARGET = rinha-server

all: $(TARGET)

$(TARGET): $(SRCS) src/knn.h src/config.h src/http_server.h src/http_resp.h src/vectorizer.h src/perf.h
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
