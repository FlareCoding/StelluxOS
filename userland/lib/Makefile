#
# Stellux Userland - Libraries
#

LIB_DIRS := libstlx libstlxcxx libstlxconf libstlxgfx libstlxwin libstlxui libbearssl
LIB_COUNT := $(words $(LIB_DIRS))

all:
ifeq ($(STLX_QUIET),1)
	@echo "  Libraries:"
	@idx=0; anim_pid=; \
	trap 'kill $$anim_pid 2>/dev/null' EXIT INT TERM; \
	for lib in $(LIB_DIRS); do \
		idx=$$((idx + 1)); \
		( while kill -0 $$$$ 2>/dev/null; do \
			printf "\r    %-48s .  " "$$lib"; sleep 0.3; \
			printf "\r    %-48s .. " "$$lib"; sleep 0.3; \
			printf "\r    %-48s ..." "$$lib"; sleep 0.3; \
		done ) & \
		anim_pid=$$!; \
		if $(MAKE) --no-print-directory -C $$lib ARCH=$(ARCH) USERLAND_ROOT=$(USERLAND_ROOT) STLX_QUIET=1 > /dev/null 2>&1; then \
			kill $$anim_pid 2>/dev/null; wait $$anim_pid 2>/dev/null; \
			printf "\r    %-48s ok  (%d/%d)\n" "$$lib" $$idx $(LIB_COUNT); \
		else \
			kill $$anim_pid 2>/dev/null; wait $$anim_pid 2>/dev/null; \
			printf "\r    %-48s FAIL\n" "$$lib"; \
			exit 1; \
		fi; \
	done
else
	@for lib in $(LIB_DIRS); do \
		$(MAKE) -C $$lib ARCH=$(ARCH) USERLAND_ROOT=$(USERLAND_ROOT) V=$(V); \
	done
endif

clean:
	@for lib in $(LIB_DIRS); do \
		$(MAKE) -C $$lib clean ARCH=$(ARCH) USERLAND_ROOT=$(USERLAND_ROOT); \
	done

.PHONY: all clean
