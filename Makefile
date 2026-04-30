CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -O2
TARGET = sync_daemon

all: $(TARGET)

$(TARGET): sync_daemon.c
	$(CC) $(CFLAGS) -o $(TARGET) sync_daemon.c

clean:
	rm -f $(TARGET)

.PHONY: all clean
