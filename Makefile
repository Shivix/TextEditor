CC = gcc
CFLAGS = -std=c2x -Wall -Wextra -Wpedantic

SRCS = main.c
TARGET = editor

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(TARGET)
