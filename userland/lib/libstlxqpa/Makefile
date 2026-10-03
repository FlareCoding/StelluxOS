#
# Stellux Userland - libstlxqpa Static Library
#
# The stellux Qt platform plugin. Qt apps link it whole, which keeps the
# registration the plugin makes at startup.
#

USERLAND_ROOT ?= $(shell cd $(dir $(lastword $(MAKEFILE_LIST)))/../.. && pwd)
include $(USERLAND_ROOT)/mk/toolchain.mk

SRC_DIR     := src
BUILD_DIR   := build/$(ARCH)
LIB_NAME    := libstlxqpa.a
TARGET      := $(BUILD_DIR)/$(LIB_NAME)
SYSROOT_LIB := $(USERLAND_ROOT)/sysroot/$(ARCH)/lib

include $(USERLAND_ROOT)/mk/qt.mk

SOURCES := $(wildcard $(SRC_DIR)/*.cpp)
OBJECTS := $(SOURCES:$(SRC_DIR)/%.cpp=$(BUILD_DIR)/%.o) $(QT_MOC_OBJECTS)

# A static platform plugin, built on QtGui's private headers
QPA_CXXFLAGS := $(CXXFLAGS_COMMON) $(QT_CPPFLAGS) $(QT_PRIVATE_CPPFLAGS) -DQT_STATICPLUGIN

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(UQ)mkdir -p $(dir $@)
	@echo "[AR]  libstlxqpa.a ($(ARCH))"
	$(UQ)rm -f $@
	$(UQ)$(AR) crs $@ $(OBJECTS)
	$(UQ)mkdir -p $(SYSROOT_LIB)
	$(UQ)cp $@ $(SYSROOT_LIB)/

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	$(UQ)mkdir -p $(dir $@)
	@echo "[CXX] $< ($(ARCH))"
	$(UQ)$(CXX) $(QPA_CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/moc_%.o: $(BUILD_DIR)/moc_%.cpp
	@echo "[CXX] $< ($(ARCH))"
	$(UQ)$(CXX) $(QPA_CXXFLAGS) -c $< -o $@

-include $(OBJECTS:.o=.d)

clean:
	$(UQ)rm -rf build
	$(UQ)rm -f $(SYSROOT_LIB)/$(LIB_NAME)

.PHONY: all clean
