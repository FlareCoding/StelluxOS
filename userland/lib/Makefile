#
# Stellux Userland - Libraries
#

LIB_DIRS := libstlx
LIB_COUNT := $(words $(LIB_DIRS))

all:
ifeq ($(STLX_QUIET),1)
	@echo "  Libraries:"
	@idx=0; \
	for lib in $(LIB_DIRS); do \
		idx=$$((idx + 1)); \
		if $(MAKE) --no-print-directory -C $$lib ARCH=$(ARCH) USERLAND_ROOT=$(USERLAND_ROOT) STLX_QUIET=1 > /dev/null 2>&1; then \
			printf "    %-48s ok  (%d/%d)\n" "$$lib" $$idx $(LIB_COUNT); \
		else \
			printf "    %-48s FAIL\n" "$$lib"; \
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
