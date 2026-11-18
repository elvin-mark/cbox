CC ?= gcc
CFLAGS ?= -Wall -Wextra -Wpedantic -Werror -std=c17 -g

SRCS = $(wildcard *.c)
BINS = $(patsubst %.c, build/%, $(SRCS))

.PHONY: all clean

all: $(BINS)

build/%: %.c | build
	$(CC) $(CFLAGS) $< -o $@

build:
	mkdir -p build

clean:
	rm -rf build
