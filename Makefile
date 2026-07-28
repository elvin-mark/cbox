CC ?= gcc
CFLAGS ?= -Wall -Wextra -Wpedantic -Werror -D_GNU_SOURCE -std=c17 -Iinclude -g
LDFLAGS ?=

TARGET = toy-container
SRC_DIR = src
INC_DIR = include
BUILD_DIR = build

SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(SRCS))

.PHONY: all clean test sanitizer

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

sanitizer: CFLAGS += -fsanitize=address,undefined
sanitizer: re

re: clean all

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

test: all
	@echo "=== Running basic functionality tests ==="
	./$(TARGET) run -V -- /bin/echo "Hello from Toy Container!"
	./$(TARGET) run -V -n my-isolated-host -- /bin/hostname
	./$(TARGET) run -V -e APP_ENV=production -e FOO=BAR -- /bin/sh -c 'echo "ENV TEST: APP_ENV=$$APP_ENV FOO=$$FOO"'
	./$(TARGET) run -V -v /tmp:/tmp -- /bin/ls -la /tmp
	@echo "=== Tests completed successfully ==="
