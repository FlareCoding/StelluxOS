#
# Stellux Userland - Applications
#

APP_DIRS := init hello shell ls cat rm stat touch sleep true false clear ptytest date \
			clockbench stlxdm stlxterm doom ping ifconfig nslookup arp udpecho tcpecho \
			fetch polltest dropbear blackjack wordle hangman snake tetris \
			grep wc head threadtest uname kill \
			echo hostname uptime whoami tee basename dirname
APP_COUNT := $(words $(APP_DIRS))

all:
ifeq ($(STLX_QUIET),1)
	@echo "  Applications:"
	@idx=0; \
	for app in $(APP_DIRS); do \
		idx=$$((idx + 1)); \
		( while true; do \
			printf "\r    %-48s .  " "$$app"; sleep 0.3; \
			printf "\r    %-48s .. " "$$app"; sleep 0.3; \
			printf "\r    %-48s ..." "$$app"; sleep 0.3; \
		done ) & \
		anim_pid=$$!; \
		if $(MAKE) --no-print-directory -C $$app ARCH=$(ARCH) USERLAND_ROOT=$(USERLAND_ROOT) STLX_QUIET=1 > /tmp/stlx-build-$$app.log 2>&1; then \
			kill $$anim_pid 2>/dev/null; wait $$anim_pid 2>/dev/null; \
			printf "\r    %-48s ok  (%d/%d)\n" "$$app" $$idx $(APP_COUNT); \
		else \
			kill $$anim_pid 2>/dev/null; wait $$anim_pid 2>/dev/null; \
			printf "\r    %-48s FAIL\n" "$$app"; \
			echo ""; \
			cat /tmp/stlx-build-$$app.log; \
			exit 1; \
		fi; \
	done
else
	@for app in $(APP_DIRS); do \
		$(MAKE) -C $$app ARCH=$(ARCH) USERLAND_ROOT=$(USERLAND_ROOT) V=$(V); \
	done
endif

clean:
	@for app in $(APP_DIRS); do \
		$(MAKE) -C $$app clean ARCH=$(ARCH) USERLAND_ROOT=$(USERLAND_ROOT); \
	done

.PHONY: all clean
