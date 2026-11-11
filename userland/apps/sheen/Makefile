APP_NAME := sheen
APP_QT := 1

# Scintilla's Qt layer and the legacy code pages convert text through QTextCodec, which Qt 6 keeps in Core5Compat
APP_QT_MODULES := core5compat

include ../../mk/cxxapp.mk

# Scintilla and Lexilla compile from their release tarballs, unpacked unmodified, which packages/sheen pins
ifeq ($(filter clean,$(MAKECMDGOALS)),)
ifeq ($(and $(SCINTILLA_DIR),$(LEXILLA_DIR)),)
  $(error SCINTILLA_DIR and LEXILLA_DIR must name unpacked Scintilla and Lexilla releases, see packages/sheen)
endif
endif

SCINTILLA_QT_DIR := $(SCINTILLA_DIR)/qt/ScintillaEditBase

SCINTILLA_SOURCES     := $(wildcard $(SCINTILLA_DIR)/src/*.cxx)
SCINTILLA_QT_SOURCES  := $(addprefix $(SCINTILLA_QT_DIR)/,PlatQt.cpp ScintillaQt.cpp ScintillaEditBase.cpp)
SCINTILLA_MOC_HEADERS := ScintillaQt.h ScintillaEditBase.h
LEXILLA_SOURCES       := $(wildcard $(LEXILLA_DIR)/lexers/*.cxx $(LEXILLA_DIR)/lexlib/*.cxx) \
                         $(LEXILLA_DIR)/src/Lexilla.cxx

UPSTREAM_OBJECTS := \
	$(SCINTILLA_SOURCES:$(SCINTILLA_DIR)/%.cxx=$(BUILD_DIR)/scintilla/%.o) \
	$(SCINTILLA_QT_SOURCES:$(SCINTILLA_DIR)/%.cpp=$(BUILD_DIR)/scintilla/%.o) \
	$(SCINTILLA_MOC_HEADERS:%.h=$(BUILD_DIR)/scintilla/moc_%.o) \
	$(LEXILLA_SOURCES:$(LEXILLA_DIR)/%.cxx=$(BUILD_DIR)/lexilla/%.o)

UPSTREAM_INCLUDES := $(SCINTILLA_DIR)/include $(SCINTILLA_DIR)/src $(SCINTILLA_QT_DIR) \
                     $(LEXILLA_DIR)/include $(LEXILLA_DIR)/lexlib
UPSTREAM_DEFINES  := -DSCINTILLA_QT=1 -DEXPORT_IMPORT_API= -DNDEBUG=1

APP_CXXFLAGS += $(addprefix -isystem ,$(UPSTREAM_INCLUDES)) $(UPSTREAM_DEFINES)
OBJECTS += $(UPSTREAM_OBJECTS)

$(TARGET): $(UPSTREAM_OBJECTS)

# Upstream sources build as released, so their own warnings are left to them
$(BUILD_DIR)/scintilla/%.o: $(SCINTILLA_DIR)/%.cxx
	$(UQ)mkdir -p $(dir $@)
	@echo "[CXX] $(notdir $<) ($(ARCH))"
	$(UQ)$(CXX) $(CXXFLAGS_COMMON) $(APP_CXXFLAGS) -w -c $< -o $@

$(BUILD_DIR)/scintilla/%.o: $(SCINTILLA_DIR)/%.cpp
	$(UQ)mkdir -p $(dir $@)
	@echo "[CXX] $(notdir $<) ($(ARCH))"
	$(UQ)$(CXX) $(CXXFLAGS_COMMON) $(APP_CXXFLAGS) -w -c $< -o $@

$(BUILD_DIR)/lexilla/%.o: $(LEXILLA_DIR)/%.cxx
	$(UQ)mkdir -p $(dir $@)
	@echo "[CXX] $(notdir $<) ($(ARCH))"
	$(UQ)$(CXX) $(CXXFLAGS_COMMON) $(APP_CXXFLAGS) -w -c $< -o $@

$(BUILD_DIR)/scintilla/moc_%.cpp: $(SCINTILLA_QT_DIR)/%.h $(QT_MOC)
	$(UQ)mkdir -p $(dir $@)
	@echo "[MOC] $(notdir $<) ($(ARCH))"
	$(UQ)$(QT_MOC) $(QT_MOC_FLAGS) $(addprefix -I,$(UPSTREAM_INCLUDES)) $(UPSTREAM_DEFINES) $< -o $@

$(BUILD_DIR)/scintilla/moc_%.o: $(BUILD_DIR)/scintilla/moc_%.cpp
	@echo "[CXX] $(notdir $<) ($(ARCH))"
	$(UQ)$(CXX) $(CXXFLAGS_COMMON) $(APP_CXXFLAGS) -w -c $< -o $@

-include $(UPSTREAM_OBJECTS:.o=.d)
