#
# Stellux Userland Qt Build Rules
#
# Included after SRC_DIR and BUILD_DIR are set by a library or app built on Qt.
# The kit is the qt build package the userland build unpacks into the SDK, and
# its code generators come from this machine's host Qt, built by 'make qt'.
#

QT_DIR      := $(USERLAND_ROOT)/toolchain/sdk/$(ARCH)/qt
QT_HOST_DIR := $(USERLAND_ROOT)/toolchain/qt-host
QT_MOC      := $(QT_HOST_DIR)/libexec/moc
QT_RCC      := $(QT_HOST_DIR)/libexec/rcc

# Cleaning needs neither the kit nor the code generators
ifeq ($(filter clean,$(MAKECMDGOALS)),)

ifeq ($(wildcard $(QT_DIR)/share/stellux/qt.mk),)
  $(error Qt kit not found in $(QT_DIR). The userland build unpacks it, build from the top-level directory)
endif

include $(QT_DIR)/share/stellux/qt.mk

QT_HOST_MISSING := Qt $(QT_VERSION) host build not found in $(QT_HOST_DIR). Run 'make qt' from the top-level directory first
ifneq ($(shell cat $(QT_HOST_DIR)/VERSION 2>/dev/null),$(QT_VERSION))
  $(error $(QT_HOST_MISSING))
endif

ifneq ($(words $(wildcard $(QT_MOC) $(QT_RCC))),2)
  $(error $(QT_HOST_MISSING))
endif

endif

# moc reads the kit's headers to expand macros such as a plugin's IID, and takes
# no -isystem, so every include directory of the kit is passed as -I
QT_INCLUDE_DIRS := $(filter-out -%,$(QT_CPPFLAGS) $(QT_PRIVATE_CPPFLAGS))
QT_MOC_FLAGS := $(filter -D%,$(QT_CPPFLAGS)) $(addprefix -I,$(QT_INCLUDE_DIRS)) --no-notes

# moc runs on every header of SRC_DIR that declares a QObject
QT_MOC_HEADERS := $(shell grep -l Q_OBJECT $(SRC_DIR)/*.h 2>/dev/null)
QT_MOC_OBJECTS := $(QT_MOC_HEADERS:$(SRC_DIR)/%.h=$(BUILD_DIR)/moc_%.o)

$(BUILD_DIR)/moc_%.cpp: $(SRC_DIR)/%.h $(QT_MOC)
	$(UQ)mkdir -p $(dir $@)
	@echo "[MOC] $< ($(ARCH))"
	$(UQ)$(QT_MOC) $(QT_MOC_FLAGS) $< -o $@

# rcc compiles each .qrc file of SRC_DIR into the program, and its depfile names
# the files the .qrc lists, each also as an empty target, as -MP does for headers
QT_RCC_SOURCES := $(wildcard $(SRC_DIR)/*.qrc)
QT_RCC_OBJECTS := $(QT_RCC_SOURCES:$(SRC_DIR)/%.qrc=$(BUILD_DIR)/qrc_%.o)

$(BUILD_DIR)/qrc_%.cpp: $(SRC_DIR)/%.qrc $(QT_RCC)
	$(UQ)mkdir -p $(dir $@)
	@echo "[RCC] $< ($(ARCH))"
	$(UQ)$(QT_RCC) --name $* --depfile $@.d $< -o $@
	$(UQ)$(QT_RCC) --list $< | sed 's/$$/:/' >> $@.d

# Generated sources stay after a build, or the next make would remake them
.SECONDARY: $(QT_MOC_OBJECTS:.o=.cpp) $(QT_RCC_OBJECTS:.o=.cpp)
