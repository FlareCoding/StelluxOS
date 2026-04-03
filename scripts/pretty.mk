#
# pretty.mk - Pretty build output with progress counter
#
# Usage:
#   include scripts/pretty.mk
#   
#   $(call compile_cxx,$<,$@)
#   $(call compile_asm,$<,$@)
#   $(call link_elf,$@)
#

# ============================================================================
# Verbosity Control
# ============================================================================
# V=0 (default): Quiet mode with pretty output
# V=1: Verbose mode showing full commands

ifeq ($(V),1)
  Q :=
  QUIET :=
else
  Q := @
  QUIET := --quiet
endif

# ============================================================================
# Color Support (auto-detected)
# ============================================================================

# Check if stdout is a terminal and supports colors
ifneq ($(TERM),)
  TPUT_COLORS := $(shell tput colors 2>/dev/null || echo 0)
  ifeq ($(shell test $(TPUT_COLORS) -ge 8 && echo yes),yes)
    COLOR_SUPPORT := 1
  endif
endif

ifdef COLOR_SUPPORT
  COLOR_RESET  := \033[0m
  COLOR_BOLD   := \033[1m
  COLOR_GREEN  := \033[32m
  COLOR_CYAN   := \033[36m
  COLOR_YELLOW := \033[33m
  COLOR_WHITE  := \033[37m
else
  COLOR_RESET  :=
  COLOR_BOLD   :=
  COLOR_GREEN  :=
  COLOR_CYAN   :=
  COLOR_YELLOW :=
  COLOR_WHITE  :=
endif

# ============================================================================
# Progress Counter
# ============================================================================
# Call reset_progress before building, then use $(call inc_progress) in rules

PROGRESS_CURRENT := 0
PROGRESS_TOTAL := 0

# Reset progress counter (call with total count)
# Usage: $(call reset_progress,$(words $(OBJECTS)))
define reset_progress
  $(eval PROGRESS_TOTAL := $(1))
  $(eval PROGRESS_CURRENT := 0)
endef

# Increment and return progress string "(N/M)"
# Usage: $(call inc_progress)
define inc_progress
$(eval PROGRESS_CURRENT := $(shell echo $$(($(PROGRESS_CURRENT) + 1))))$(PROGRESS_CURRENT)/$(PROGRESS_TOTAL)
endef

# ============================================================================
# Pretty Print Functions
# ============================================================================

# Print build header with configuration summary
# Usage: $(call print_header)
define print_header
	@printf "$(COLOR_BOLD)$(COLOR_WHITE)"
	@printf "  %-10s %s\n" "ARCH" "$(ARCH)"
	@printf "  %-10s %s\n" "MODE" "$(if $(RELEASE),release,debug)"
	@printf "  %-10s %s\n" "CXXFLAGS" "$(CXXFLAGS_DISPLAY)"
	@printf "$(COLOR_RESET)\n"
endef

# Pretty print for C++ compilation
# Usage: $(call print_cxx,source,progress)
define print_cxx
	@printf "  $(COLOR_GREEN)[CXX]$(COLOR_RESET)    %-45s $(COLOR_CYAN)(%s)$(COLOR_RESET)\n" "$(1)" "$(2)"
endef

# Pretty print for assembly
# Usage: $(call print_asm,source,progress)
define print_asm
	@printf "  $(COLOR_GREEN)[AS]$(COLOR_RESET)     %-45s $(COLOR_CYAN)(%s)$(COLOR_RESET)\n" "$(1)" "$(2)"
endef

# Pretty print for linking
# Usage: $(call print_ld,output,progress)
define print_ld
	@printf "  $(COLOR_YELLOW)[LD]$(COLOR_RESET)     %-45s $(COLOR_CYAN)(%s)$(COLOR_RESET)\n" "$(1)" "$(2)"
endef

# Pretty print for generic action
# Usage: $(call print_action,ACTION,target)
define print_action
	@printf "  $(COLOR_GREEN)[$(1)]$(COLOR_RESET)    %s\n" "$(2)"
endef

# ============================================================================
# Compile Helper Macros
# ============================================================================
# These combine pretty printing with actual compilation

# Compile C++ file
# Usage: $(call compile_cxx,source,output,progress)
define compile_cxx
	$(call print_cxx,$(1),$(3))
	$(Q)$(CXX) $(CXXFLAGS) -c $(1) -o $(2)
endef

# Compile assembly file  
# Usage: $(call compile_asm,source,output,progress)
define compile_asm
	$(call print_asm,$(1),$(3))
	$(Q)$(AS) $(ASFLAGS) -c $(1) -o $(2)
endef

# Link ELF binary
# Usage: $(call link_elf,output,objects,progress)
define link_elf
	$(call print_ld,$(1),$(3))
	$(Q)$(LD) $(LDFLAGS) -o $(1) $(2)
endef

# ============================================================================
# Utility Functions
# ============================================================================

# Create directory for target if it doesn't exist
# Usage: $(call ensure_dir,$@)
define ensure_dir
	$(Q)mkdir -p $(dir $(1))
endef
