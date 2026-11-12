#
# Stellux Userland libstlxhttp Host Tests
#
# Builds each *.test.c with `SOURCES` by the build machine's own compiler and runs it, for `make host-test`.
#

HOST_CC ?= clang

SRC_DIR     := ../src
BUILD_DIR   := ../build/host
BEARSSL_DIR := ../../libbearssl
SOURCES     := $(SRC_DIR)/reply.c $(SRC_DIR)/url.c
HEADERS     := $(wildcard ../include/stlxhttp/*.h ../include/stlxhttp/internal/*.h)
TESTS       := $(patsubst %.test.c,$(BUILD_DIR)/%,$(wildcard *.test.c))

CFLAGS := -std=c11 -O2 -g -Wall -Wextra -Werror -I../include

# macOS has getentropy where Linux has getrandom
ifeq ($(shell uname -s),Darwin)
HTTP_CFLAGS := '-Dgetrandom(buffer,size,flags)=(getentropy(buffer, size) == 0 ? (ssize_t)(size) : -1)'
endif

run: $(TESTS)
	@for test in $(TESTS); do echo "[RUN] $$test"; $$test || exit 1; done

# http.c also compiles against BearSSL's headers, whose calls its test stubs
$(BUILD_DIR)/http: http.test.c $(SRC_DIR)/http.c $(SOURCES) $(HEADERS)
	@mkdir -p $(BUILD_DIR)/include/bearssl
	@cp $(BEARSSL_DIR)/bearssl/inc/*.h $(BEARSSL_DIR)/trust_anchors.h $(BUILD_DIR)/include/bearssl/
	@echo "[CC]  $< (host)"
	@$(HOST_CC) $(CFLAGS) $(HTTP_CFLAGS) -I$(BUILD_DIR)/include $< $(SRC_DIR)/http.c $(SOURCES) -o $@

$(BUILD_DIR)/%: %.test.c $(SOURCES) $(HEADERS)
	@mkdir -p $(BUILD_DIR)
	@echo "[CC]  $< (host)"
	@$(HOST_CC) $(CFLAGS) $< $(SOURCES) -o $@

.PHONY: run
