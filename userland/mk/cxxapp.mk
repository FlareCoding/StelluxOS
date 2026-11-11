#
# Stellux Userland Generic C++ App Build Rules
#
# Include from an app Makefile after setting APP_NAME:
#   APP_NAME := myapp
#   include ../../mk/cxxapp.mk
#
# Handles both .c and .cpp sources. Links against libc++, libc++abi,
# and libunwind in addition to musl libc. Requires 'make libcxx' to
# have been run first. APP_QT := 1 builds the app on Qt, see qt.mk, and
# APP_QT_MODULES := core5compat adds Qt's Core5Compat module to it.
# APP_VULKAN := 1 links the app with the Vulkan library, see vulkan.mk.
# APP_SOURCE_DIRS names subdirectories of src/ holding more C++ sources.
#

USERLAND_ROOT ?= $(shell cd $(dir $(lastword $(MAKEFILE_LIST)))/../.. && pwd)
include $(USERLAND_ROOT)/mk/toolchain.mk

# Verify libc++ sysroot exists
ifeq ($(wildcard $(SYSROOT)/lib/libc++.a),)
  $(error libc++ not found in sysroot. Run 'make libcxx' from the top-level directory first)
endif

SRC_DIR   := src
BUILD_DIR := build/$(ARCH)
BIN_DIR   := $(USERLAND_ROOT)/build/$(ARCH)/bin

APP_LIBS ?=
LIB_ARCHIVES := $(patsubst %,$(SYSROOT)/lib/lib%.a,stlx $(APP_LIBS))

C_SOURCES   := $(wildcard $(SRC_DIR)/*.c)
CXX_SOURCES := $(wildcard $(SRC_DIR)/*.cpp $(APP_SOURCE_DIRS:%=$(SRC_DIR)/%/*.cpp))
C_OBJECTS   := $(C_SOURCES:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
CXX_OBJECTS := $(CXX_SOURCES:$(SRC_DIR)/%.cpp=$(BUILD_DIR)/%.o)
OBJECTS     := $(C_OBJECTS) $(CXX_OBJECTS)
TARGET      := $(BIN_DIR)/$(APP_NAME)

# A Qt app compiles against the kit and links libstlxqpa whole, so the stellux
# platform plugin registers
ifdef APP_QT
include $(USERLAND_ROOT)/mk/qt.mk
APP_CXXFLAGS := $(QT_CPPFLAGS)
APP_LDLIBS   := -Wl,--whole-archive -lstlxqpa -Wl,--no-whole-archive -lstlxwin -lstlxconf $(QT_LDLIBS)
OBJECTS      += $(QT_MOC_OBJECTS)
LIB_ARCHIVES += $(SYSROOT)/lib/libstlxqpa.a $(SYSROOT)/lib/libstlxwin.a $(SYSROOT)/lib/libstlxconf.a

# Only an app asking for Core5Compat links it, so every other app links as it would without the module
ifneq ($(filter core5compat,$(APP_QT_MODULES)),)
  $(if $(QT_CORE5COMPAT_LDLIBS),,$(error The Qt kit in $(QT_DIR) has no Core5Compat, so packages.lock pins an older kit))
APP_CXXFLAGS += $(QT_CORE5COMPAT_CPPFLAGS)
APP_LDLIBS   += $(QT_CORE5COMPAT_LDLIBS)
endif
endif

# A Vulkan app's C++ sources compile against the kit's headers, and the app
# links the Vulkan library and libstlxwin, which the library calls
ifdef APP_VULKAN
include $(USERLAND_ROOT)/mk/vulkan.mk
APP_CXXFLAGS += $(VULKAN_CPPFLAGS)
APP_LDLIBS   += $(VULKAN_LIBS) -lstlxwin
LIB_ARCHIVES += $(VULKAN_LIBRARY) $(SYSROOT)/lib/libstlxwin.a
endif

# Each compute shader in shaders/ compiles to a header of SPIR-V words,
# which a source includes as <name>_spirv.h and names as <NAME>_SPIRV
SHADER_SOURCES := $(wildcard shaders/*.comp)
SHADER_HEADERS := $(SHADER_SOURCES:shaders/%.comp=$(BUILD_DIR)/shaders/%_spirv.h)
ifneq ($(SHADER_HEADERS),)
APP_CXXFLAGS += -I$(BUILD_DIR)/shaders
endif

all: $(TARGET)

$(TARGET): $(OBJECTS) $(LIB_ARCHIVES)
	$(UQ)mkdir -p $(dir $@)
	@echo "[LD]  $(APP_NAME) ($(ARCH))"
	$(UQ)$(CXX) -nostdlib -fuse-ld=lld --target=$(TARGET_TRIPLE) \
		-static -o $@ \
		$(SYSROOT)/lib/crt1.o $(SYSROOT)/lib/crti.o \
		$(OBJECTS) \
		-L$(SYSROOT)/lib \
		-Wl,--start-group \
		-lstlx $(addprefix -l,$(APP_LIBS)) $(APP_LDLIBS) -lc++ -lc++abi -lunwind -lc -lm $(BUILTINS_LIB) \
		-Wl,--end-group \
		$(SYSROOT)/lib/crtn.o

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	$(UQ)mkdir -p $(dir $@)
	@echo "[CC]  $< ($(ARCH))"
	$(UQ)$(CC) $(CFLAGS_COMMON) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	$(UQ)mkdir -p $(dir $@)
	@echo "[CXX] $< ($(ARCH))"
	$(UQ)$(CXX) $(CXXFLAGS_COMMON) $(APP_CXXFLAGS) -c $< -o $@

ifdef APP_QT
$(BUILD_DIR)/moc_%.o: $(BUILD_DIR)/moc_%.cpp
	@echo "[CXX] $< ($(ARCH))"
	$(UQ)$(CXX) $(CXXFLAGS_COMMON) $(APP_CXXFLAGS) -c $< -o $@
endif

# The headers exist before the first compile, whose dependency files then
# rebuild each source that includes one when its shader changes
$(OBJECTS): | $(SHADER_HEADERS)

$(BUILD_DIR)/shaders/%_spirv.h: shaders/%.comp
	$(UQ)mkdir -p $(dir $@)
	@echo "[GLSL] $< ($(ARCH))"
	$(UQ)$(STLX_GLSLANG) --quiet -V --target-env vulkan1.2 \
		--vn $$(echo $* | tr a-z A-Z)_SPIRV -o $@ $<

-include $(OBJECTS:.o=.d)

clean:
	$(UQ)rm -rf build
	$(UQ)rm -f $(TARGET)

.PHONY: all clean
