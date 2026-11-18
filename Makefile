CC ?= gcc
CFLAGS ?= -Wall -Wextra -Wpedantic -Werror -D_GNU_SOURCE -std=c17 -Iinclude -g
LDFLAGS ?=

TARGET = cbox
SRC_DIR = src
INC_DIR = include
BUILD_DIR = build
TEST_BIN = $(BUILD_DIR)/test_unit

SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(SRCS))
LIB_OBJS = $(filter-out $(BUILD_DIR)/main.o, $(OBJS))

ALPINE_VER ?= 3.20.3

.PHONY: all clean test unit-test sanitizer re rootfs

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

unit-test: $(LIB_OBJS) tests/test_unit.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -fsanitize=address,undefined tests/test_unit.c $(LIB_OBJS) -o $(TEST_BIN) $(LDFLAGS)
	./$(TEST_BIN)

test: all unit-test
	@chmod +x tests/test_integration.sh
	./tests/test_integration.sh ./$(TARGET)

rootfs:
	@ARCH=$$(uname -m); \
	case "$$ARCH" in \
		x86_64|aarch64) ;; \
		*) echo "Unsupported arch: $$ARCH"; exit 1 ;; \
	esac; \
	URL="https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/$$ARCH/alpine-minirootfs-$(ALPINE_VER)-$$ARCH.tar.gz"; \
	mkdir -p rootfs; \
	echo "Downloading $$URL ..."; \
	curl -fsSL "$$URL" | tar -xz -C rootfs; \
	echo "Alpine rootfs ready in ./rootfs"
