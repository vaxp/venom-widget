.PHONY: all widgets clean rebuild migrate-so install

CC ?= gcc

COMMON_CFLAGS  ?= -O2 -Wall -Wextra -fPIC
COMMON_LDFLAGS ?= -shared

BUILD_DIR := build
INSTALL_DIR ?= $(HOME)/.config/venom/widgets

WIDGET_SOURCES := \
	src/widgets/analog_clock.c \
	src/widgets/calendar.c \
	src/widgets/gpu_monitor.c \
	src/widgets/mpris-player.c \
	src/widgets/slideshow.c \
	src/widgets/sysmonitor-pro.c \
	src/widgets/weather.c

WIDGET_SOS := $(patsubst src/widgets/%.c,$(BUILD_DIR)/%.so,$(WIDGET_SOURCES))

all: widgets

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

widgets: $(WIDGET_SOS)

install: widgets
	@mkdir -p $(INSTALL_DIR)
	@install -m 755 $(WIDGET_SOS) $(INSTALL_DIR)/

$(BUILD_DIR)/analog_clock.so: PKGS = gtk+-3.0 cairo
$(BUILD_DIR)/analog_clock.so: EXTRA_LIBS = -lm

$(BUILD_DIR)/calendar.so: PKGS = gtk+-3.0

$(BUILD_DIR)/gpu_monitor.so: PKGS = gtk+-3.0 cairo
$(BUILD_DIR)/gpu_monitor.so: EXTRA_LIBS = -lm

$(BUILD_DIR)/mpris-player.so: PKGS = gtk+-3.0 gio-2.0
$(BUILD_DIR)/mpris-player.so: EXTRA_LIBS = -lm

$(BUILD_DIR)/slideshow.so: PKGS = gtk+-3.0 gdk-pixbuf-2.0
$(BUILD_DIR)/slideshow.so: EXTRA_LIBS = -lm

$(BUILD_DIR)/sysmonitor-pro.so: PKGS = gtk+-3.0
$(BUILD_DIR)/sysmonitor-pro.so: EXTRA_LIBS = -lm

$(BUILD_DIR)/weather.so: PKGS = gtk+-3.0 cairo
$(BUILD_DIR)/weather.so: EXTRA_LIBS = -lcurl -lcjson -lm

$(BUILD_DIR)/%.so: src/widgets/%.c | $(BUILD_DIR)
	$(CC) $(COMMON_CFLAGS) $(COMMON_LDFLAGS) -o $@ $< \
		$$(pkg-config --cflags $(PKGS)) \
		$$(pkg-config --libs $(PKGS)) \
		$(EXTRA_LIBS)

clean:
	@rm -f $(BUILD_DIR)/*.so

rebuild: clean widgets

migrate-so: | $(BUILD_DIR)
	@sh -c 'if ls src/widgets/*.so >/dev/null 2>&1; then mv -f src/widgets/*.so $(BUILD_DIR)/; fi'
