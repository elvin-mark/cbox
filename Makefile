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

.PHONY: all clean test unit-test sanitizer re rootfs rootfs-alpine rootfs-busybox busybox-rootfs rootfs-all

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

rootfs: rootfs-alpine

rootfs-alpine:
	@ARCH=$$(uname -m); \
	case "$$ARCH" in \
		x86_64|aarch64) ;; \
		*) echo "Unsupported arch: $$ARCH"; exit 1 ;; \
	esac; \
	URL="https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/$$ARCH/alpine-minirootfs-$(ALPINE_VER)-$$ARCH.tar.gz"; \
	mkdir -p rootfs/alpine; \
	echo "Downloading $$URL ..."; \
	curl -fsSL "$$URL" | tar -xz -C rootfs/alpine; \
	echo "Alpine rootfs ready in ./rootfs/alpine"

busybox-rootfs: rootfs-busybox

rootfs-busybox: all
	@mkdir -p rootfs
	./$(TARGET) create-rootfs rootfs/busybox
	@BB=$$(command -v busybox || true); \
	if [ -n "$$BB" ] && file "$$BB" | grep -q "statically linked"; then \
		echo "Using host static busybox: $$BB"; \
		cp "$$BB" rootfs/busybox/bin/busybox; \
	else \
		ARCH=$$(uname -m); \
		URL="https://busybox.net/downloads/binaries/1.35.0-$$ARCH-linux-musl/busybox"; \
		echo "Downloading static busybox from $$URL ..."; \
		curl -fsSL "$$URL" -o rootfs/busybox/bin/busybox; \
	fi
	@chmod +x rootfs/busybox/bin/busybox
	@for applet in $$(./rootfs/busybox/bin/busybox --list); do \
		if [ "$$applet" != "busybox" ]; then \
			ln -sf /bin/busybox "rootfs/busybox/bin/$$applet"; \
		fi; \
	done
	@printf "root:x:0:0:root:/root:/bin/sh\n" > rootfs/busybox/etc/passwd
	@printf "root:x:0:\n" > rootfs/busybox/etc/group
	@echo "BusyBox rootfs ready in ./rootfs/busybox"

rootfs-all: rootfs-alpine rootfs-busybox
