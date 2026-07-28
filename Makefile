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
	./$(TARGET) run -v -- /bin/echo "Hello from Toy Container!"
	./$(TARGET) run -v -n my-isolated-host -- /bin/hostname
	./$(TARGET) run -v -- /bin/ps aux
	@echo "=== Tests completed successfully ==="
