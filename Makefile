CC = gcc
CFLAGS = -O3 -march=haswell -flto -fomit-frame-pointer -fno-plt \
         -fno-semantic-interposition -fno-trapping-math \
         -DNDEBUG -Wall -Wextra
LDFLAGS = -static -lm

SRCS = src/main.c src/config.c src/bridge.c src/http_server.c src/http_resp.c src/vectorizer.c
TARGET = rinha-server

all: $(TARGET)

$(TARGET): $(SRCS) src/bridge.h src/config.h src/http_server.h src/http_resp.h src/vectorizer.h
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
