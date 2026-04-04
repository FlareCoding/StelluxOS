#
# Stellux Userland - llama.cpp inference app
#
# CPU-only build of llama.cpp with ggml tensor library.
# Threading adapted for Stellux's native model.
#

USERLAND_ROOT ?= $(shell cd $(dir $(lastword $(MAKEFILE_LIST)))/../.. && pwd)
include $(USERLAND_ROOT)/mk/toolchain.mk

ifeq ($(wildcard $(SYSROOT)/lib/libc++.a),)
  $(error libc++ not found in sysroot. Run 'make libcxx' from the top-level directory first)
endif

APP_NAME  := llama
BUILD_DIR := build/$(ARCH)
BIN_DIR   := $(USERLAND_ROOT)/build/$(ARCH)/bin
TARGET    := $(BIN_DIR)/$(APP_NAME)

# Include paths
INCLUDES := \
	-Iggml/include \
	-Iinclude \
	-Isrc \
	-Iggml/src \
	-Iggml/src/ggml-cpu

# Defines
DEFINES := \
	-DNDEBUG \
	-DGGML_USE_CPU \
	-D_GNU_SOURCE \
	-DGGML_VERSION='"stellux"' \
	-DGGML_COMMIT='"stellux"' \
	-DGGML_ABORT_CALLBACK=1 \
	-DGGML_CPU_GENERIC=1

# Override warning flags: llama.cpp has some we can't fix upstream
WARN_FLAGS := -Wall -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function

# C sources
C_SOURCES := \
	ggml/src/ggml.c \
	ggml/src/ggml-alloc.c \
	ggml/src/ggml-quants.c \
	ggml/src/ggml-cpu/ggml-cpu.c \
	ggml/src/ggml-cpu/quants.c

# C++ sources
CXX_SOURCES := \
	ggml/src/ggml.cpp \
	ggml/src/ggml-backend.cpp \
	ggml/src/ggml-backend-reg.cpp \
	ggml/src/ggml-backend-dl.cpp \
	ggml/src/ggml-threading.cpp \
	ggml/src/gguf.cpp \
	ggml/src/ggml-opt.cpp \
	ggml/src/ggml-cpu/ggml-cpu.cpp \
	ggml/src/ggml-cpu/ops.cpp \
	ggml/src/ggml-cpu/vec.cpp \
	ggml/src/ggml-cpu/binary-ops.cpp \
	ggml/src/ggml-cpu/unary-ops.cpp \
	ggml/src/ggml-cpu/traits.cpp \
	ggml/src/ggml-cpu/repack.cpp \
	ggml/src/ggml-cpu/amx/amx.cpp \
	ggml/src/ggml-cpu/amx/mmq.cpp \
	src/llama.cpp \
	src/llama-model.cpp \
	src/llama-model-loader.cpp \
	src/llama-model-saver.cpp \
	src/llama-context.cpp \
	src/llama-mmap.cpp \
	src/llama-vocab.cpp \
	src/llama-arch.cpp \
	src/llama-batch.cpp \
	src/llama-graph.cpp \
	src/llama-sampler.cpp \
	src/llama-kv-cache.cpp \
	src/llama-kv-cache-iswa.cpp \
	src/llama-memory.cpp \
	src/llama-memory-recurrent.cpp \
	src/llama-memory-hybrid.cpp \
	src/llama-memory-hybrid-iswa.cpp \
	src/llama-impl.cpp \
	src/llama-hparams.cpp \
	src/llama-cparams.cpp \
	src/llama-io.cpp \
	src/llama-chat.cpp \
	src/llama-adapter.cpp \
	src/llama-grammar.cpp \
	src/llama-quant.cpp \
	src/unicode.cpp \
	src/unicode-data.cpp \
	$(wildcard src/models/*.cpp) \
	src/simple.cpp

C_OBJECTS   := $(C_SOURCES:%.c=$(BUILD_DIR)/%_c.o)
CXX_OBJECTS := $(CXX_SOURCES:%.cpp=$(BUILD_DIR)/%_cpp.o)
OBJECTS     := $(C_OBJECTS) $(CXX_OBJECTS)

# Override the strict flags from toolchain.mk
LOCAL_CFLAGS := \
	--target=$(TARGET_TRIPLE) \
	--sysroot=$(SYSROOT) \
	-nostdlibinc \
	-isystem $(SYSROOT)/include \
	-std=c11 -O2 -g \
	$(WARN_FLAGS) \
	-fno-stack-protector \
	$(INCLUDES) $(DEFINES)

LOCAL_CXXFLAGS := \
	--target=$(TARGET_TRIPLE) \
	--sysroot=$(SYSROOT) \
	-nostdlibinc \
	-isystem $(SYSROOT)/include/c++/v1 \
	-isystem $(SYSROOT)/include \
	-std=c++20 -O2 -g \
	$(WARN_FLAGS) \
	-fno-stack-protector \
	$(INCLUDES) $(DEFINES)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(UQ)mkdir -p $(dir $@)
	@echo "[LD]  $(APP_NAME) ($(ARCH))"
	$(UQ)$(CXX) -nostdlib -fuse-ld=lld --target=$(TARGET_TRIPLE) \
		-static -o $@ \
		$(SYSROOT)/lib/crt1.o $(SYSROOT)/lib/crti.o \
		$(OBJECTS) \
		-L$(SYSROOT)/lib \
		-Wl,--start-group \
		-lstlx -lstlxcxx -lc++ -lc++abi -lunwind -lc -lm $(BUILTINS_LIB) \
		-Wl,--end-group \
		$(SYSROOT)/lib/crtn.o

$(BUILD_DIR)/%_c.o: %.c
	$(UQ)mkdir -p $(dir $@)
	@echo "[CC]  $< ($(ARCH))"
	$(UQ)$(CC) $(LOCAL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/%_cpp.o: %.cpp
	$(UQ)mkdir -p $(dir $@)
	@echo "[CXX] $< ($(ARCH))"
	$(UQ)$(CXX) $(LOCAL_CXXFLAGS) -c $< -o $@

clean:
	$(UQ)rm -rf build
	$(UQ)rm -f $(TARGET)

.PHONY: all clean
