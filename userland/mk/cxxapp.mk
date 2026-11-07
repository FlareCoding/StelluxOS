#
# Stellux Userland Generic C++ App Build Rules
#
# Include from an app Makefile after setting APP_NAME:
#   APP_NAME := myapp
#   include ../../mk/cxxapp.mk
#
# Handles both .c and .cpp sources. Links against libc++, libc++abi,
# and libunwind in addition to musl libc. Requires 'make libcxx' to
# have been run first. APP_QT := 1 builds the app on Qt, see qt.mk,
# APP_QT_QUICK := 1 builds it on Qt Quick as well, and APP_VULKAN := 1 links
# the app with the Vulkan library, see vulkan.mk.
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
CXX_SOURCES := $(wildcard $(SRC_DIR)/*.cpp)
C_OBJECTS   := $(C_SOURCES:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
CXX_OBJECTS := $(CXX_SOURCES:$(SRC_DIR)/%.cpp=$(BUILD_DIR)/%.o)
OBJECTS     := $(C_OBJECTS) $(CXX_OBJECTS)
TARGET      := $(BIN_DIR)/$(APP_NAME)

ifdef APP_QT_QUICK
APP_QT := 1
endif

# A Qt app compiles against the kit and links libstlxqpa whole, so the stellux
# platform plugin registers
ifdef APP_QT
include $(USERLAND_ROOT)/mk/qt.mk
APP_CXXFLAGS := $(QT_CPPFLAGS)
APP_LDLIBS   := -Wl,--whole-archive -lstlxqpa -Wl,--no-whole-archive -lstlxwin -lstlxconf $(QT_LDLIBS)
OBJECTS      += $(QT_MOC_OBJECTS) $(QT_RCC_OBJECTS)
LIB_ARCHIVES += $(SYSROOT)/lib/libstlxqpa.a $(SYSROOT)/lib/libstlxwin.a $(SYSROOT)/lib/libstlxconf.a
endif

# A Qt Quick app also compiles against Qt Quick, and links it with every QML
# module the qt recipe's probe program imports
ifdef APP_QT_QUICK
APP_CXXFLAGS += $(QT_QUICK_CPPFLAGS)
APP_LDLIBS   += $(QT_QUICK_LDLIBS)
endif

# A Vulkan app's C++ sources compile against the kit's headers, and the app
# links the Vulkan library
ifdef APP_VULKAN
include $(USERLAND_ROOT)/mk/vulkan.mk
APP_CXXFLAGS += $(VULKAN_CPPFLAGS)
APP_LDLIBS   += $(VULKAN_LIBS)
LIB_ARCHIVES += $(VULKAN_LIBRARY)
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

$(BUILD_DIR)/qrc_%.o: $(BUILD_DIR)/qrc_%.cpp
	@echo "[CXX] $< ($(ARCH))"
	$(UQ)$(CXX) $(CXXFLAGS_COMMON) $(APP_CXXFLAGS) -c $< -o $@
endif

-include $(OBJECTS:.o=.d) $(QT_RCC_OBJECTS:.o=.cpp.d)

clean:
	$(UQ)rm -rf build
	$(UQ)rm -f $(TARGET)

.PHONY: all clean
